#include "screen_race.h"
#include "ui/ui.h"

#include <iostream>

void screen_race_show(const MainData &mainData, bool recordingEnabled) {
    // Console log for debugging
    std::cout << "[Display ESP] ScreenRace: sog=" << mainData.sog
              << " heel=" << mainData.heel
              << " trim=" << mainData.trim
              << " pitch=" << mainData.pitch
              << " time=" << mainData.time
              << " recording=" << (recordingEnabled ? "on" : "waiting")
              << std::endl;

    // Update LVGL labels if screen objects exist
    if (ui_ValKTS) {
        lv_label_set_text_fmt(ui_ValKTS, "%.1f", mainData.sog);
    }
    if (ui_ValHEEL) {
        lv_label_set_text_fmt(ui_ValHEEL, "%.0f", mainData.heel);
    }
    if (ui_ValTRIM) {
        lv_label_set_text_fmt(ui_ValTRIM, "%.0f", mainData.trim);
    }
    // Use pitch to display on HDG label if available (fallback)
    if (ui_ValHDG) {
        lv_label_set_text_fmt(ui_ValHDG, "%.0f", mainData.pitch);
    }
}
