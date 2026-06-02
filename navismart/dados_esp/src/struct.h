#pragma once
#include <cstdint>

struct MainData {
    float sog; // speed over ground
    float heel;
    float trim;
    float pitch;
    uint32_t time;
};

struct CompData {
    float cog; // course over ground
    float latitude;
    float longitude;
};

