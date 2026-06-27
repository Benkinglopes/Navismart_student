#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include "SerialTransfer.h"
#include "struct.h"

SerialTransfer myTransfer;
MainData dadosBarco;

// --- PINOS DE RF24 ---
#define CE_PIN  D2
#define CSN_PIN D3
#define RF24_SCK D8
#define RF24_MISO D1
#define RF24_MOSI D10

const byte radioAddress[5] = {'B','A','R','C','1'};
RF24 radio(CE_PIN, CSN_PIN);
bool radioOk = false;

HardwareSerial SerialS3(1);

// ==========================================
// 2. SETUP (INICIALIZAÇÃO)
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configurar Serial para enviar ao S3
  SerialS3.begin(460800, SERIAL_8N1, -1, 21);
  myTransfer.begin(SerialS3);

  // Iniciar RF24 como receptor de telemetria
  SPI.begin(RF24_SCK, RF24_MISO, RF24_MOSI);
  if (!radio.begin()) {
    Serial.println("[ERRO] RF24 não detectado!");
  } else {
    radio.setDataRate(RF24_250KBPS);
    radio.setChannel(115);
    radio.setPALevel(RF24_PA_MAX);
    radio.openReadingPipe(0, radioAddress);
    radio.startListening();
    radioOk = true;
    Serial.println("[RF24] Pronto para receber.");
  }
}

// ==========================================
// 3. LOOP (RECEPÇÃO E REPASSO)
// ==========================================
void loop() {
  if (radioOk && radio.available()) {
    while (radio.available()) {
      radio.read(&dadosBarco, sizeof(dadosBarco));
      Serial.println("[RF24] Pacote recebido do transmissor.");

      uint16_t sendSize = myTransfer.txObj(dadosBarco, 0);
      myTransfer.sendData(sendSize);
      Serial.println("[SerialTransfer] Pacote enviado para o S3.");
    }
  }

  delay(10);
}
