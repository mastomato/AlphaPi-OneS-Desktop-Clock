#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "stations.h"

namespace radio {

// Lightweight persistence for the xiaozhi-integrated radio. The standalone
// radio's online catalog / JSON import is intentionally not carried over:
// xiaozhi already owns Wi-Fi provisioning and the release firmware only ever
// shipped with the built-in list.
void StationStoreInit();
size_t StationCount();
Station StationAt(size_t index);
std::vector<Station> StationsSnapshot();

// Remember the last tuned station across reboots (NVS namespace "radio").
void SaveStationIndex(size_t index);
size_t LoadStationIndex();

// Volume is applied to the audio coprocessor through AudioUart::SetVolume.
void SaveVolume(int percent);
int LoadVolume();

}  // namespace radio
