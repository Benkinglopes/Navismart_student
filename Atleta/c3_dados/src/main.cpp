#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <TinyGPS++.h>
#include <Preferences.h>
#include <SPI.h>
#include <RF24.h>

// =========================================================
// 1. BIBLIOTECAS DE COMUNICAÇÃO (A NOSSA PONTE)
// =========================================================
#include "SerialTransfer.h"
#include "struct.h"

SerialTransfer myTransfer;
TelemetryPayload dadosBarco; // Esta é a encomenda oficial que vai para o S3!
MainData AntenaDados; // Esta é a estrutura que vamos usar para ler os dados do GPS e do BNO055 antes de os colocar na encomenda oficial

// --- PINOS DE RF24 ---
#define CE_PIN  D2
#define CSN_PIN D3
#define RF24_SCK D8
#define RF24_MISO D1
#define RF24_MOSI D10

const byte radioAddress[6] = "BARC1";
RF24 radio(CE_PIN, CSN_PIN);
bool radioOk = false;

// --- PINOS DE HARDWARE ---
#define SDA_PIN D4
#define SCL_PIN D5
#define PINO_RX 20 
#define PINO_TX 21 

// Criamos uma porta específica para o S3 (UART0) e deixamos a Serial1 para o GPS
HardwareSerial SerialS3(0); 

// --- OBJETOS DE SISTEMA ---
TinyGPSPlus gps;
Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28, &Wire);
Preferences memoriaBarco;

unsigned long ultimoPrint = 0;
bool calibGuardada = false;
bool APAGAR_CALIBRACAO_ANTIGA = false; 

// ==========================================
// 2. SETUP (INICIALIZAÇÃO)
// ==========================================
void setup() {
  Serial.begin(115200); // Monitor Serial do PC (via USB)
  
  
  // 1. Ouve o GPS no D7 (GPIO 20). Desliga o TX (-1)
  Serial1.begin(9600, SERIAL_8N1, 20, -1);
  
  // 2. Fala com o S3 pelo D6 (GPIO 21). Desliga o RX (-1)
  SerialS3.begin(460800, SERIAL_8N1, -1, 21);
  myTransfer.begin(SerialS3);

  delay(3000); 
  Serial.println("\n\n--- SISTEMA DE NAVEGAÇÃO E TELEMETRIA INICIADO ---");

  // Iniciar rádio RF24 para transmissão de telemetria
  SPI.begin(RF24_SCK, RF24_MISO, RF24_MOSI);
  if (!radio.begin()) {
    Serial.println("[ERRO] RF24 não detectado!");
  } else {
    radio.setDataRate(RF24_250KBPS);
    radio.setChannel(115);
    radio.setPALevel(RF24_PA_MAX);
    radio.openWritingPipe(radioAddress);
    radio.stopListening();
    radioOk = true;
    Serial.println("[RF24] Pronto para transmitir.");
  }

  // Iniciar BNO055
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!bno.begin()) {
    Serial.println("[ERRO] BNO055 não detetado!");
    while (1); 
  }
  bno.setExtCrystalUse(true);

  // Iniciar a Memória Flash
  memoriaBarco.begin("bno055", false);
  if (APAGAR_CALIBRACAO_ANTIGA) {
    Serial.println("[ALERTA] A limpar memória Flash...");
    memoriaBarco.clear();
  }

  // Carregar calibração ou forçar uma nova
  if (memoriaBarco.getBytesLength("calib") == sizeof(adafruit_bno055_offsets_t)) {
    Serial.println("=> [SUCESSO] Calibração de hardware carregada da Flash!");
    adafruit_bno055_offsets_t offsetsGuardados;
    memoriaBarco.getBytes("calib", &offsetsGuardados, sizeof(offsetsGuardados));
    bno.setSensorOffsets(offsetsGuardados);
    calibGuardada = true;
  } else {
    Serial.println("\n*** MODO DE CALIBRAÇÃO ***");
    Serial.println("Faz movimentos em '8' no ar com o sensor.");
    uint8_t sys, gyro, accel, mag;
    while (true) {
      bno.getCalibration(&sys, &gyro, &accel, &mag);
      Serial.print("A Calibrar Mag: "); Serial.print(mag); Serial.println("/3");
      if (mag == 3) break;
      delay(300);
    }
    adafruit_bno055_offsets_t novosOffsets;
    bno.getSensorOffsets(novosOffsets);
    memoriaBarco.putBytes("calib", &novosOffsets, sizeof(novosOffsets));
    calibGuardada = true;
    Serial.println("\n*** CALIBRAÇÃO GRAVADA NA FLASH! ***");
  }
}

// ==========================================
// 3. LOOP (AQUISIÇÃO E ENVIO)
// ==========================================
void loop() {
  // --- A. ADQUIRIR DADOS GPS CONSTANTEMENTE ---
  while (Serial1.available() > 0) {
    gps.encode(Serial1.read());
  }

  // --- B. ATUALIZAR, EMPACOTAR E ENVIAR (A cada 100ms para o ecrã ser rápido) ---
  if (millis() - ultimoPrint > 100) { // Baixei para 100ms para os ponteiros serem fluidos!
    ultimoPrint = millis();

    // 1. Extrair dados do BNO055
    sensors_event_t event;
    bno.getEvent(&event);
    
    // Matemática do Rumo
    float headingCorrigido = event.orientation.x + 90.0;
    if (headingCorrigido < 0.0) headingCorrigido += 360.0;
    if (headingCorrigido >= 360.0) headingCorrigido -= 360.0;

    // 2. GUARDAR NA ESTRUTURA OFICIAL PARA O S3
    dadosBarco.main.time = millis();
    
    // GPS
    dadosBarco.main.sog = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
    dadosBarco.comp.cog = gps.course.isValid() ? gps.course.deg() : 0.0;

    dadosBarco.main.heading = headingCorrigido;
    if (gps.location.isValid()) {
      dadosBarco.comp.latitude = gps.location.lat();
      dadosBarco.comp.longitude = gps.location.lng();
    } else {
      dadosBarco.comp.latitude = 0.0;
      dadosBarco.comp.longitude = 0.0;
    }

    // BNO055 (Mapear Roll para Heel, e Pitch para Trim)
    dadosBarco.main.heel = event.orientation.y; 
    dadosBarco.main.trim = event.orientation.z; 

    // 3. ENVIAR PELO CABO PARA O S3!
    uint16_t sendSize = 0;
    sendSize = myTransfer.txObj(dadosBarco, sendSize);
    myTransfer.sendData(sendSize);

    // 4. Transmitir também por RF24 para um segundo ESP
    if (radioOk) {
      if (!radio.write(&dadosBarco.main, sizeof(dadosBarco.main))) {
        Serial.println("[RF24] Envio falhou!");
      }
    }

    // 5. Imprimir no PC (Apenas de 1 em 1 segundo para não encher o terminal)
    static unsigned long ultimoLogPC = 0;
    if (millis() - ultimoLogPC > 1000) {
        ultimoLogPC = millis();
        Serial.println("\n[========== DADOS ENVIADOS PARA O S3 ==========]");
        Serial.print("Velocidade (SOG):  "); Serial.print(dadosBarco.main.sog, 1); Serial.println(" km/h");
        Serial.print("Inclinação (Heel): "); Serial.print(dadosBarco.main.heel, 1); Serial.println(" deg");
        Serial.print("Orientação (HDG):  "); Serial.print(headingCorrigido, 1); Serial.println(" deg"); // <-- A TUA BÚSSOLA AQUI!
        Serial.print("Rumo GPS (COG):    "); Serial.print(dadosBarco.comp.cog, 1); Serial.println(" deg");
        
        if (!gps.location.isValid()) {
            Serial.println("[AVISO] GPS à procura de satélites...");
        }
    }
  }
}