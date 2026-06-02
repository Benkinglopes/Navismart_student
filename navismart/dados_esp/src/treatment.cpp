#include "treatment.h"

void process_data(const RawData &raw, MainData &outMain, CompData &outComp) {
    outMain.sog = raw.sog;
    outMain.heel = raw.roll;
    outMain.trim = raw.heading;
    outMain.pitch = raw.pitch;
    outMain.time = raw.timestamp_ms / 1000;

    outComp.cog = raw.cog;
    outComp.latitude = static_cast<float>(raw.latitude);
    outComp.longitude = static_cast<float>(raw.longitude);
}
