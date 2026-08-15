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

using namespace subtitler::probe;

namespace {

constexpr std::string_view kKmsDriver = "vc4";

int Usage(const char* program) {
  std::println(stderr,
               "Usage: {} [--json] [devices | capture <device> | audio | "
               "plugins | drm | pipeline]",
               program);
  return EXIT_FAILURE;
}

struct ProbeResults {
  std::vector<VideoDevice> devices;
  std::vector<std::vector<VideoMode>> modes;
  std::vector<ElementAvailability> elements;
  DrmInfo drm;
  PipelinePlan plan;
};

ProbeResults RunProbes() {
  ProbeResults results;

  results.devices = ListVideoDevices();
  for (const auto& device : results.devices) {
    results.modes.push_back(ListCaptureModes(device.path));
  }
  results.elements = ProbeElements();
  results.drm = ProbeDrm(std::string{kKmsDriver});
  results.plan = RecommendPipeline(results.devices, results.modes,
                                   results.elements, results.drm);
  TestNegotiation(results.plan);

  return results;
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

  if (args.empty()) {
    const auto results = RunProbes();
    const auto audio = ListAudioDevices();

    if (json) {
      std::println(
          "{{\"video_devices\": {}, \"audio_capture\": {}, "
          "\"audio_playback\": {}, \"gstreamer_elements\": {}, \"drm\": {}, "
          "\"recommendation\": {}}}",
          DevicesToJson(results.devices, results.modes),
          AudioToJson(audio, true), AudioToJson(audio, false),
          ElementsToJson(results.elements), DrmToJson(results.drm),
          PipelineToJson(results.plan));
    } else {
      PrintDevicesText(results.devices);
      for (std::size_t i = 0; i < results.devices.size(); ++i) {
        PrintModesText(results.devices[i].path, results.modes[i]);
      }
      PrintAudioText(audio);
      PrintElementsText(results.elements);
      PrintDrmText(results.drm);
      PrintPipelineText(results.plan);
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
    const auto results = RunProbes();

    if (json) {
      std::println("{}", PipelineToJson(results.plan));
    } else {
      PrintPipelineText(results.plan);
    }
    return EXIT_SUCCESS;
  }

  return Usage(argv[0]);
}
