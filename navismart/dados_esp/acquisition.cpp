#include "acquisition.h"

#if defined(ARDUINO)
#include <Adafruit_BNO055.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <Preferences.h>
#include <TinyGPS++.h>
#include <Wire.h>

#define SDA_PIN D4
#define SCL_PIN D5
#define PINO_RX 20
#define PINO_TX 21

static TinyGPSPlus gps;
static Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28, &Wire);
static Preferences memoriaBarco;
static bool calibGuardada = false;
static bool apagarCalibracaoAntiga = false;
static RawData estadoBarco{};

static void reset_estado_barco() {
    estadoBarco = RawData{};
}

void acquisition_init() {
    Serial.begin(115200);
    Serial1.begin(9600, SERIAL_8N1, PINO_RX, PINO_TX);
    delay(3000);

    Serial.println("\n\n--- SISTEMA DE NAVEGACAO - DADOS ESP ---");
    reset_estado_barco();

    Wire.begin(SDA_PIN, SCL_PIN);
    if (!bno.begin()) {
        Serial.println("[ERRO] BNO055 nao detetado!");
        while (1) {
            delay(1000);
        }
    }
    bno.setExtCrystalUse(true);

    memoriaBarco.begin("bno055", false);
    if (apagarCalibracaoAntiga) {
        Serial.println("[ALERTA] A limpar memoria Flash...");
        memoriaBarco.clear();
    }

    if (memoriaBarco.getBytesLength("calib") == sizeof(adafruit_bno055_offsets_t)) {
        Serial.println("=> [SUCESSO] Calibracao de hardware carregada da Flash!");
        adafruit_bno055_offsets_t offsetsGuardados;
        memoriaBarco.getBytes("calib", &offsetsGuardados, sizeof(offsetsGuardados));
        bno.setSensorOffsets(offsetsGuardados);
        calibGuardada = true;
    } else {
        Serial.println("\n*** MODO DE CALIBRACAO ***");
        Serial.println("Faz movimentos em '8' no ar com o sensor.");

        uint8_t sys, gyro, accel, mag;
        while (true) {
            bno.getCalibration(&sys, &gyro, &accel, &mag);
            Serial.print("A Calibrar Mag: ");
            Serial.print(mag);
            Serial.println("/3");
            if (mag == 3) {
                break;
            }
            delay(300);
        }

        adafruit_bno055_offsets_t novosOffsets;
        bno.getSensorOffsets(novosOffsets);
        memoriaBarco.putBytes("calib", &novosOffsets, sizeof(novosOffsets));
        calibGuardada = true;
        Serial.println("\n*** CALIBRACAO GRAVADA NA FLASH! ***");
    }

    Serial.println("--- PRONTO A NAVEGAR ---");
}

void acquisition_poll() {
    while (Serial1.available() > 0) {
        gps.encode(Serial1.read());
    }
}

RawData acquire_sample(uint32_t sample_time) {
    acquisition_poll();

    estadoBarco.timestamp_ms = sample_time;
    estadoBarco.gps_synced = gps.location.isValid();
    if (estadoBarco.gps_synced) {
        estadoBarco.latitude = gps.location.lat();
        estadoBarco.longitude = gps.location.lng();
    }
    estadoBarco.sog = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
    estadoBarco.cog = gps.course.isValid() ? gps.course.deg() : 0.0f;

    sensors_event_t event;
    bno.getEvent(&event);

    uint8_t sys, gyro, accel, mag;
    bno.getCalibration(&sys, &gyro, &accel, &mag);

    float headingCorrigido = event.orientation.x + 90.0f;
    if (headingCorrigido < 0.0f) {
        headingCorrigido += 360.0f;
    }
    if (headingCorrigido >= 360.0f) {
        headingCorrigido -= 360.0f;
    }

    estadoBarco.heading = headingCorrigido;
    estadoBarco.roll = event.orientation.y;
    estadoBarco.pitch = event.orientation.z;
    estadoBarco.calib_mag = mag;

    if (mag == 3 && !calibGuardada) {
        adafruit_bno055_offsets_t novosOffsets;
        bno.getSensorOffsets(novosOffsets);
        memoriaBarco.putBytes("calib", &novosOffsets, sizeof(novosOffsets));
        calibGuardada = true;
    }

    return estadoBarco;
}

#else

void acquisition_init() {}

void acquisition_poll() {}

RawData acquire_sample(uint32_t sample_time) {
    RawData r{};
    r.timestamp_ms = sample_time * 1000;
    r.latitude = 0.0;
    r.longitude = 0.0;
    r.sog = 0.0f;
    r.cog = 0.0f;
    r.heading = 0.0f;
    r.calib_mag = 0;
    r.roll = 0.0f;
    r.pitch = 0.0f;
    r.gps_synced = false;
    return r;
}

#endif
