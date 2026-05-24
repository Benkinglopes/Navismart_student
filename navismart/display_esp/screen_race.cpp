#include "screen_race.h"

#include <iostream>

void screen_race_show(const MainData &mainData, bool recordingEnabled) {
    std::cout << "[Display ESP] ScreenRace: sog=" << mainData.sog
              << " heel=" << mainData.heel
              << " trim=" << mainData.trim
              << " pitch=" << mainData.pitch
              << " time=" << mainData.time
              << " recording=" << (recordingEnabled ? "on" : "waiting")
              << std::endl;
}
