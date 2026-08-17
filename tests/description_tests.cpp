#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <gst/gst.h>

#include <memory>
#include <string>
#include <utility>

#include "stream/deleters.h"
#include "stream/stream.h"

namespace {

void CheckConstructible(const std::string& description) {
  subtitler::GstPointer<GError> error;
  const subtitler::GstPointer<GstElement> pipeline{
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
        subtitler::VideoCapturePipelineDescription("/dev/video0");

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
    CHECK(subtitler::VideoCapturePipelineDescription("/dev/video42")
              .contains("device=\"/dev/video42\""));
  }

  SUBCASE("audio branch contains the key audio properties") {
    const auto description =
        subtitler::AudioCapturePipelineDescription("hw:CARD=Video,DEV=0");

    CHECK(description.contains("alsasrc"));
    CHECK(description.contains("device=\"hw:CARD=Video,DEV=0\""));
    CHECK(description.contains("do-timestamp=true"));
    CHECK(description.contains("audio/x-raw"));
    CHECK(description.contains("format=S16LE"));
    CHECK(description.contains("rate=48000"));
    CHECK(description.contains("channels=2"));
    CHECK(description.contains("appsink"));
    CHECK(description.contains("name=capture_audio_sink"));
  }

  SUBCASE("audio branch is optional") {
    const auto with_audio =
        subtitler::CapturePipelineDescription("/dev/video0", true);
    const auto without_audio =
        subtitler::CapturePipelineDescription("/dev/video0", false);

    CHECK(with_audio.contains("alsasrc"));
    CHECK(with_audio.contains("v4l2src"));
    CHECK_FALSE(without_audio.contains("alsasrc"));
    CHECK(without_audio.contains("v4l2src"));
  }

  SUBCASE("is constructible") {
    CheckConstructible(
        subtitler::CapturePipelineDescription("/dev/video0", false));

    subtitler::GstPointer<GstElementFactory> alsa_factory{
        gst_element_factory_find("alsasrc")};
    if (alsa_factory != nullptr) {
      CheckConstructible(
          subtitler::CapturePipelineDescription("/dev/video0", true));
    }
  }
}

TEST_CASE("output pipeline description") {
  gst_init(nullptr, nullptr);

  SUBCASE("contains the key output properties") {
    const auto description = subtitler::VideoOutputPipelineDescription(
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
    const auto description = subtitler::VideoOutputPipelineDescription(
        subtitler::OutputMode::kKmsPisp, std::nullopt);

    CHECK(description.contains("pispconvert"));
    CHECK(description.contains("video/x-raw(memory:DMABuf)"));
    CHECK(description.contains("drm-format=NV12"));
  }

  SUBCASE("omits connector-id without a connector") {
    CHECK_FALSE(subtitler::VideoOutputPipelineDescription(
                    subtitler::OutputMode::kKmsSoftware, std::nullopt)
                    .contains("connector-id"));
  }

  SUBCASE("includes connector-id with a connector") {
    CHECK(subtitler::VideoOutputPipelineDescription(
              subtitler::OutputMode::kKmsSoftware, 7)
              .contains("connector-id=7"));
  }

  SUBCASE("audio branch contains the key audio properties") {
    const auto description =
        subtitler::AudioOutputPipelineDescription("hw:CARD=vc4hdmi0,DEV=0");

    CHECK(description.contains("appsrc"));
    CHECK(description.contains("name=output_audio_source"));
    CHECK(description.contains("is-live=true"));
    CHECK(description.contains("format=time"));
    CHECK(description.contains("alsasink"));
    CHECK(description.contains("device=\"hw:CARD=vc4hdmi0,DEV=0\""));
    CHECK(description.contains("slave-method=skew"));
  }

  SUBCASE("audio branch is optional") {
    const auto with_audio = subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsSoftware, std::nullopt,
        "hw:CARD=vc4hdmi0,DEV=0");
    const auto without_audio = subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsSoftware, std::nullopt, std::nullopt);

    CHECK(with_audio.contains("alsasink"));
    CHECK(with_audio.contains("kmssink"));
    CHECK_FALSE(without_audio.contains("alsasink"));
    CHECK(without_audio.contains("kmssink"));
  }

  SUBCASE("is constructible") {
    CheckConstructible(subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsSoftware, std::nullopt, std::nullopt));
    CheckConstructible(subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kKmsSoftware, 7, std::nullopt));
    CheckConstructible(subtitler::OutputPipelineDescription(
        subtitler::OutputMode::kNull, std::nullopt, std::nullopt));

    subtitler::GstPointer<GstElementFactory> alsa_factory{
        gst_element_factory_find("alsasink")};
    if (alsa_factory != nullptr) {
      CheckConstructible(subtitler::OutputPipelineDescription(
          subtitler::OutputMode::kNull, std::nullopt,
          "hw:CARD=vc4hdmi0,DEV=0"));
    }

    subtitler::GstPointer<GstElementFactory> pisp_factory{
        gst_element_factory_find("pispconvert")};
    if (pisp_factory != nullptr) {
      CheckConstructible(subtitler::OutputPipelineDescription(
          subtitler::OutputMode::kKmsPisp, std::nullopt, std::nullopt));
    }

    subtitler::GstPointer<GstElementFactory> gl_factory{
        gst_element_factory_find("glimagesink")};
    if (gl_factory != nullptr) {
      CheckConstructible(subtitler::OutputPipelineDescription(
          subtitler::OutputMode::kWindow, std::nullopt, std::nullopt));
    }
  }
}
