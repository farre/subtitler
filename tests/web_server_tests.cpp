#include <doctest/doctest.h>
#include <libsoup/soup.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
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

Response HttpGet(std::uint16_t port, std::string_view path) {
  GObjectPtr<SoupSession> session{soup_session_new()};
  GObjectPtr<SoupMessage> message{
      soup_message_new("GET", Url(port, path).c_str())};

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

  SUBCASE("index page shows the stream") {
    auto server = subtitler::WebServer::Create(port, frames);
    REQUIRE(server != nullptr);

    const auto response = HttpGet(port, "/");

    CHECK(response.status == SOUP_STATUS_OK);
    CHECK(response.content_type.starts_with("text/html"));
    CHECK(response.body.contains("/api/preview.mjpeg"));
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
      BytesPtr chunk{g_input_stream_read_bytes(
          stream.get(), 16384, cancellable.get(), nullptr)};

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
