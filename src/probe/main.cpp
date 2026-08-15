#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "probe/audio.h"
#include "probe/capture.h"
#include "probe/drm.h"
#include "probe/pipeline.h"
#include "probe/plugins.h"
#include "probe/report.h"

namespace {

constexpr std::string_view kKmsDriver = "vc4";

int Usage(const char* program) {
  std::println(stderr,
               "Usage: {} [--json] [devices | capture <device> | audio | "
               "plugins | drm | pipeline]",
               program);
  return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
  bool json = false;
  std::vector<std::string_view> args;

  for (int i = 1; i < argc; ++i) {
    if (argv[i] == std::string_view{"--json"}) {
      json = true;
    } else {
      args.emplace_back(argv[i]);
    }
  }

  if (args.size() > 2 || (args.size() == 2 && args[0] != "capture")) {
    return Usage(argv[0]);
  }

  using namespace subtitler::probe;

  if (args.empty()) {
    const auto devices = ListVideoDevices();
    std::vector<std::vector<VideoMode>> modes;
    for (const auto& device : devices) {
      modes.push_back(ListCaptureModes(device.path));
    }
    const auto audio = ListAudioDevices();
    const auto elements = ProbeElements();
    const auto drm = ProbeDrm(std::string{kKmsDriver});
    auto plan = RecommendPipeline(devices, modes, elements, drm);
    TestNegotiation(plan);

    if (json) {
      std::println(
          "{{\"video_devices\": {}, \"audio_capture\": {}, "
          "\"audio_playback\": {}, \"gstreamer_elements\": {}, \"drm\": {}, "
          "\"recommendation\": {}}}",
          DevicesToJson(devices, modes), AudioToJson(audio, true),
          AudioToJson(audio, false), ElementsToJson(elements), DrmToJson(drm),
          PipelineToJson(plan));
    } else {
      PrintDevicesText(devices);
      for (std::size_t i = 0; i < devices.size(); ++i) {
        PrintModesText(devices[i].path, modes[i]);
      }
      PrintAudioText(audio);
      PrintElementsText(elements);
      PrintDrmText(drm);
      PrintPipelineText(plan);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "devices" && args.size() == 1) {
    const auto devices = ListVideoDevices();
    if (json) {
      std::println("{}", DevicesToJson(devices, {}));
    } else {
      PrintDevicesText(devices);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "capture" && args.size() == 2) {
    const std::string path{args[1]};
    const auto modes = ListCaptureModes(path);
    if (json) {
      std::println("{{\"path\": \"{}\", \"modes\": {}}}", path,
                   ModesToJson(modes));
    } else {
      PrintModesText(path, modes);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "audio" && args.size() == 1) {
    const auto audio = ListAudioDevices();
    if (json) {
      std::println("{{\"capture\": {}, \"playback\": {}}}",
                   AudioToJson(audio, true), AudioToJson(audio, false));
    } else {
      PrintAudioText(audio);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "plugins" && args.size() == 1) {
    const auto elements = ProbeElements();
    if (json) {
      std::println("{}", ElementsToJson(elements));
    } else {
      PrintElementsText(elements);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "drm" && args.size() == 1) {
    const auto drm = ProbeDrm(std::string{kKmsDriver});
    if (json) {
      std::println("{}", DrmToJson(drm));
    } else {
      PrintDrmText(drm);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "pipeline" && args.size() == 1) {
    const auto devices = ListVideoDevices();
    std::vector<std::vector<VideoMode>> modes;
    for (const auto& device : devices) {
      modes.push_back(ListCaptureModes(device.path));
    }
    const auto elements = ProbeElements();
    const auto drm = ProbeDrm(std::string{kKmsDriver});
    auto plan = RecommendPipeline(devices, modes, elements, drm);
    TestNegotiation(plan);

    if (json) {
      std::println("{}", PipelineToJson(plan));
    } else {
      PrintPipelineText(plan);
    }
    return EXIT_SUCCESS;
  }

  return Usage(argv[0]);
}
