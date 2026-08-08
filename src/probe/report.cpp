#include "probe/report.h"

#include <algorithm>
#include <format>
#include <map>
#include <print>

namespace {

std::string escape_json(const std::string& text) {
  std::string result;
  for (const char c : text) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      default:
        result += c;
    }
  }
  return result;
}

std::string join(const std::vector<std::string>& items,
                 const std::string& separator) {
  std::string result;
  for (const auto& item : items) {
    if (!result.empty()) {
      result += separator;
    }
    result += item;
  }
  return result;
}

std::string rates_to_string(const std::vector<int>& rates) {
  std::string result;
  for (const int rate : rates) {
    if (!result.empty()) {
      result += ", ";
    }
    result += std::to_string(rate);
  }
  return result;
}

}  // namespace

namespace subtitler::probe {

void print_devices_text(const std::vector<VideoDevice>& devices) {
  std::println("## Video devices\n");
  if (devices.empty()) {
    std::println("(no capture devices found)\n");
    return;
  }
  for (const auto& device : devices) {
    std::println("{}: {} ({}, {}){}", device.path, device.card, device.driver,
                 device.bus, device.is_cv105 ? " [CV105]" : "");
  }
  std::println();
}

void print_modes_text(const std::string& path,
                      const std::vector<VideoMode>& modes) {
  std::println("## Capture modes: {}\n", path);
  if (modes.empty()) {
    std::println("(no modes found)\n");
    return;
  }
  for (const auto& mode : modes) {
    std::println("{}  {}x{}  {} fps", mode.format, mode.width, mode.height,
                 rates_to_string(mode.frame_rates));
  }
  std::println();
}

void print_audio_text(const std::vector<AudioDevice>& devices) {
  const auto print = [&](bool capture, std::string_view title) {
    std::println("## {}\n", title);
    bool any = false;
    for (const auto& device : devices) {
      if (capture ? !device.capture : !device.playback) {
        continue;
      }
      any = true;
      std::println(
          "hw:CARD={},DEV={}  {}{}", device.card_id, device.device,
          device.card_name,
          device.device_name.empty() ? "" : " — " + device.device_name);
    }
    if (!any) {
      std::println("(none found)");
    }
    std::println();
  };

  print(true, "Audio capture");
  print(false, "Audio playback");
}

void print_elements_text(const std::vector<ElementAvailability>& elements) {
  std::println("## GStreamer elements\n");
  for (const auto& [name, available] : elements) {
    std::println("{:<13}{}", name, available ? "available" : "MISSING");
  }
  std::println();
}

void print_drm_text(const DrmInfo& info) {
  std::println("## DRM\n");
  if (info.driver.empty()) {
    std::println("(driver not found)\n");
    return;
  }

  std::println("Driver: {}\n", info.driver);
  std::println("Connectors:");
  for (const auto& connector : info.connectors) {
    std::println("  {}: {}  {}", connector.id, connector.name,
                 connector.connected ? "connected" : "disconnected");
  }

  // Planes sharing a format set are reported together.
  std::map<std::string, std::vector<int>> by_formats;
  for (const auto& plane : info.planes) {
    by_formats[join(plane.formats, " ")].push_back(plane.id);
  }
  std::println("\nPlanes:");
  for (const auto& [formats, ids] : by_formats) {
    std::vector<std::string> id_strings;
    for (const int id : ids) {
      id_strings.push_back(std::to_string(id));
    }
    std::println("  {} plane{} ({}): {}", ids.size(),
                 ids.size() == 1 ? "" : "s", join(id_strings, ", "), formats);
  }
  std::println();
}

void print_pipeline_text(const PipelinePlan& plan) {
  std::println("## Recommended pipeline\n");
  if (plan.device_path.empty()) {
    std::println("(no recommendation: capture device missing)\n");
    return;
  }

  std::println("Capture: {} {}x{} at {} fps from {}{}", plan.capture_format,
               plan.width, plan.height, plan.frame_rate, plan.device_path,
               plan.needs_jpegdec ? " (via jpegdec)" : "");
  std::println("Conversion: {} to {}", plan.converter, plan.kms_format);
  if (plan.connector_id) {
    std::println("Output: kmssink connector-id={}", *plan.connector_id);
  }

  for (const auto& note : plan.notes) {
    std::println("Note: {}", note);
  }

  if (plan.negotiation_tested) {
    std::println(
        "\nNegotiation: {}",
        plan.negotiation_ok ? "ok" : "FAILED: " + plan.negotiation_error);
  }
  std::println();
}

std::string devices_to_json(const std::vector<VideoDevice>& devices,
                            const std::vector<std::vector<VideoMode>>& modes) {
  std::string result = "[";
  for (std::size_t i = 0; i < devices.size(); ++i) {
    const auto& device = devices[i];
    result += std::format(
        "{}{{\"path\": \"{}\", \"name\": \"{}\", \"driver\": \"{}\", "
        "\"bus\": \"{}\", \"is_cv105\": {}",
        i == 0 ? "" : ", ", escape_json(device.path), escape_json(device.card),
        escape_json(device.driver), escape_json(device.bus), device.is_cv105);
    if (!modes.empty()) {
      result += std::format(", \"modes\": {}", modes_to_json(modes[i]));
    }
    result += "}";
  }
  return result + "]";
}

std::string modes_to_json(const std::vector<VideoMode>& modes) {
  std::string result = "[";
  bool first = true;
  for (const auto& mode : modes) {
    std::string rates;
    for (const int rate : mode.frame_rates) {
      rates += (rates.empty() ? "" : ", ") + std::to_string(rate);
    }
    result += std::format(
        "{}{{\"format\": \"{}\", \"width\": {}, \"height\": {}, "
        "\"frame_rates\": [{}]}}",
        first ? "" : ", ", mode.format, mode.width, mode.height, rates);
    first = false;
  }
  return result + "]";
}

std::string audio_to_json(const std::vector<AudioDevice>& devices,
                          bool capture) {
  std::string result = "[";
  bool first = true;
  for (const auto& device : devices) {
    if (capture ? !device.capture : !device.playback) {
      continue;
    }
    result += std::format(
        "{}{{\"device\": \"hw:CARD={},DEV={}\", \"card_id\": \"{}\", "
        "\"card_name\": \"{}\", \"device_name\": \"{}\"}}",
        first ? "" : ", ", device.card_id, device.device,
        escape_json(device.card_id), escape_json(device.card_name),
        escape_json(device.device_name));
    first = false;
  }
  return result + "]";
}

std::string elements_to_json(const std::vector<ElementAvailability>& elements) {
  std::string result = "{";
  bool first = true;
  for (const auto& [name, available] : elements) {
    result += std::format("{}\"{}\": {}", first ? "" : ", ", name, available);
    first = false;
  }
  return result + "}";
}

std::string drm_to_json(const DrmInfo& info) {
  std::string connectors = "[";
  bool first = true;
  for (const auto& connector : info.connectors) {
    connectors += std::format(
        "{}{{\"id\": {}, \"name\": \"{}\", \"connected\": {}}}",
        first ? "" : ", ", connector.id, connector.name, connector.connected);
    first = false;
  }
  connectors += "]";

  std::string planes = "[";
  first = true;
  for (const auto& plane : info.planes) {
    std::string formats;
    for (const auto& format : plane.formats) {
      formats += (formats.empty() ? "" : ", ") + std::format("\"{}\"", format);
    }
    planes += std::format("{}{{\"id\": {}, \"formats\": [{}]}}",
                          first ? "" : ", ", plane.id, formats);
    first = false;
  }
  planes += "]";

  return std::format(
      "{{\"driver\": \"{}\", \"connectors\": {}, "
      "\"planes\": {}}}",
      escape_json(info.driver), connectors, planes);
}

std::string pipeline_to_json(const PipelinePlan& plan) {
  std::string notes = "[";
  bool first = true;
  for (const auto& note : plan.notes) {
    notes += std::format("{}\"{}\"", first ? "" : ", ", escape_json(note));
    first = false;
  }
  notes += "]";

  return std::format(
      "{{\"device\": \"{}\", \"capture_format\": \"{}\", \"width\": {}, "
      "\"height\": {}, \"frame_rate\": {}, \"needs_jpegdec\": {}, "
      "\"converter\": \"{}\", \"kms_format\": \"{}\", \"connector_id\": {}, "
      "\"notes\": {}, \"negotiation\": {{\"tested\": {}, \"ok\": {}, "
      "\"error\": \"{}\"}}}}",
      escape_json(plan.device_path), plan.capture_format, plan.width,
      plan.height, plan.frame_rate, plan.needs_jpegdec, plan.converter,
      plan.kms_format,
      plan.connector_id ? std::to_string(*plan.connector_id) : "null", notes,
      plan.negotiation_tested, plan.negotiation_ok,
      escape_json(plan.negotiation_error));
}

}  // namespace subtitler::probe
