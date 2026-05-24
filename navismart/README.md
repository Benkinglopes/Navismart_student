# Navismart

Navismart is split into two ESP firmwares:

- `dados_esp`: reads GPS/IMU data, treats it, and sends compact navigation structs.
- `display_esp`: receives the treated data, shows it on the display, and records values only after race mode has been active for 5 minutes.

The shared payloads sent to the display are defined in `display_esp/struct.h`.

## Project Layout

```text
dados_esp/
  acquisition.*   GPS, BNO055 and calibration acquisition
  treatment.*     Converts RawData into MainData and CompData
  comms.*         Sends data to the display ESP
  main.cpp        Dados ESP firmware entry point / desktop simulator

display_esp/
  struct.h        Shared MainData and CompData payload structs
  screen_start.*  Start screen behavior
  screen_race.*   Race screen behavior and MainData presentation
  writer.*        Record writer used after the 5 minute race delay
  display.cpp     Arduino/LVGL display firmware code
  main.cpp        Desktop simulator for the display flow
  ui/             Generated LVGL UI files
```

## Data Flow

1. `dados_esp` acquires raw GPS and BNO055 values into `RawData`.
2. `treatment.cpp` maps `RawData` into:
   - `MainData`: speed, heel, trim, pitch and time.
   - `CompData`: course, latitude and longitude.
3. `comms.cpp` sends those structs to the display ESP.
4. `display_esp` starts on `ScreenStart`.
5. Pressing the play button switches to `ScreenRace`.
6. `ScreenRace` displays `MainData` immediately.
7. Recording starts only after `ScreenRace` has been active for 5 minutes.

## Desktop Build Checks

The CMake files are for local validation/simulation, not final board upload.

```sh
cmake -S dados_esp -B dados_esp/build
cmake --build dados_esp/build

cmake -S display_esp -B display_esp/build
cmake --build display_esp/build
```

On Windows with WSL:

```sh
wsl.exe -d Ubuntu-22.04 -- bash -lc "cd /mnt/c/Users/berna/Desktop/navismart && cmake -S dados_esp -B dados_esp/build && cmake --build dados_esp/build"
wsl.exe -d Ubuntu-22.04 -- bash -lc "cd /mnt/c/Users/berna/Desktop/navismart && cmake -S display_esp -B display_esp/build && cmake --build display_esp/build"
```

## Board Builds

`display_esp/platformio.ini` contains the current PlatformIO configuration for the ESP32-S3 display board.

The `dados_esp` Arduino dependencies are:

- Arduino framework
- TinyGPS++
- Adafruit BNO055
- Adafruit Unified Sensor
- Preferences
- RF24

## GitHub Notes

Generated folders such as `build/`, `.pio/`, firmware binaries and local CSV records are ignored. The old local reference folder `navismart-displays-main/` is also ignored so the repository stays focused on the current two-firmware structure.
