#pragma once
#include <cstdint>

struct RawData {
    uint32_t timestamp_ms;
    double latitude;
    double longitude;
    float sog;
    float cog;
    float heading;
    uint8_t calib_mag;
    float roll;
    float pitch;
    bool gps_synced;
};

void acquisition_init();
void acquisition_poll();
RawData acquire_sample(uint32_t sample_time);
