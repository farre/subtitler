#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <gst/gst.h>

#include <memory>
#include <string>
#include <utility>

#include "stream/deleters.h"
#include "stream/stream.h"

namespace {

template <typename T>
using GstPointer = std::unique_ptr<T, subtitler::GstDeleter<T>>;

void CheckConstructible(const std::string& description) {
  GstPointer<GError> error;
  const GstPointer<GstElement> pipeline{
      gst_parse_launch(description.c_str(), std::out_ptr(error))};

  // gst_parse_launch can return a partially built pipeline on failure;
  // the error is the source of truth.
  INFO("gst_parse_launch error: ",
       error != nullptr ? std::string{error->message} : "none");
  CHECK(error == nullptr);
}

}  // namespace

TEST_CASE("capture pipeline description") {
  gst_init(nullptr, nullptr);

  SUBCASE("contains the key capture properties") {
    const auto description =
        subtitler::CapturePipelineDescription("/dev/video0");

    CHECK(description.contains("v4l2src"));
    CHECK(description.contains("device=\"/dev/video0\""));
    CHECK(description.contains("video/x-raw"));
    CHECK(description.contains("format=YUY2"));
    CHECK(description.contains("width=1920"));
    CHECK(description.contains("height=1080"));
    CHECK(description.contains("framerate=60/1"));
    CHECK(description.contains("appsink"));
    CHECK(description.contains("name=capture_sink"));
  }

  SUBCASE("substitutes the device") {
    CHECK(subtitler::CapturePipelineDescription("/dev/video42")
              .contains("device=\"/dev/video42\""));
  }

  SUBCASE("is constructible") {
    CheckConstructible(subtitler::CapturePipelineDescription("/dev/video0"));
  }
}

TEST_CASE("output pipeline description") {
  gst_init(nullptr, nullptr);

  SUBCASE("contains the key output properties") {
    const auto description = subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsSoftware, std::nullopt);

    CHECK(description.contains("appsrc"));
    CHECK(description.contains("name=output_source"));
    CHECK(description.contains("videoconvert"));
    CHECK(description.contains("format=NV16"));
    CHECK(description.contains("width=1920"));
    CHECK(description.contains("height=1080"));
    CHECK(description.contains("framerate=60/1"));
    CHECK(description.contains("kmssink"));
    CHECK(description.contains("driver-name=vc4"));
  }

  SUBCASE("pisp pipeline has pispconvert and NV12 DMABuf") {
    const auto description = subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsPisp, std::nullopt);

    CHECK(description.contains("pispconvert"));
    CHECK(description.contains("video/x-raw(memory:DMABuf)"));
    CHECK(description.contains("drm-format=NV12"));
  }

  SUBCASE("omits connector-id without a connector") {
    CHECK_FALSE(subtitler::OutputPipelineDescription(
                    subtitler::OutputMode::kKmsSoftware, std::nullopt)
                    .contains("connector-id"));
  }

  SUBCASE("includes connector-id with a connector") {
    CHECK(subtitler::OutputPipelineDescription(
              subtitler::OutputMode::kKmsSoftware, 7)
              .contains("connector-id=7"));
  }

  SUBCASE("is constructible") {
    CheckConstructible(subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsSoftware, std::nullopt));
    CheckConstructible(subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsSoftware, 7));
    CheckConstructible(subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kNull, std::nullopt));

    GstPointer<GstElementFactory> pisp_factory{
        gst_element_factory_find("pispconvert")};
    if (pisp_factory != nullptr) {
      CheckConstructible(subtitler::OutputPipelineDescription(
          subtitler::OutputMode::kKmsPisp, std::nullopt));
    }

    GstPointer<GstElementFactory> gl_factory{
        gst_element_factory_find("glimagesink")};
    if (gl_factory != nullptr) {
      CheckConstructible(subtitler::OutputPipelineDescription(
          subtitler::OutputMode::kWindow, std::nullopt));
    }
  }
}
