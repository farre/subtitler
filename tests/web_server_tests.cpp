#include <doctest/doctest.h>
#include <libsoup/soup.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "utils/paths.h"
#include "utils/preview_frame.h"
#include "utils/unique_ptr.h"
#include "web/json_helpers.h"
#include "web/web_server.h"

namespace {

template <typename T>
using GObjectPtr = subtitler::UniquePtr<T, g_object_unref>;

using BytesPtr = subtitler::UniquePtr<GBytes, g_bytes_unref>;

std::uint16_t FindFreePort() {
  GObjectPtr<GSocket> socket{g_socket_new(G_SOCKET_FAMILY_IPV4,
                                          G_SOCKET_TYPE_STREAM,
                                          G_SOCKET_PROTOCOL_TCP, nullptr)};
  REQUIRE(socket != nullptr);

  GObjectPtr<GSocketAddress> address{
      g_inet_socket_address_new_from_string("127.0.0.1", 0)};
  REQUIRE(g_socket_bind(socket.get(), address.get(), TRUE, nullptr));

  GObjectPtr<GSocketAddress> local{
      g_socket_get_local_address(socket.get(), nullptr)};
  return g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(local.get()));
}

std::string Url(std::uint16_t port, std::string_view path) {
  return std::format("http://127.0.0.1:{}{}", port, path);
}

std::shared_ptr<const std::vector<std::byte>> FakeJpeg(std::byte tag) {
  return std::make_shared<const std::vector<std::byte>>(
      std::initializer_list<std::byte>{std::byte{0xFF}, std::byte{0xD8},
                                       std::byte{0xFF}, tag, tag, tag,
                                       std::byte{0xFF}, std::byte{0xD9}});
}

struct Response {
  guint status = 0;
  std::string content_type;
  std::string body;
};

Response HttpRequest(std::string_view method, std::uint16_t port,
                     std::string_view path,
                     std::optional<std::string_view> body = std::nullopt) {
  GObjectPtr<SoupSession> session{soup_session_new()};
  GObjectPtr<SoupMessage> message{
      soup_message_new(std::string{method}.c_str(), Url(port, path).c_str())};

  if (body) {
    BytesPtr bytes{g_bytes_new(body->data(), body->size())};
    soup_message_set_request_body_from_bytes(
        message.get(), "application/octet-stream", bytes.get());
  }

  GObjectPtr<GInputStream> stream{
      soup_session_send(session.get(), message.get(), nullptr, nullptr)};

  Response response;

  if (stream == nullptr) {
    return response;
  }

  response.status = soup_message_get_status(message.get());

  if (const char* type = soup_message_headers_get_content_type(
          soup_message_get_response_headers(message.get()), nullptr)) {
    response.content_type = type;
  }

  for (;;) {
    BytesPtr chunk{
        g_input_stream_read_bytes(stream.get(), 16384, nullptr, nullptr)};

    if (chunk == nullptr || g_bytes_get_size(chunk.get()) == 0) {
      break;
    }

    response.body.append(
        static_cast<const char*>(g_bytes_get_data(chunk.get(), nullptr)),
        g_bytes_get_size(chunk.get()));
  }

  return response;
}

Response HttpGet(std::uint16_t port, std::string_view path) {
  return HttpRequest("GET", port, path);
}

// Writes request parts over a raw socket and reads the response status
// line: for declared lengths and chunked bodies the libsoup client API
// doesn't produce on demand. shutdown_write signals the request's end
// without a body (libsoup won't answer an incomplete declared body).
guint RawHttpRequest(std::uint16_t port,
                     const std::vector<std::string_view>& parts,
                     bool shutdown_write = false) {
  GObjectPtr<GSocketClient> client{g_socket_client_new()};
  g_socket_client_set_timeout(client.get(), 10);
  GObjectPtr<GSocketConnection> connection{g_socket_client_connect_to_host(
      client.get(), "127.0.0.1", port, nullptr, nullptr)};
  REQUIRE(connection != nullptr);

  GOutputStream* out =
      g_io_stream_get_output_stream(G_IO_STREAM(connection.get()));
  for (const auto part : parts) {
    REQUIRE(g_output_stream_write_all(out, part.data(), part.size(), nullptr,
                                      nullptr, nullptr));
  }

  if (shutdown_write) {
    GError* error = nullptr;
    REQUIRE(g_socket_shutdown(
        g_socket_connection_get_socket(connection.get()), FALSE, TRUE,
        &error));
  }

  GInputStream* in =
      g_io_stream_get_input_stream(G_IO_STREAM(connection.get()));
  std::string head;
  char buffer[4096];
  while (!head.contains("\r\n\r\n")) {
    const gssize read =
        g_input_stream_read(in, buffer, sizeof buffer, nullptr, nullptr);
    if (read <= 0) {
      break;
    }
    head.append(buffer, static_cast<std::size_t>(read));
  }

  unsigned status = 0;
  REQUIRE(std::sscanf(head.c_str(), "HTTP/%*c.%*c %u", &status) == 1);
  return status;
}

// RawHttpRequest for requests the server legitimately never answers
// (a truncated declared body is just disconnected); asserts the
// connection closes without a response.
void RawHttpRequestQuiet(std::uint16_t port,
                         const std::vector<std::string_view>& parts,
                         bool shutdown_write = false) {
  GObjectPtr<GSocketClient> client{g_socket_client_new()};
  g_socket_client_set_timeout(client.get(), 10);
  GObjectPtr<GSocketConnection> connection{g_socket_client_connect_to_host(
      client.get(), "127.0.0.1", port, nullptr, nullptr)};
  REQUIRE(connection != nullptr);

  GOutputStream* out =
      g_io_stream_get_output_stream(G_IO_STREAM(connection.get()));
  for (const auto part : parts) {
    REQUIRE(g_output_stream_write_all(out, part.data(), part.size(), nullptr,
                                      nullptr, nullptr));
  }

  if (shutdown_write) {
    GError* error = nullptr;
    REQUIRE(g_socket_shutdown(
        g_socket_connection_get_socket(connection.get()), FALSE, TRUE,
        &error));
  }

  GInputStream* in =
      g_io_stream_get_input_stream(G_IO_STREAM(connection.get()));
  char buffer[1024];
  const gssize read =
      g_input_stream_read(in, buffer, sizeof buffer, nullptr, nullptr);
  CHECK(read <= 0);  // closed without a response
}

// A held-open MJPEG connection.
struct MjpegClient {
  ~MjpegClient() { Close(); }

  guint Connect(std::uint16_t port) {
    message.reset(
        soup_message_new("GET", Url(port, "/api/preview.mjpeg").c_str()));
    stream.reset(
        soup_session_send(session.get(), message.get(), nullptr, nullptr));

    return stream != nullptr ? soup_message_get_status(message.get()) : 0;
  }

  void Close() {
    stream.reset();
    soup_session_abort(session.get());
  }

  GObjectPtr<SoupSession> session{soup_session_new()};
  GObjectPtr<SoupMessage> message;
  GObjectPtr<GInputStream> stream;
};

// Feeds frames like the real encoder does while the gate is open. The
// server detects disconnects only on write (an idle chunked response
// schedules no I/O), so disconnect assertions need frames flowing.
struct FrameFeeder {
  explicit FrameFeeder(subtitler::PreviewFrameBuffer& frames)
      : thread{[&frames](std::stop_token stop) {
          while (!stop.stop_requested()) {
            frames.Store(1000, FakeJpeg(std::byte{0x33}));
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
          }
        }} {}

  std::jthread thread;
};

bool WaitFor(const std::function<bool()>& condition) {
  for (int i = 0; i < 200; ++i) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  return false;
}

}  // namespace

TEST_CASE("web server preview endpoints") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  SUBCASE("preview.jpg is 503 while no frame was ever stored") {
    auto server = subtitler::WebServer::Create(port, frames);
    REQUIRE(server != nullptr);

    const auto response = HttpGet(port, "/api/preview.jpg");

    CHECK(response.status == SOUP_STATUS_SERVICE_UNAVAILABLE);
  }

  SUBCASE("preview.jpg serves the newest stored frame") {
    frames.Store(1000, FakeJpeg(std::byte{0x11}));

    auto server = subtitler::WebServer::Create(port, frames);
    REQUIRE(server != nullptr);

    const auto response = HttpGet(port, "/api/preview.jpg");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type == "image/jpeg");
    CHECK(response.body.size() == 8);
    CHECK(response.body.starts_with("\xFF\xD8\xFF\x11"));
  }

  SUBCASE("preview.mjpeg streams stored frames as multipart parts") {
    frames.Store(1000, FakeJpeg(std::byte{0x11}));

    auto server = subtitler::WebServer::Create(port, frames);
    REQUIRE(server != nullptr);

    GObjectPtr<GCancellable> cancellable{g_cancellable_new()};
    std::jthread watchdog{[&](std::stop_token stop) {
      for (int i = 0; i < 200 && !stop.stop_requested(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
      }
      g_cancellable_cancel(cancellable.get());
    }};

    GObjectPtr<SoupSession> session{soup_session_new()};
    GObjectPtr<SoupMessage> message{
        soup_message_new("GET", Url(port, "/api/preview.mjpeg").c_str())};

    GObjectPtr<GInputStream> stream{soup_session_send(
        session.get(), message.get(), cancellable.get(), nullptr)};
    REQUIRE(stream != nullptr);
    CHECK(soup_message_get_status(message.get()) == SOUP_STATUS_OK);

    const std::string content_type = soup_message_headers_get_one(
        soup_message_get_response_headers(message.get()), "Content-Type");
    CHECK(content_type.starts_with("multipart/x-mixed-replace"));
    CHECK(content_type.contains("boundary=frame"));

    // Read until the second frame shows up; the watchdog bounds the wait.
    std::string received;
    bool second_frame_seen = false;

    for (int parts = 0; !second_frame_seen;) {
      BytesPtr chunk{g_input_stream_read_bytes(stream.get(), 16384,
                                               cancellable.get(), nullptr)};

      if (chunk == nullptr) {
        break;
      }

      received.append(
          static_cast<const char*>(g_bytes_get_data(chunk.get(), nullptr)),
          g_bytes_get_size(chunk.get()));

      if (!received.contains("X-Frame-Sequence: 1")) {
        continue;
      }

      if (parts == 0) {
        // The first frame (stored before the client connected) arrived.
        parts = 1;
        frames.Store(2000, FakeJpeg(std::byte{0x22}));
      }

      second_frame_seen = received.contains("X-Frame-Sequence: 2");
    }

    watchdog.request_stop();

    INFO("received:\n", received);
    CHECK(received.contains("--frame"));
    CHECK(received.contains("Content-Type: image/jpeg"));
    CHECK(received.contains("X-Frame-Sequence: 1"));
    CHECK(received.contains("\xFF\xD8\xFF\x11"));
    CHECK(second_frame_seen);
    CHECK(received.contains("\xFF\xD8\xFF\x22"));

    g_input_stream_close(stream.get(), nullptr, nullptr);
    soup_session_abort(session.get());
  }
}

TEST_CASE("web server preview client limits and activation") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;
  frames.Store(1000, FakeJpeg(std::byte{0x11}));

  std::atomic_int activation_calls = 0;
  std::atomic_bool gate_active = false;

  subtitler::WebServerHooks hooks;
  hooks.preview_activation = [&](bool active) {
    gate_active.store(active);
    ++activation_calls;
  };
  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("activation toggles with the client count") {
    FrameFeeder feeder{frames};

    MjpegClient client;
    CHECK(client.Connect(port) == SOUP_STATUS_OK);

    CHECK(WaitFor([&] { return activation_calls.load() == 1; }));
    CHECK(gate_active.load());

    client.Close();

    CHECK(WaitFor([&] { return !gate_active.load(); }));
    CHECK(WaitFor([&] { return activation_calls.load() == 2; }));
  }

  SUBCASE("the fifth MJPEG client is rejected") {
    FrameFeeder feeder{frames};

    MjpegClient clients[4];

    for (auto& client : clients) {
      CHECK(client.Connect(port) == SOUP_STATUS_OK);
    }

    CHECK(WaitFor([&] { return gate_active.load(); }));

    const auto response = HttpGet(port, "/api/preview.mjpeg");
    CHECK(response.status == SOUP_STATUS_SERVICE_UNAVAILABLE);

    for (auto& client : clients) {
      client.Close();
    }

    CHECK(WaitFor([&] { return !gate_active.load(); }));
  }
}

TEST_CASE("web server static files") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  const auto root =
      std::filesystem::temp_directory_path() / "subtitler-static-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "sub");
  {
    std::ofstream{root / "index.html"} << "<!doctype html>\n<title>t</title>\n";
    std::ofstream{root / "app.js"} << "console.log('hi');\n";
    std::ofstream{root / "style.css"} << "body {}\n";
    std::ofstream{root / "logo.png"} << "\x89PNG";
    std::ofstream{root / "secret.txt"} << "nope\n";
    std::ofstream{root / "sub" / "page.html"} << "sub\n";
  }

  subtitler::WebServerHooks hooks;
  hooks.web_root = root;

  SUBCASE("the root path serves index.html") {
    auto server = subtitler::WebServer::Create(port, frames, hooks);
    REQUIRE(server != nullptr);

    const auto response = HttpGet(port, "/");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type.starts_with("text/html"));
    CHECK(response.body.contains("<title>t</title>"));
  }

  SUBCASE("allowlisted types are served with their MIME types") {
    auto server = subtitler::WebServer::Create(port, frames, hooks);
    REQUIRE(server != nullptr);

    const auto js = HttpGet(port, "/app.js");
    CHECK(js.status == SOUP_STATUS_OK);
    CHECK(js.content_type.starts_with("text/javascript"));
    CHECK(js.body.contains("console.log"));

    const auto css = HttpGet(port, "/style.css");
    CHECK(css.status == SOUP_STATUS_OK);
    CHECK(css.content_type.starts_with("text/css"));

    const auto png = HttpGet(port, "/logo.png");
    CHECK(png.status == SOUP_STATUS_OK);
    CHECK(png.content_type.starts_with("image/png"));
    CHECK(png.body.size() == 4);
  }

  SUBCASE("files in subdirectories are served") {
    auto server = subtitler::WebServer::Create(port, frames, hooks);
    REQUIRE(server != nullptr);

    const auto response = HttpGet(port, "/sub/page.html");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.body.contains("sub"));
  }

  SUBCASE("missing files and non-allowlisted types are 404") {
    auto server = subtitler::WebServer::Create(port, frames, hooks);
    REQUIRE(server != nullptr);

    CHECK(HttpGet(port, "/missing.html").status == SOUP_STATUS_NOT_FOUND);
    // Present on disk, but outside the allowlist.
    CHECK(HttpGet(port, "/secret.txt").status == SOUP_STATUS_NOT_FOUND);
  }

  SUBCASE("traversal attempts are 404") {
    auto server = subtitler::WebServer::Create(port, frames, hooks);
    REQUIRE(server != nullptr);

    // Encoded slashes are rejected by libsoup before routing.
    CHECK(HttpGet(port, "/..%2f..%2fetc%2fpasswd").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpGet(port, "/%2e%2e/%2e%2e/etc/passwd").status ==
          SOUP_STATUS_NOT_FOUND);
    CHECK(HttpGet(port, "/sub/../../etc/passwd").status ==
          SOUP_STATUS_NOT_FOUND);
    CHECK(HttpGet(port, "/%2e%2e%5csecret.txt").status ==
          SOUP_STATUS_NOT_FOUND);
  }

  SUBCASE("non-GET methods are 404") {
    auto server = subtitler::WebServer::Create(port, frames, hooks);
    REQUIRE(server != nullptr);

    CHECK(HttpRequest("POST", port, "/index.html", "x").status ==
          SOUP_STATUS_NOT_FOUND);
  }

  SUBCASE("no web root means no static files") {
    auto server = subtitler::WebServer::Create(port, frames);
    REQUIRE(server != nullptr);

    CHECK(HttpGet(port, "/").status == SOUP_STATUS_NOT_FOUND);
  }

  std::filesystem::remove_all(root);
}

TEST_CASE("web server subtitle upload") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  std::string captured_title;
  std::string captured_contents;
  subtitler::SubtitleUploadStatus next_status =
      subtitler::SubtitleUploadStatus::kStored;

  subtitler::WebServerHooks hooks;
  hooks.subtitle_upload = [&](std::string_view title,
                              std::string_view contents) {
    captured_title = title;
    captured_contents = contents;
    return subtitler::SubtitleUploadResult{
        next_status, next_status == subtitler::SubtitleUploadStatus::kStored
                         ? "m/My Movie.srt"
                         : ""};
  };
  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("a valid upload is stored and activated") {
    const auto response =
        HttpRequest("PUT", port, "/api/subtitles/My%20Movie.srt",
                    "1\n00:00:01,000 --> 00:00:02,000\nHi\n");

    CHECK(response.status == SOUP_STATUS_CREATED);
    CHECK(response.content_type == "application/json");
    CHECK(response.body == "{\"stored_name\":\"m/My Movie.srt\"}");
    CHECK(captured_title == "My Movie.srt");
    CHECK(captured_contents == "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
  }

  SUBCASE("an invalid title is a 400") {
    next_status = subtitler::SubtitleUploadStatus::kInvalidTitle;

    CHECK(HttpRequest("PUT", port, "/api/subtitles/no-extension", "x").status ==
          SOUP_STATUS_BAD_REQUEST);
  }

  SUBCASE("a storage or activation failure is a 500") {
    next_status = subtitler::SubtitleUploadStatus::kFailed;

    CHECK(HttpRequest("PUT", port, "/api/subtitles/movie.srt", "x").status ==
          SOUP_STATUS_INTERNAL_SERVER_ERROR);
  }

  SUBCASE("methods other than PUT are a 405") {
    CHECK(HttpGet(port, "/api/subtitles/movie.srt").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }

  SUBCASE("GET on the collection without a list hook is a 405") {
    CHECK(HttpGet(port, "/api/subtitles").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }

  SUBCASE("an empty body is a 400 and never reaches the handler") {
    CHECK(HttpRequest("PUT", port, "/api/subtitles/movie.srt", "").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(captured_title.empty());
  }

  SUBCASE("a missing title is a 400") {
    CHECK(HttpRequest("PUT", port, "/api/subtitles", "x").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitles/", "x").status ==
          SOUP_STATUS_BAD_REQUEST);
  }

  SUBCASE("an oversized body is a 413 and never reaches the handler") {
    const std::string huge(8 * 1024 * 1024 + 1, 'x');

    CHECK(HttpRequest("PUT", port, "/api/subtitles/movie.srt", huge).status ==
          SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
    CHECK(captured_title.empty());
  }

  // #8: a body with no declared length is bounded while streaming; the
  // 413 comes from counting received bytes, not from a header.
  SUBCASE("a chunked body crossing the cap is a 413") {
    const std::string chunk(1024 * 1024, 'x');  // 0x100000 bytes
    std::vector<std::string_view> parts{
        "PUT /api/subtitles/movie.srt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n",
    };
    const std::string chunk_head = std::format("{:x}\r\n", chunk.size());
    for (int i = 0; i < 9; ++i) {  // 9 MiB > the 8 MiB cap
      parts.push_back(chunk_head);
      parts.push_back(chunk);
      parts.push_back("\r\n");
    }
    parts.push_back("0\r\n\r\n");

    CHECK(RawHttpRequest(port, parts) == SOUP_STATUS_REQUEST_ENTITY_TOO_LARGE);
    CHECK(captured_title.empty());
  }
}

TEST_CASE("web server subtitle upload without a handler") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  auto server = subtitler::WebServer::Create(port, frames);
  REQUIRE(server != nullptr);

  CHECK(HttpRequest("PUT", port, "/api/subtitles/movie.srt", "x").status ==
        SOUP_STATUS_NOT_FOUND);
  CHECK(HttpGet(port, "/api/subtitles/movie.srt").status ==
        SOUP_STATUS_NOT_FOUND);
  CHECK(HttpRequest("DELETE", port, "/api/subtitles/movie.srt").status ==
        SOUP_STATUS_NOT_FOUND);
  CHECK(HttpGet(port, "/api/subtitles").status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpGet(port, "/api/subtitle-state").status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpRequest("PUT", port, "/api/subtitle-state?paused=true").status ==
        SOUP_STATUS_NOT_FOUND);
  CHECK(HttpGet(port, "/api/fonts").status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpGet(port, "/api/opensubtitles").status == SOUP_STATUS_NOT_FOUND);
}

TEST_CASE("web server opensubtitles") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  subtitler::WebServerHooks hooks;
  hooks.api_key = "test-api-key";

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("GET returns the API key as JSON") {
    const auto response = HttpGet(port, "/api/opensubtitles");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type == "application/json");
    CHECK(response.body == "{\"api_key\":\"test-api-key\"}");
  }

  SUBCASE("methods other than GET are a 405") {
    CHECK(HttpRequest("POST", port, "/api/opensubtitles", "x").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }

  SUBCASE("a longer path is a 404") {
    CHECK(HttpGet(port, "/api/opensubtitles/extra").status ==
          SOUP_STATUS_NOT_FOUND);
  }
}

TEST_CASE("web server opensubtitles key with special characters") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  subtitler::WebServerHooks hooks;
  hooks.api_key = "key\"with\\escapes";

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  const auto response = HttpGet(port, "/api/opensubtitles");

  CHECK(response.status == SOUP_STATUS_OK);
  CHECK(response.body == "{\"api_key\":\"key\\\"with\\\\escapes\"}");
}

TEST_CASE("web server subtitle list") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  subtitler::WebServerHooks hooks;
  hooks.subtitle_list = [] {
    return std::vector<std::string>{"Movie.srt", "Quote\"Back\\.srt"};
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("the library titles as a JSON array") {
    const auto response = HttpGet(port, "/api/subtitles");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type == "application/json");
    CHECK(response.body == "[\"Movie.srt\",\"Quote\\\"Back\\\\.srt\"]");
  }
}

TEST_CASE("web server subtitle get") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  std::string captured_title;
  subtitler::WebServerHooks hooks;
  hooks.subtitle_get =
      [&captured_title](std::string_view title) -> std::optional<std::string> {
    captured_title = title;
    if (title == "missing.srt") {
      return std::nullopt;
    }
    return std::string{"1\n00:00:01,000 --> 00:00:02,000\nHi \"there\"\n"};
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("the stored SRT as JSON") {
    const auto response = HttpGet(port, "/api/subtitles/Movie.srt");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type == "application/json");
    CHECK(response.body ==
          "{\"body\":\"1\\u000a00:00:01,000 --> 00:00:02,000\\u000aHi "
          "\\\"there\\\"\\u000a\"}");
    CHECK(captured_title == "Movie.srt");
  }

  SUBCASE("the title is percent-decoded") {
    const auto response = HttpGet(port, "/api/subtitles/My%20Movie.srt");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(captured_title == "My Movie.srt");
  }

  SUBCASE("an unknown title is a 404") {
    CHECK(HttpGet(port, "/api/subtitles/missing.srt").status ==
          SOUP_STATUS_NOT_FOUND);
  }
}

// Filenames travel the route exactly once-encoded: the client
// percent-encodes the segment, libsoup decodes the path once, and the
// hook sees the title byte-for-byte — `?`, `#`, `%`, literal `%20` or
// `%2F` text, spaces, quotes, and Unicode included (#2 review). Actual
// decoded separators and traversal still fail validation.
TEST_CASE("web server subtitle filenames round-trip") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  std::map<std::string, std::string> library;

  subtitler::WebServerHooks hooks;
  hooks.subtitle_upload =
      [&library](std::string_view title,
                 std::string_view contents) -> subtitler::SubtitleUploadResult {
    if (!subtitler::LibrarySubtitlePath(title)) {
      return {subtitler::SubtitleUploadStatus::kInvalidTitle, {}};
    }
    library[std::string{title}] = contents;
    return {subtitler::SubtitleUploadStatus::kStored,
            subtitler::LibrarySubtitlePath(title)->generic_string()};
  };
  hooks.subtitle_get = [&library](std::string_view title)
      -> std::optional<std::string> {
    const auto entry = library.find(std::string{title});
    if (entry == library.end()) {
      return std::nullopt;
    }
    return entry->second;
  };
  hooks.subtitle_delete = [&library](std::string_view title) {
    if (!subtitler::LibrarySubtitlePath(title)) {
      return subtitler::SubtitleDeleteStatus::kNotFound;
    }
    return library.erase(std::string{title}) > 0
               ? subtitler::SubtitleDeleteStatus::kDeleted
               : subtitler::SubtitleDeleteStatus::kNotFound;
  };
  hooks.subtitle_list = [&library] {
    std::vector<std::string> titles;
    for (const auto& [title, _] : library) {
      titles.push_back(title);
    }
    return titles;
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  // Percent-encodes like the client's encodeURIComponent: everything
  // outside the unreserved set, UTF-8 bytes included.
  const auto escape = [](std::string_view title) {
    const subtitler::UniquePtr<gchar, g_free> escaped{g_uri_escape_string(
        std::string{title}.c_str(), nullptr, FALSE)};
    return std::string{escaped.get()};
  };

  const std::vector<std::string> titles = {
      "Who?.srt", "C# Sharp.srt", "100%.srt", "a%20b.srt", "a%2Fb.srt",
      "Quote\"Test.srt", "München Éire ☃.srt", "plain space.srt",
  };

  for (const auto& title : titles) {
    const auto encoded = escape(title);
    INFO("title: ", title, " encoded: ", encoded);

    const std::string srt = "1\n00:00:01,000 --> 00:00:02,000\nHi\n";
    CHECK(HttpRequest("PUT", port, "/api/subtitles/" + encoded, srt).status ==
          SOUP_STATUS_CREATED);
    CHECK(library.contains(title));

    const auto got = HttpGet(port, "/api/subtitles/" + encoded);
    CHECK(got.status == SOUP_STATUS_OK);
    CHECK(got.body.contains("Hi"));

    const auto list = HttpGet(port, "/api/subtitles");
    CHECK(list.status == SOUP_STATUS_OK);
    CHECK(list.body.contains(subtitler::JsonEscape(title)));

    CHECK(HttpRequest("DELETE", port, "/api/subtitles/" + encoded).status ==
          SOUP_STATUS_NO_CONTENT);
    CHECK(!library.contains(title));
  }

  // A decoded slash from %2F is a separator, not a name character.
  CHECK(HttpRequest("PUT", port, "/api/subtitles/a%2Fb.srt", "x").status ==
        SOUP_STATUS_BAD_REQUEST);
  // Traversal, encoded and plain, stays rejected.
  CHECK(HttpRequest("PUT", port, "/api/subtitles/..%2F..%2Fetc.srt", "x")
            .status == SOUP_STATUS_BAD_REQUEST);
  CHECK(HttpRequest("PUT", port, "/api/subtitles/a/b.srt", "x").status ==
        SOUP_STATUS_BAD_REQUEST);
  CHECK(library.empty());
}

TEST_CASE("web server subtitle delete") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  std::string captured_title;
  subtitler::SubtitleDeleteStatus next_status =
      subtitler::SubtitleDeleteStatus::kDeleted;

  subtitler::WebServerHooks hooks;
  hooks.subtitle_delete = [&](std::string_view title) {
    captured_title = title;
    return next_status;
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("a deleted entry is a 204") {
    const auto response =
        HttpRequest("DELETE", port, "/api/subtitles/My%20Movie.srt");

    CHECK(response.status == SOUP_STATUS_NO_CONTENT);
    CHECK(captured_title == "My Movie.srt");
  }

  SUBCASE("an unknown title is a 404") {
    next_status = subtitler::SubtitleDeleteStatus::kNotFound;

    CHECK(HttpRequest("DELETE", port, "/api/subtitles/missing.srt").status ==
          SOUP_STATUS_NOT_FOUND);
  }

  SUBCASE("a removal failure is a 500") {
    next_status = subtitler::SubtitleDeleteStatus::kFailed;

    CHECK(HttpRequest("DELETE", port, "/api/subtitles/movie.srt").status ==
          SOUP_STATUS_INTERNAL_SERVER_ERROR);
  }

  SUBCASE("methods other than DELETE are a 405") {
    CHECK(HttpGet(port, "/api/subtitles/movie.srt").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
    CHECK(HttpRequest("PUT", port, "/api/subtitles/movie.srt", "x").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }

  SUBCASE("a missing title is a 400") {
    CHECK(HttpRequest("DELETE", port, "/api/subtitles").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("DELETE", port, "/api/subtitles/").status ==
          SOUP_STATUS_BAD_REQUEST);
  }
}

TEST_CASE("web server subtitle fonts") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  subtitler::WebServerHooks hooks;
  hooks.font_list = [] {
    return std::vector<std::string>{"Cantarell", "DejaVu Sans"};
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("the renderer-available families as a JSON array") {
    const auto response = HttpGet(port, "/api/fonts");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type == "application/json");
    CHECK(response.body == "[\"Cantarell\",\"DejaVu Sans\"]");
  }

  SUBCASE("methods other than GET are a 405") {
    CHECK(HttpRequest("PUT", port, "/api/fonts", "x").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }

  SUBCASE("a longer path is a 404") {
    CHECK(HttpGet(port, "/api/fonts/extra").status == SOUP_STATUS_NOT_FOUND);
  }
}

TEST_CASE("web server subtitle state") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  subtitler::SubtitleState state{
      {"Movie.srt"}, true, false, 1234, -150, {"Sans"}, {24}, {0xFF'FF'FF'FFu}};
  bool set_ok = true;

  subtitler::WebServerHooks hooks;
  hooks.subtitle_state_get = [&] { return state; };
  hooks.subtitle_state_set = [&](const subtitler::SubtitleStatePatch& patch) {
    if (!set_ok) {
      return false;
    }
    if (patch.file) {
      if (patch.file->empty()) {
        state.file = std::nullopt;
      } else {
        state.file = *patch.file;
      }
    }
    if (patch.visible) {
      state.visible = *patch.visible;
    }
    if (patch.paused) {
      state.paused = *patch.paused;
    }
    if (patch.time_ms) {
      state.time_ms = *patch.time_ms;
    }
    if (patch.delay_ms) {
      state.delay_ms = *patch.delay_ms;
    }
    if (patch.font_family) {
      state.font_family = *patch.font_family;
    }
    if (patch.font_size) {
      state.font_size = *patch.font_size;
    }
    if (patch.font_color) {
      state.font_color = *patch.font_color;
    }
    return true;
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("GET returns the state as JSON") {
    const auto response = HttpGet(port, "/api/subtitle-state");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type == "application/json");
    CHECK(response.body ==
          "{\"file\":\"Movie.srt\",\"visible\":true,\"paused\":false,"
          "\"time\":1234,\"delay\":-150,\"font_family\":\"Sans\","
          "\"font_size\":24,\"font_color\":\"#ffffff\"}");
  }

  SUBCASE("GET with an unset font has null font fields") {
    state.font_family = std::nullopt;
    state.font_size = std::nullopt;
    state.font_color = std::nullopt;

    const auto response = HttpGet(port, "/api/subtitle-state");

    CHECK(response.body.contains("\"font_family\":null"));
    CHECK(response.body.contains("\"font_size\":null"));
    CHECK(response.body.contains("\"font_color\":null"));
  }

  SUBCASE("GET with detached subtitles has a null file") {
    state.file = std::nullopt;

    const auto response = HttpGet(port, "/api/subtitle-state");

    CHECK(response.body.contains("\"file\":null"));
  }

  SUBCASE("PUT applies changes and answers the new state") {
    const auto response =
        HttpRequest("PUT", port, "/api/subtitle-state?paused=true&time=0");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(state.paused);
    CHECK(state.time_ms == 0);
    CHECK(response.body.contains("\"paused\":true"));
    CHECK(response.body.contains("\"time\":0"));
  }

  SUBCASE("PUT file switches and detaches") {
    CHECK(
        HttpRequest("PUT", port, "/api/subtitle-state?file=Other.srt").status ==
        SOUP_STATUS_OK);
    CHECK(state.file == std::optional<std::string>{"Other.srt"});

    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?file=").status ==
          SOUP_STATUS_OK);
    CHECK(state.file == std::nullopt);
  }

  SUBCASE("PUT applies font changes and answers the new state") {
    const auto response =
        HttpRequest("PUT", port,
                    "/api/subtitle-state?font_family=Serif&font_size=36&"
                    "font_color=%23ffd700");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(state.font_family == std::optional<std::string>{"Serif"});
    CHECK(state.font_size == std::optional<std::int64_t>{36});
    CHECK(state.font_color == std::optional<std::uint32_t>{0xFF'FF'D7'00u});
    CHECK(response.body.contains("\"font_family\":\"Serif\""));
    CHECK(response.body.contains("\"font_size\":36"));
    CHECK(response.body.contains("\"font_color\":\"#ffd700\""));
  }

  SUBCASE("PUT rejects bad values without touching the state") {
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?time=abc").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?paused=yes").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_size=0").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_size=-3").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(
        HttpRequest("PUT", port, "/api/subtitle-state?font_size=big").status ==
        SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_family=").status ==
          SOUP_STATUS_BAD_REQUEST);
    // font_color must be "#" plus six hex digits.
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_color=white")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_color=%23fff")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_color=%23gg0000")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?bogus=1").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK_FALSE(state.paused);
    CHECK(state.time_ms == 1234);
    CHECK(state.font_size == std::optional<std::int64_t>{24});
    CHECK(state.font_color == std::optional<std::uint32_t>{0xFF'FF'FF'FFu});
  }

  SUBCASE("PUT accepts the documented numeric ranges (#446)") {
    // The extremes of the ms range (~73 years either way) are valid;
    // negative time and delay keep their meaning.
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?time=2305843009213")
              .status == SOUP_STATUS_OK);
    CHECK(state.time_ms == 2305843009213LL);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?time=-2305843009213")
              .status == SOUP_STATUS_OK);
    CHECK(state.time_ms == -2305843009213LL);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?delay=-2305843009213")
              .status == SOUP_STATUS_OK);
    CHECK(state.delay_ms == -2305843009213LL);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_size=1000")
              .status == SOUP_STATUS_OK);
    CHECK(state.font_size == std::optional<std::int64_t>{1000});
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_size=1")
              .status == SOUP_STATUS_OK);
    CHECK(state.font_size == std::optional<std::int64_t>{1});
  }

  SUBCASE("PUT rejects out-of-range numbers without touching the state "
          "(#446)") {
    // Past the ms bound the stream's ns conversion could overflow.
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?time=2305843009214")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?time=-2305843009214")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(
        HttpRequest("PUT", port,
                    "/api/subtitle-state?time=9223372036854775807")
            .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(
        HttpRequest("PUT", port,
                    "/api/subtitle-state?time=-9223372036854775808")
            .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?delay=2305843009214")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(
        HttpRequest("PUT", port,
                    "/api/subtitle-state?delay=-9223372036854775808")
            .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?font_size=1001")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(
        HttpRequest("PUT", port,
                    "/api/subtitle-state?font_size=9223372036854775807")
            .status == SOUP_STATUS_BAD_REQUEST);

    CHECK(state.time_ms == 1234);
    CHECK(state.delay_ms == -150);
    CHECK(state.font_size == std::optional<std::int64_t>{24});
  }

  SUBCASE("a failing set hook is a 400") {
    set_ok = false;

    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?paused=true").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK_FALSE(state.paused);
  }

  SUBCASE("methods other than GET and PUT are a 405") {
    CHECK(HttpRequest("POST", port, "/api/subtitle-state").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }

  SUBCASE("a longer path is a 404") {
    CHECK(HttpGet(port, "/api/subtitle-state/extra").status ==
          SOUP_STATUS_NOT_FOUND);
  }
}

TEST_CASE("web server whisper endpoints") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  const auto state_dir =
      std::filesystem::temp_directory_path() / "subtitler-whisper-test";
  std::filesystem::remove_all(state_dir);
  std::filesystem::create_directories(state_dir / "models");
  {
    std::ofstream{state_dir / "models" / "ggml-tiny.en.bin"} << "fake-ggml";
  }

  bool enabled = false;
  std::optional<std::string> model;
  int model_clears = 0;

  subtitler::WebServerHooks hooks;
  hooks.state_dir = state_dir;
  hooks.whisper_state_get = [&] {
    return subtitler::WhisperRouteState{.enabled = enabled, .model = model};
  };
  hooks.whisper_state_set = [&](std::optional<bool> new_enabled,
                                std::optional<std::string_view> new_model) {
    // Only stored models are selectable, like the real hook resolves.
    if (new_model && *new_model != "ggml-tiny.en.bin") {
      return false;
    }
    if (new_enabled) {
      enabled = *new_enabled;
    }
    if (new_model) {
      model = std::string{*new_model};
    }
    return !enabled || model.has_value();
  };
  hooks.whisper_model_clear = [&] {
    ++model_clears;
    model = std::nullopt;
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("state round-trips through GET and PUT") {
    auto response = HttpGet(port, "/api/whisper");
    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.body ==
          R"({"enabled":false,"model":null,"models":["ggml-tiny.en.bin"]})");

    response = HttpRequest("PUT", port,
                           "/api/whisper?enabled=true&model=ggml-tiny.en.bin");
    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(
        response.body ==
        R"({"enabled":true,"model":"ggml-tiny.en.bin","models":["ggml-tiny.en.bin"]})");
    CHECK(enabled);
    CHECK(model == "ggml-tiny.en.bin");

    response = HttpRequest("PUT", port, "/api/whisper?enabled=false");
    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.body.contains(R"("enabled":false)"));
    CHECK_FALSE(enabled);
  }

  SUBCASE("bad state input is a 400") {
    CHECK(HttpRequest("PUT", port, "/api/whisper?enabled=maybe").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/whisper?model=ggml-missing.bin")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/whisper?model=../evil.bin").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/whisper?bogus=1").status ==
          SOUP_STATUS_BAD_REQUEST);
  }

  SUBCASE("methods other than GET and PUT are a 405") {
    CHECK(HttpRequest("POST", port, "/api/whisper").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }

  SUBCASE("model store saves the body and answers 201") {
    const auto response =
        HttpRequest("PUT", port, "/api/whisper/models/ggml-base.en.bin",
                    std::string_view{"fake-ggml-bytes"});
    CHECK(response.status == SOUP_STATUS_CREATED);
    CHECK(response.body == R"({"stored_name":"ggml-base.en.bin"})");

    std::ifstream file{state_dir / "models" / "ggml-base.en.bin",
                       std::ios::binary};
    CHECK(std::string{std::istreambuf_iterator<char>{file},
                      std::istreambuf_iterator<char>{}} == "fake-ggml-bytes");

    // The staged upload is gone once the store commits.
    CHECK(subtitler::ListWhisperModels(state_dir) ==
          std::vector<std::string>{"ggml-base.en.bin", "ggml-tiny.en.bin"});
    for (const auto& entry :
         std::filesystem::directory_iterator(state_dir / "models")) {
      CHECK(!entry.path().filename().string().starts_with(".upload-"));
    }
  }

  // #8: an excessive declared length is rejected at the headers:
  // nothing is staged. A full oversize body is discarded as it arrives
  // and answered 413 at completion; a truncated one just gets its
  // connection closed (libsoup never answers an incomplete declared
  // body).
  SUBCASE("a declared-oversize model is never staged") {
    const std::vector<std::string_view> parts{
        "PUT /api/whisper/models/ggml-huge.en.bin HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 536870913\r\n"  // 512 MiB + 1
        "Connection: close\r\n\r\n",
    };
    // The shutdown ends the request; the server closes silently.
    RawHttpRequestQuiet(port, parts, true);
    CHECK(!std::filesystem::exists(state_dir / "models" /
                                   "ggml-huge.en.bin"));
    for (const auto& entry :
         std::filesystem::directory_iterator(state_dir / "models")) {
      CHECK(!entry.path().filename().string().starts_with(".upload-"));
    }

    // The server is unaffected and still serves requests.
    const auto response =
        HttpRequest("PUT", port, "/api/whisper/models/ggml-base.en.bin",
                    std::string_view{"fake-ggml-bytes"});
    CHECK(response.status == SOUP_STATUS_CREATED);
  }

  SUBCASE("model store rejects bad input") {
    CHECK(HttpRequest("PUT", port, "/api/whisper/models/not-a-model").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/whisper/models/ggml-empty.bin",
                      std::string_view{})
              .status == SOUP_STATUS_BAD_REQUEST);
  }

  SUBCASE("model delete removes the entry") {
    CHECK(HttpRequest("DELETE", port, "/api/whisper/models/ggml-tiny.en.bin")
              .status == SOUP_STATUS_NO_CONTENT);
    CHECK_FALSE(
        std::filesystem::exists(state_dir / "models" / "ggml-tiny.en.bin"));

    const auto response = HttpGet(port, "/api/whisper");
    CHECK(response.body == R"({"enabled":false,"model":null,"models":[]})");
  }

  SUBCASE("model delete rejects bad input") {
    CHECK(HttpRequest("DELETE", port, "/api/whisper/models/ggml-missing.bin")
              .status == SOUP_STATUS_NOT_FOUND);
    CHECK(HttpRequest("DELETE", port, "/api/whisper/models/not-a-model")
              .status == SOUP_STATUS_BAD_REQUEST);
    CHECK(std::filesystem::exists(state_dir / "models" / "ggml-tiny.en.bin"));
  }

  SUBCASE("model delete refuses the model the tap is running") {
    enabled = true;
    model = "ggml-tiny.en.bin";

    const auto response =
        HttpRequest("DELETE", port, "/api/whisper/models/ggml-tiny.en.bin");
    CHECK(response.status == SOUP_STATUS_CONFLICT);
    CHECK(response.body == R"({"reason":"model in use"})");
    CHECK(std::filesystem::exists(state_dir / "models" / "ggml-tiny.en.bin"));

    // Selected but disabled doesn't block the delete.
    enabled = false;
    CHECK(HttpRequest("DELETE", port, "/api/whisper/models/ggml-tiny.en.bin")
              .status == SOUP_STATUS_NO_CONTENT);
  }

  SUBCASE("deleting the selected model clears the selection server-side") {
    model = "ggml-tiny.en.bin";

    CHECK(HttpRequest("DELETE", port, "/api/whisper/models/ggml-tiny.en.bin")
              .status == SOUP_STATUS_NO_CONTENT);
    CHECK(model_clears == 1);
    CHECK(model == std::nullopt);

    const auto response = HttpGet(port, "/api/whisper");
    CHECK(response.body == R"({"enabled":false,"model":null,"models":[]})");
  }

  SUBCASE("deleting another model keeps the selection") {
    model = "ggml-tiny.en.bin";
    {
      std::ofstream{state_dir / "models" / "ggml-base.en.bin"} << "fake";
    }

    CHECK(HttpRequest("DELETE", port, "/api/whisper/models/ggml-base.en.bin")
              .status == SOUP_STATUS_NO_CONTENT);
    CHECK(model_clears == 0);
    CHECK(model == "ggml-tiny.en.bin");
  }

  std::filesystem::remove_all(state_dir);
}

TEST_CASE("web server subtitle sync endpoints") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  subtitler::SubtitleSyncState state;
  subtitler::SubtitleSyncStartResult start_result =
      subtitler::SubtitleSyncStartResult::kStarted;
  int starts = 0;
  std::optional<std::string> started_model;

  subtitler::WebServerHooks hooks;
  hooks.subtitle_sync_get = [&] { return state; };
  hooks.subtitle_sync_start = [&](std::optional<std::string_view> model) {
    ++starts;
    started_model = model ? std::make_optional<std::string>(*model)
                          : std::nullopt;
    return start_result;
  };

  auto server = subtitler::WebServer::Create(port, frames, std::move(hooks));
  REQUIRE(server != nullptr);

  SUBCASE("PUT starts a session and GET answers the state") {
    state.status = subtitler::SubtitleSyncStatus::kListening;

    auto response = HttpRequest("PUT", port, "/api/subtitle-sync");
    CHECK(response.status == SOUP_STATUS_ACCEPTED);
    CHECK(response.body == R"({"state":"listening"})");
    CHECK(starts == 1);
    CHECK(started_model == std::nullopt);

    response = HttpGet(port, "/api/subtitle-sync");
    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.body == R"({"state":"listening"})");

    state.status = subtitler::SubtitleSyncStatus::kSynced;
    state.time_ms = 452300;
    response = HttpGet(port, "/api/subtitle-sync");
    CHECK(response.body == R"({"state":"synced","time":452300})");

    state.status = subtitler::SubtitleSyncStatus::kFailed;
    state.time_ms = std::nullopt;
    state.reason = "no stable match within the listening window";
    response = HttpGet(port, "/api/subtitle-sync");
    CHECK(
        response.body ==
        R"({"state":"failed","reason":"no stable match within the listening window"})");
  }

  SUBCASE("PUT passes the named model to the session") {
    const auto response =
        HttpRequest("PUT", port, "/api/subtitle-sync?model=ggml-tiny.en.bin");
    CHECK(response.status == SOUP_STATUS_ACCEPTED);
    CHECK(started_model == "ggml-tiny.en.bin");
  }

  SUBCASE("PUT rejects bad query input without starting") {
    CHECK(HttpRequest("PUT", port, "/api/subtitle-sync?model=").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-sync?bogus=1").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(starts == 0);
  }

  SUBCASE("start failures answer 409 with the reason") {
    start_result = subtitler::SubtitleSyncStartResult::kNoSubtitles;
    auto response = HttpRequest("PUT", port, "/api/subtitle-sync");
    CHECK(response.status == SOUP_STATUS_CONFLICT);
    CHECK(response.body ==
          R"({"state":"failed","reason":"no subtitles attached"})");

    start_result = subtitler::SubtitleSyncStartResult::kNoCapture;
    response = HttpRequest("PUT", port, "/api/subtitle-sync");
    CHECK(response.status == SOUP_STATUS_CONFLICT);
    CHECK(response.body ==
          R"({"state":"failed","reason":"capture isn't running"})");

    start_result = subtitler::SubtitleSyncStartResult::kNoWhisper;
    response = HttpRequest("PUT", port, "/api/subtitle-sync");
    CHECK(response.status == SOUP_STATUS_CONFLICT);
    CHECK(response.body ==
          R"({"state":"failed","reason":"whisper is disabled"})");

    start_result = subtitler::SubtitleSyncStartResult::kModelUnavailable;
    response = HttpRequest("PUT", port, "/api/subtitle-sync");
    CHECK(response.status == SOUP_STATUS_CONFLICT);
    CHECK(response.body ==
          R"({"state":"failed","reason":"the model isn't available"})");

    start_result = subtitler::SubtitleSyncStartResult::kUnparseableSubtitles;
    response = HttpRequest("PUT", port, "/api/subtitle-sync");
    CHECK(response.status == SOUP_STATUS_CONFLICT);
    CHECK(response.body ==
          R"({"state":"failed","reason":"the subtitle file can't be parsed"})");
  }

  SUBCASE("methods other than GET and PUT are a 405") {
    CHECK(HttpRequest("POST", port, "/api/subtitle-sync").status ==
          SOUP_STATUS_METHOD_NOT_ALLOWED);
  }
}

TEST_CASE("web server subtitle sync endpoints without hooks") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;
  auto server = subtitler::WebServer::Create(port, frames);
  REQUIRE(server != nullptr);

  CHECK(HttpGet(port, "/api/subtitle-sync").status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpRequest("PUT", port, "/api/subtitle-sync").status ==
        SOUP_STATUS_NOT_FOUND);
}

TEST_CASE("web server whisper endpoints without hooks") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;
  auto server = subtitler::WebServer::Create(port, frames);
  REQUIRE(server != nullptr);

  CHECK(HttpGet(port, "/api/whisper").status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpRequest("PUT", port, "/api/whisper?enabled=true").status ==
        SOUP_STATUS_NOT_FOUND);
  CHECK(HttpRequest("PUT", port, "/api/whisper/models/ggml-tiny.en.bin",
                    std::string_view{"fake"})
            .status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpRequest("DELETE", port, "/api/whisper/models/ggml-tiny.en.bin")
            .status == SOUP_STATUS_NOT_FOUND);
}

TEST_CASE("web server transcript stream") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;
  auto server = subtitler::WebServer::Create(port, frames);
  REQUIRE(server != nullptr);

  GObjectPtr<GCancellable> cancellable{g_cancellable_new()};
  std::jthread watchdog{[&](std::stop_token stop) {
    for (int i = 0; i < 200 && !stop.stop_requested(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    g_cancellable_cancel(cancellable.get());
  }};

  GObjectPtr<SoupSession> session{soup_session_new()};
  GObjectPtr<SoupMessage> message{soup_message_new(
      "GET", Url(port, "/api/whisper/transcript").c_str())};

  GObjectPtr<GInputStream> stream{soup_session_send(
      session.get(), message.get(), cancellable.get(), nullptr)};
  REQUIRE(stream != nullptr);
  CHECK(soup_message_get_status(message.get()) == SOUP_STATUS_OK);
  CHECK(soup_message_headers_get_one(
            soup_message_get_response_headers(message.get()),
            "Content-Type") == std::string_view{"text/event-stream"});

  server->PublishTranscript("hello world", 123456789);
  server->PublishTranscript("quotes \" and \\ slashes", 123456790);

  // Read until both events have fully arrived; the watchdog bounds the
  // wait. Waiting for the timestamp alone races a chunk split: the
  // marker can arrive before the rest of its event.
  std::string received;
  while (!received.contains("slashes\"}\n\n")) {
    BytesPtr chunk{g_input_stream_read_bytes(stream.get(), 16384,
                                             cancellable.get(), nullptr)};

    if (chunk == nullptr) {
      break;
    }

    received.append(
        static_cast<const char*>(g_bytes_get_data(chunk.get(), nullptr)),
        g_bytes_get_size(chunk.get()));
  }

  watchdog.request_stop();

  INFO("received:\n", received);
  CHECK(received.contains(
      "data: {\"timestamp_ns\":123456789,\"text\":\"hello world\"}\n\n"));
  CHECK(received.contains(
      "data: {\"timestamp_ns\":123456790,\"text\":\"quotes \\\" and "
      "\\\\ slashes\"}\n\n"));

  g_input_stream_close(stream.get(), nullptr, nullptr);
  soup_session_abort(session.get());
}
