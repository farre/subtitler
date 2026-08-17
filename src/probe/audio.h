#pragma once

#include <optional>
#include <string>
#include <vector>

namespace subtitler::probe {

// Raw device capabilities from snd_pcm_hw_params on the hw: device.
struct PcmCaps {
  std::vector<std::string> formats;  // ALSA names, e.g. "S16_LE"
  std::vector<unsigned int> rates;   // supported standard rates, Hz
  unsigned int min_channels = 0;
  unsigned int max_channels = 0;
};

struct AudioDevice {
  int card;                 // ALSA card index
  int device;               // PCM device index
  std::string card_id;      // e.g. "Video" — stable across reboots
  std::string card_name;    // e.g. "USB3 Video"
  std::string device_name;  // e.g. "USB Audio"
  bool capture = false;
  bool playback = false;
  std::optional<PcmCaps> capture_caps;  // nullopt if the PCM couldn't be opened
};

// Enumerates ALSA cards x PCM devices x streams via the libasound ctl API.
std::vector<AudioDevice> ListAudioDevices();

}  // namespace subtitler::probe
