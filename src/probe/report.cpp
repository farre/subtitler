#include "probe/report.h"

#include <algorithm>
#include <format>
#include <map>
#include <print>

namespace {

std::string EscapeJson(const std::string& text) {
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

std::string Join(const std::vector<std::string>& items,
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

std::string RatesToString(const std::vector<int>& rates) {
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

void PrintDevicesText(const std::vector<VideoDevice>& devices) {
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

void PrintModesText(const std::string& path,
                    const std::vector<VideoMode>& modes) {
  std::println("## Capture modes: {}\n", path);
  if (modes.empty()) {
    std::println("(no modes found)\n");
    return;
  }
  for (const auto& mode : modes) {
    std::println("{}  {}x{}  {} fps", mode.format, mode.width, mode.height,
                 RatesToString(mode.frame_rates));
  }
  std::println();
}

void PrintAudioText(const std::vector<AudioDevice>& devices) {
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
      if (capture && device.capture_caps) {
        const auto& caps = *device.capture_caps;
        std::vector<std::string> rates;
        for (const unsigned int rate : caps.rates) {
          rates.push_back(std::to_string(rate));
        }
        std::println("  formats: {}", Join(caps.formats, ", "));
        std::println("  rates: {}", Join(rates, ", "));
        std::println("  channels: {}-{}", caps.min_channels, caps.max_channels);
      }
    }
    if (!any) {
      std::println("(none found)");
    }
    std::println();
  };

  print(true, "Audio capture");
  print(false, "Audio playback");
}

void PrintElementsText(const std::vector<ElementAvailability>& elements) {
  std::println("## GStreamer elements\n");
  for (const auto& [name, available] : elements) {
    std::println("{:<13}{}", name, available ? "available" : "MISSING");
  }
  std::println();
}

void PrintDrmText(const DrmInfo& info) {
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
    by_formats[Join(plane.formats, " ")].push_back(plane.id);
  }
  std::println("\nPlanes:");
  for (const auto& [formats, ids] : by_formats) {
    std::vector<std::string> id_strings;
    for (const int id : ids) {
      id_strings.push_back(std::to_string(id));
    }
    std::println("  {} plane{} ({}): {}", ids.size(),
                 ids.size() == 1 ? "" : "s", Join(id_strings, ", "), formats);
  }
  std::println();
}

void PrintPipelineText(const PipelinePlan& plan) {
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

std::string DevicesToJson(const std::vector<VideoDevice>& devices,
                          const std::vector<std::vector<VideoMode>>& modes) {
  std::string result = "[";
  for (std::size_t i = 0; i < devices.size(); ++i) {
    const auto& device = devices[i];
    result += std::format(
        "{}{{\"path\": \"{}\", \"name\": \"{}\", \"driver\": \"{}\", "
        "\"bus\": \"{}\", \"is_cv105\": {}",
        i == 0 ? "" : ", ", EscapeJson(device.path), EscapeJson(device.card),
        EscapeJson(device.driver), EscapeJson(device.bus), device.is_cv105);
    if (!modes.empty()) {
      result += std::format(", \"modes\": {}", ModesToJson(modes[i]));
    }
    result += "}";
  }
  return result + "]";
}

std::string ModesToJson(const std::vector<VideoMode>& modes) {
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

std::string AudioToJson(const std::vector<AudioDevice>& devices, bool capture) {
  std::string result = "[";
  bool first = true;
  for (const auto& device : devices) {
    if (capture ? !device.capture : !device.playback) {
      continue;
    }
    result += std::format(
        "{}{{\"device\": \"hw:CARD={},DEV={}\", \"card_id\": \"{}\", "
        "\"card_name\": \"{}\", \"device_name\": \"{}\"",
        first ? "" : ", ", device.card_id, device.device,
        EscapeJson(device.card_id), EscapeJson(device.card_name),
        EscapeJson(device.device_name));
    if (capture && device.capture_caps) {
      const auto& caps = *device.capture_caps;
      std::string formats;
      std::string rates;
      for (const auto& format : caps.formats) {
        formats += (formats.empty() ? "" : ", ") +
                   std::format("\"{}\"", EscapeJson(format));
      }
      for (const unsigned int rate : caps.rates) {
        rates += (rates.empty() ? "" : ", ") + std::to_string(rate);
      }
      result += std::format(
          ", \"caps\": {{\"formats\": [{}], \"rates\": [{}], "
          "\"min_channels\": {}, \"max_channels\": {}}}",
          formats, rates, caps.min_channels, caps.max_channels);
    }
    result += "}";
    first = false;
  }
  return result + "]";
}

std::string ElementsToJson(const std::vector<ElementAvailability>& elements) {
  std::string result = "{";
  bool first = true;
  for (const auto& [name, available] : elements) {
    result += std::format("{}\"{}\": {}", first ? "" : ", ", name, available);
    first = false;
  }
  return result + "}";
}

std::string DrmToJson(const DrmInfo& info) {
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
      EscapeJson(info.driver), connectors, planes);
}

std::string PipelineToJson(const PipelinePlan& plan) {
  std::string notes = "[";
  bool first = true;
  for (const auto& note : plan.notes) {
    notes += std::format("{}\"{}\"", first ? "" : ", ", EscapeJson(note));
    first = false;
  }
  notes += "]";

  return std::format(
      "{{\"device\": \"{}\", \"capture_format\": \"{}\", \"width\": {}, "
      "\"height\": {}, \"frame_rate\": {}, \"needs_jpegdec\": {}, "
      "\"converter\": \"{}\", \"kms_format\": \"{}\", \"connector_id\": {}, "
      "\"notes\": {}, \"negotiation\": {{\"tested\": {}, \"ok\": {}, "
      "\"error\": \"{}\"}}}}",
      EscapeJson(plan.device_path), plan.capture_format, plan.width,
      plan.height, plan.frame_rate, plan.needs_jpegdec, plan.converter,
      plan.kms_format,
      plan.connector_id ? std::to_string(*plan.connector_id) : "null", notes,
      plan.negotiation_tested, plan.negotiation_ok,
      EscapeJson(plan.negotiation_error));
}

}  // namespace subtitler::probe
