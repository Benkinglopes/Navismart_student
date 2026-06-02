#include <chrono>
#include <iostream>
#include <thread>

#include "screen_race.h"
#include "screen_start.h"
#include "struct.h"
#include "writer.h"

namespace {
constexpr auto RECORD_DELAY = std::chrono::minutes(5);
constexpr auto LOOP_DELAY = std::chrono::seconds(1);

enum class DisplayScreen {
    ScreenStart,
    ScreenRace
};
}

bool receive_data(MainData &outMain, CompData &outComp) {
    // TODO: replace with UART/RF receive from Dados ESP.
    outMain.sog = 1.2f;
    outMain.heel = 3.4f;
    outMain.trim = 45.0f;
    outMain.pitch = 1.0f;
    outMain.time = 1;
    outComp.cog = 45.0f;
    outComp.latitude = 12.34f;
    outComp.longitude = 56.78f;
    return true;
}

static bool play_button_pressed(uint32_t loopCount) {
    // Desktop simulation for the ScreenStart play button.
    return loopCount == 1;
}

int main() {
    DisplayScreen screen = DisplayScreen::ScreenStart;
    auto raceStartedAt = std::chrono::steady_clock::time_point{};

    screen_start_show();

    for (uint32_t loopCount = 0; loopCount < 10; ++loopCount) {
        if (screen == DisplayScreen::ScreenStart) {
            if (play_button_pressed(loopCount)) {
                screen = DisplayScreen::ScreenRace;
                raceStartedAt = std::chrono::steady_clock::now();
                std::cout << "[Display ESP] Play pressed. Switching to ScreenRace." << std::endl;
            }
        }

        if (screen == DisplayScreen::ScreenRace) {
            MainData mainData{};
            CompData compData{};

            if (receive_data(mainData, compData)) {
                const bool recordingEnabled =
                    std::chrono::steady_clock::now() - raceStartedAt >= RECORD_DELAY;

                screen_race_show(mainData, recordingEnabled);
                if (recordingEnabled) {
                    writer_write(mainData, compData);
                }
            } else {
                std::cout << "[Display ESP] Waiting for data from Dados ESP..." << std::endl;
            }
        }

        std::this_thread::sleep_for(LOOP_DELAY);
    }

    std::cout << "[Display ESP] Loop completed." << std::endl;
    return 0;
}
