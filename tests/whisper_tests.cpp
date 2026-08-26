#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "stream/whisper_transcriber.h"

namespace {

// Reads the samples of a canonical 16 kHz mono S16 PCM WAV (e.g.
// whisper.cpp's samples/jfk.wav): walks the RIFF chunks instead of
// assuming a 44-byte header.
std::vector<float> ReadWavSamples(const std::string& path) {
  std::ifstream file{path, std::ios::binary};
  if (!file) {
    return {};
  }
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file},
                                        std::istreambuf_iterator<char>{}};

  if (bytes.size() < 12 ||
      std::string{bytes.begin(), bytes.begin() + 4} != "RIFF" ||
      std::string{bytes.begin() + 8, bytes.begin() + 12} != "WAVE") {
    return {};
  }

  for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
    const std::string id{bytes.begin() + offset, bytes.begin() + offset + 4};
    const auto size = static_cast<std::uint32_t>(bytes[offset + 4]) |
                      static_cast<std::uint32_t>(bytes[offset + 5]) << 8 |
                      static_cast<std::uint32_t>(bytes[offset + 6]) << 16 |
                      static_cast<std::uint32_t>(bytes[offset + 7]) << 24;
    offset += 8;

    if (id == "data" && offset + size <= bytes.size()) {
      std::vector<float> samples;
      samples.reserve(size / 2);
      for (std::size_t i = offset; i + 1 < offset + size; i += 2) {
        const auto sample = static_cast<std::int16_t>(
            bytes[i] | static_cast<std::uint16_t>(bytes[i + 1]) << 8);
        samples.push_back(static_cast<float>(sample) / 32768.0F);
      }
      return samples;
    }

    offset += size + (size % 2);
  }

  return {};
}

}  // namespace

TEST_CASE("whisper transcriber") {
  SUBCASE("rejects an unloadable model") {
    CHECK(subtitler::WhisperTranscriber::Create("/nonexistent/model.bin") ==
          nullptr);
  }

  // Model-gated: point SUBTITLER_TEST_WHISPER_MODEL at a ggml model (e.g.
  // ggml-tiny.en.bin) to run the inference checks.
  const char* model = std::getenv("SUBTITLER_TEST_WHISPER_MODEL");
  if (model == nullptr) {
    MESSAGE("skipping: SUBTITLER_TEST_WHISPER_MODEL is not set");
    return;
  }

  auto transcriber = subtitler::WhisperTranscriber::Create(model);
  REQUIRE(transcriber != nullptr);

  SUBCASE("returns nullopt until a window has accumulated") {
    const std::vector<float> chunk(16000, 0.0F);

    CHECK_FALSE(transcriber->Push(chunk).has_value());
    CHECK_FALSE(transcriber->Push(chunk).has_value());
    CHECK_FALSE(transcriber->Push(chunk).has_value());
    CHECK_FALSE(transcriber->Push(chunk).has_value());
    // The fifth second completes a window. Digital silence still earns a
    // token or two from the tiny model — fine, the spike logs whatever
    // it hears.
    const auto text = transcriber->Push(chunk);
    REQUIRE(text.has_value());
    INFO("silence transcript: '", *text, "'");
    CHECK(text->size() < 16);
  }

  SUBCASE("transcribes speech fed in live-sized chunks") {
    const char* wav = std::getenv("SUBTITLER_TEST_WHISPER_WAV");
    if (wav == nullptr) {
      MESSAGE("skipping: SUBTITLER_TEST_WHISPER_WAV is not set");
      return;
    }

    const auto samples = ReadWavSamples(wav);
    REQUIRE_FALSE(samples.empty());

    // Feed 10 ms chunks, the cadence of the capture pipeline's buffers.
    std::string transcript;
    for (std::size_t offset = 0; offset < samples.size(); offset += 160) {
      const auto size = std::min<std::size_t>(160, samples.size() - offset);
      if (auto text = transcriber->Push(
              std::span<const float>{samples.data() + offset, size})) {
        transcript += *text;
      }
    }

    INFO("transcript: ", transcript);
    CHECK(transcript.contains("country"));
  }
}
