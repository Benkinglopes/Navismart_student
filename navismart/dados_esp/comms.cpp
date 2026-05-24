#include "comms.h"

#if defined(ARDUINO)
#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#define CE_PIN  D2
#define CSN_PIN D3

RF24 radio(CE_PIN, CSN_PIN);

const byte addressMain[6] = "MAIN1";
const byte addressComp[6] = "COMP1";

static bool radioReady = false;

static void comms_init_radio() {
    if (radioReady) {
        return;
    }

    // SCK = D8, MISO = D1, MOSI = D10 on the XIAO ESP32-C3.
    SPI.begin(D8, D1, D10);

    if (!radio.begin()) {
        Serial.println("[ERRO] Radio nRF24L01 nao detetado!");
        while (1) {
            delay(1000);
        }
    }

    radio.setDataRate(RF24_250KBPS);
    radio.setChannel(115);
    radio.setPALevel(RF24_PA_MIN);
    radio.stopListening();

    radioReady = true;
}
#else
#include <iostream>
#endif

void comms_send(const MainData &m, const CompData &c) {
#if defined(ARDUINO)
    comms_init_radio();

    radio.openWritingPipe(addressMain);
    bool sucMain = radio.write(&m, sizeof(m));

    radio.openWritingPipe(addressComp);
    bool sucComp = radio.write(&c, sizeof(c));

    if (!sucMain) {
        Serial.println("[AVISO] Falha ao enviar MainData.");
    }
    if (!sucComp) {
        Serial.println("[AVISO] Falha ao enviar CompData.");
    }
#else
    // Desktop/demo build: replace with Arduino RF24 when compiling for the ESP.
    std::cout << "[Dados ESP] Sending data: sog=" << m.sog
              << " heel=" << m.heel
              << " lat=" << c.latitude
              << " lon=" << c.longitude
              << std::endl;
#endif
}

bool comms_receive(MainData &outMain, CompData &outComp) {
    // TODO: replace with actual radio receive logic.
    // Stub returns a sample payload for display demo.
    outMain.sog = 1.2;
    outMain.heel = 3.4;
    outMain.trim = 0.5;
    outMain.pitch = 1.0;
    outMain.time = 1;
    outComp.cog = 45.0;
    outComp.latitude = 12.34;
    outComp.longitude = 56.78;
    return true;
}
