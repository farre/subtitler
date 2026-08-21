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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "utils/preview_frame.h"
#include "utils/unique_ptr.h"
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
    CHECK(response.body == "m/My Movie.srt");
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
}

TEST_CASE("web server subtitle upload without a handler") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  auto server = subtitler::WebServer::Create(port, frames);
  REQUIRE(server != nullptr);

  CHECK(HttpRequest("PUT", port, "/api/subtitles/movie.srt", "x").status ==
        SOUP_STATUS_NOT_FOUND);
  CHECK(HttpGet(port, "/api/subtitles").status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpGet(port, "/api/subtitle-state").status == SOUP_STATUS_NOT_FOUND);
  CHECK(HttpRequest("PUT", port, "/api/subtitle-state?paused=true").status ==
        SOUP_STATUS_NOT_FOUND);
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

TEST_CASE("web server subtitle state") {
  const std::uint16_t port = FindFreePort();

  subtitler::PreviewFrameBuffer frames;

  subtitler::SubtitleState state{{"Movie.srt"}, true, false, 1234, -150};
  bool set_ok = true;

  subtitler::WebServerHooks hooks;
  hooks.subtitle_state_get = [&] { return state; };
  hooks.subtitle_state_set =
      [&](const subtitler::SubtitleStatePatch& patch) {
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
          "\"time\":1234,\"delay\":-150}");
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
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?file=Other.srt")
              .status == SOUP_STATUS_OK);
    CHECK(state.file == std::optional<std::string>{"Other.srt"});

    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?file=").status ==
          SOUP_STATUS_OK);
    CHECK(state.file == std::nullopt);
  }

  SUBCASE("PUT rejects bad values without touching the state") {
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?time=abc").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?paused=yes").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK(HttpRequest("PUT", port, "/api/subtitle-state?bogus=1").status ==
          SOUP_STATUS_BAD_REQUEST);
    CHECK_FALSE(state.paused);
    CHECK(state.time_ms == 1234);
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
