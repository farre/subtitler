#pragma once

#include <string>
#include <vector>

#include "probe/audio.h"
#include "probe/capture.h"
#include "probe/drm.h"
#include "probe/pipeline.h"
#include "probe/plugins.h"

namespace subtitler::probe {

void print_devices_text(const std::vector<VideoDevice>& devices);
void print_modes_text(const std::string& path,
                      const std::vector<VideoMode>& modes);
void print_audio_text(const std::vector<AudioDevice>& devices);
void print_elements_text(const std::vector<ElementAvailability>& elements);
void print_drm_text(const DrmInfo& info);
void print_pipeline_text(const PipelinePlan& plan);

std::string devices_to_json(const std::vector<VideoDevice>& devices,
                            const std::vector<std::vector<VideoMode>>& modes);
std::string modes_to_json(const std::vector<VideoMode>& modes);
std::string audio_to_json(const std::vector<AudioDevice>& devices,
                          bool capture);
std::string elements_to_json(const std::vector<ElementAvailability>& elements);
std::string drm_to_json(const DrmInfo& info);
std::string pipeline_to_json(const PipelinePlan& plan);

}  // namespace subtitler::probe
