#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <chrono>
#include <iostream>
#include <thread>
#endif

#include "acquisition.h"
#include "comms.h"
#include "treatment.h"

#if defined(ARDUINO)
static uint32_t ultimoEnvio = 0;

void setup() {
    acquisition_init();
}

void loop() {
    acquisition_poll();

    if (millis() - ultimoEnvio > 1000) {
        ultimoEnvio = millis();

        RawData raw = acquire_sample(ultimoEnvio);
        MainData mainData{};
        CompData compData{};

        process_data(raw, mainData, compData);
        comms_send(mainData, compData);

        Serial.println("\n[========== PAINEL DE TELEMETRIA ==========]");
        Serial.print("Timestamp:   ");
        Serial.println(raw.timestamp_ms);
        Serial.print("Heading (X): ");
        Serial.print(raw.heading, 1);
        Serial.println(" deg");
        Serial.print("Roll:        ");
        Serial.print(raw.roll, 1);
        Serial.println(" deg");
        Serial.print("Pitch:       ");
        Serial.print(raw.pitch, 1);
        Serial.println(" deg");
        Serial.print("Calib. Mag:  ");
        Serial.print(raw.calib_mag);
        Serial.println("/3");
        Serial.print("Latitude:    ");
        Serial.println(raw.latitude, 6);
        Serial.print("Longitude:   ");
        Serial.println(raw.longitude, 6);
        Serial.print("SOG (Vel):   ");
        Serial.print(raw.sog, 1);
        Serial.println(" km/h");
        Serial.print("COG (Rumo):  ");
        Serial.print(raw.cog, 1);
        Serial.println(" deg");
        Serial.println("[==========================================]");
    }
}
#else
int main() {
    acquisition_init();

    for (uint32_t t = 0; t < 10; ++t) {
        RawData raw = acquire_sample(t);
        MainData mainData{};
        CompData compData{};

        process_data(raw, mainData, compData);
        comms_send(mainData, compData);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "[Dados ESP] Loop completed." << std::endl;
    return 0;
}
#endif
