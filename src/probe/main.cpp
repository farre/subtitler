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

constexpr std::string_view kms_driver = "vc4";

int usage(const char* program) {
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
    return usage(argv[0]);
  }

  using namespace subtitler::probe;

  if (args.empty()) {
    const auto devices = list_video_devices();
    std::vector<std::vector<VideoMode>> modes;
    for (const auto& device : devices) {
      modes.push_back(list_capture_modes(device.path));
    }
    const auto audio = list_audio_devices();
    const auto elements = probe_elements();
    const auto drm = probe_drm(std::string{kms_driver});
    auto plan = recommend_pipeline(devices, modes, elements, drm);
    test_negotiation(plan);

    if (json) {
      std::println(
          "{{\"video_devices\": {}, \"audio_capture\": {}, "
          "\"audio_playback\": {}, \"gstreamer_elements\": {}, \"drm\": {}, "
          "\"recommendation\": {}}}",
          devices_to_json(devices, modes), audio_to_json(audio, true),
          audio_to_json(audio, false), elements_to_json(elements),
          drm_to_json(drm), pipeline_to_json(plan));
    } else {
      print_devices_text(devices);
      for (std::size_t i = 0; i < devices.size(); ++i) {
        print_modes_text(devices[i].path, modes[i]);
      }
      print_audio_text(audio);
      print_elements_text(elements);
      print_drm_text(drm);
      print_pipeline_text(plan);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "devices" && args.size() == 1) {
    const auto devices = list_video_devices();
    if (json) {
      std::println("{}", devices_to_json(devices, {}));
    } else {
      print_devices_text(devices);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "capture" && args.size() == 2) {
    const std::string path{args[1]};
    const auto modes = list_capture_modes(path);
    if (json) {
      std::println("{{\"path\": \"{}\", \"modes\": {}}}", path,
                   modes_to_json(modes));
    } else {
      print_modes_text(path, modes);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "audio" && args.size() == 1) {
    const auto audio = list_audio_devices();
    if (json) {
      std::println("{{\"capture\": {}, \"playback\": {}}}",
                   audio_to_json(audio, true), audio_to_json(audio, false));
    } else {
      print_audio_text(audio);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "plugins" && args.size() == 1) {
    const auto elements = probe_elements();
    if (json) {
      std::println("{}", elements_to_json(elements));
    } else {
      print_elements_text(elements);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "drm" && args.size() == 1) {
    const auto drm = probe_drm(std::string{kms_driver});
    if (json) {
      std::println("{}", drm_to_json(drm));
    } else {
      print_drm_text(drm);
    }
    return EXIT_SUCCESS;
  }

  if (args[0] == "pipeline" && args.size() == 1) {
    const auto devices = list_video_devices();
    std::vector<std::vector<VideoMode>> modes;
    for (const auto& device : devices) {
      modes.push_back(list_capture_modes(device.path));
    }
    const auto elements = probe_elements();
    const auto drm = probe_drm(std::string{kms_driver});
    auto plan = recommend_pipeline(devices, modes, elements, drm);
    test_negotiation(plan);

    if (json) {
      std::println("{}", pipeline_to_json(plan));
    } else {
      print_pipeline_text(plan);
    }
    return EXIT_SUCCESS;
  }

  return usage(argv[0]);
}
