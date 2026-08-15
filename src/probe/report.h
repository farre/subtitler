#pragma once

#include <string>
#include <vector>

#include "probe/audio.h"
#include "probe/capture.h"
#include "probe/drm.h"
#include "probe/pipeline.h"
#include "probe/plugins.h"

namespace subtitler::probe {

void PrintDevicesText(const std::vector<VideoDevice>& devices);
void PrintModesText(const std::string& path,
                    const std::vector<VideoMode>& modes);
void PrintAudioText(const std::vector<AudioDevice>& devices);
void PrintElementsText(const std::vector<ElementAvailability>& elements);
void PrintDrmText(const DrmInfo& info);
void PrintPipelineText(const PipelinePlan& plan);

std::string DevicesToJson(const std::vector<VideoDevice>& devices,
                          const std::vector<std::vector<VideoMode>>& modes);
std::string ModesToJson(const std::vector<VideoMode>& modes);
std::string AudioToJson(const std::vector<AudioDevice>& devices, bool capture);
std::string ElementsToJson(const std::vector<ElementAvailability>& elements);
std::string DrmToJson(const DrmInfo& info);
std::string PipelineToJson(const PipelinePlan& plan);

}  // namespace subtitler::probe
