#pragma once

#include <string>
#include <vector>

namespace subtitler::probe {

struct AudioDevice {
  int card;                 // ALSA card index
  int device;               // PCM device index
  std::string card_id;      // e.g. "Video" — stable across reboots
  std::string card_name;    // e.g. "USB3 Video"
  std::string device_name;  // e.g. "USB Audio"
  bool capture = false;
  bool playback = false;
};

// Enumerates ALSA cards x PCM devices x streams via the libasound ctl API.
std::vector<AudioDevice> list_audio_devices();

}  // namespace subtitler::probe
