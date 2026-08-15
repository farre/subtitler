#include "probe/audio.h"

#include <alsa/asoundlib.h>

#include <format>
#include <memory>
#include <utility>

namespace {

struct CtlDeleter {
  void operator()(snd_ctl_t* ctl) const { snd_ctl_close(ctl); }
};

struct CardInfoDeleter {
  void operator()(snd_ctl_card_info_t* info) const {
    snd_ctl_card_info_free(info);
  }
};

struct PcmInfoDeleter {
  void operator()(snd_pcm_info_t* info) const { snd_pcm_info_free(info); }
};

using CtlPtr = std::unique_ptr<snd_ctl_t, CtlDeleter>;
using CardInfoPtr = std::unique_ptr<snd_ctl_card_info_t, CardInfoDeleter>;
using PcmInfoPtr = std::unique_ptr<snd_pcm_info_t, PcmInfoDeleter>;

}  // namespace

namespace subtitler::probe {

std::vector<AudioDevice> ListAudioDevices() {
  std::vector<AudioDevice> devices;

  int card = -1;
  while (snd_card_next(&card) == 0 && card >= 0) {
    const auto ctl_name = std::format("hw:{}", card);

    snd_ctl_t* raw_ctl = nullptr;
    if (snd_ctl_open(&raw_ctl, ctl_name.c_str(), 0) < 0) {
      continue;
    }
    const CtlPtr ctl{raw_ctl};

    snd_ctl_card_info_t* raw_card_info = nullptr;
    if (snd_ctl_card_info_malloc(&raw_card_info) < 0) {
      continue;
    }
    const CardInfoPtr card_info{raw_card_info};

    if (snd_ctl_card_info(ctl.get(), card_info.get()) < 0) {
      continue;
    }

    int device = -1;
    while (snd_ctl_pcm_next_device(ctl.get(), &device) == 0 && device >= 0) {
      AudioDevice entry;
      entry.card = card;
      entry.device = device;
      entry.card_id = snd_ctl_card_info_get_id(card_info.get());
      entry.card_name = snd_ctl_card_info_get_name(card_info.get());

      for (const snd_pcm_stream_t stream :
           {SND_PCM_STREAM_CAPTURE, SND_PCM_STREAM_PLAYBACK}) {
        snd_pcm_info_t* raw_pcm_info = nullptr;
        if (snd_pcm_info_malloc(&raw_pcm_info) < 0) {
          continue;
        }
        const PcmInfoPtr pcm_info{raw_pcm_info};

        snd_pcm_info_set_device(pcm_info.get(), device);
        snd_pcm_info_set_subdevice(pcm_info.get(), 0);
        snd_pcm_info_set_stream(pcm_info.get(), stream);

        if (snd_ctl_pcm_info(ctl.get(), pcm_info.get()) < 0 ||
            snd_pcm_info_get_subdevices_count(pcm_info.get()) == 0) {
          continue;
        }

        if (entry.device_name.empty()) {
          entry.device_name = snd_pcm_info_get_name(pcm_info.get());
        }

        if (stream == SND_PCM_STREAM_CAPTURE) {
          entry.capture = true;
        } else {
          entry.playback = true;
        }
      }

      if (entry.capture || entry.playback) {
        devices.push_back(std::move(entry));
      }
    }
  }

  return devices;
}

}  // namespace subtitler::probe
