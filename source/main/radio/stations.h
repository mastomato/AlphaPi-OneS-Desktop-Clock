#pragma once

#include <cstddef>
#include <string>

namespace radio {

struct Station {
    std::string name;
    std::string tag;
    std::string url;
};

const Station* BuiltinStations();
size_t BuiltinStationCount();
size_t StationCount();

}  // namespace radio
