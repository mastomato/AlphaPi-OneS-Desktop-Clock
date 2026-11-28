#include "station_store.h"

#include <algorithm>

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace radio {
namespace {

constexpr char kTag[] = "RadioStore";
constexpr char kNamespace[] = "radio";

bool EnsureNvs() {
    static bool initialized = false;
    if (initialized) return true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Only erase the NVS partition, never the whole flash.
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs init failed: %s", esp_err_to_name(err));
        return false;
    }
    initialized = true;
    return true;
}

template <typename T>
T Read(const char* key, T fallback) {
    if (!EnsureNvs()) return fallback;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return fallback;
    T value = fallback;
    nvs_get_i32(handle, key, reinterpret_cast<int32_t*>(&value));
    nvs_close(handle);
    return value;
}

void Write(const char* key, int32_t value) {
    if (!EnsureNvs()) return;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_i32(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace

void StationStoreInit() { EnsureNvs(); }

size_t StationCount() { return BuiltinStationCount(); }

Station StationAt(size_t index) {
    const size_t count = BuiltinStationCount();
    if (count == 0) return {"", "", ""};
    return BuiltinStations()[index % count];
}

std::vector<Station> StationsSnapshot() {
    std::vector<Station> out;
    out.reserve(BuiltinStationCount());
    for (size_t i = 0; i < BuiltinStationCount(); ++i) out.push_back(BuiltinStations()[i]);
    return out;
}

void SaveStationIndex(size_t index) {
    Write("station", static_cast<int32_t>(index % BuiltinStationCount()));
}

size_t LoadStationIndex() {
    int32_t value = Read<int32_t>("station", 0);
    if (value < 0) value = 0;
    return static_cast<size_t>(value) % BuiltinStationCount();
}

void SaveVolume(int percent) { Write("volume", std::clamp(percent, 0, 100)); }

// Read<int32_t> returns int32_t (= long int on xtensa), so clamp's template
// argument has to be spelled out: std::clamp(long, int, int) does not deduce.
int LoadVolume() { return static_cast<int>(std::clamp<int32_t>(Read<int32_t>("volume", 67), 0, 100)); }

}  // namespace radio
