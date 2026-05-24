#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <utility/imumaths.h>
#include <TinyGPS++.h>
#include <SPI.h>
#include <RF24.h>
#include <Preferences.h> // Biblioteca para salvar na Flash

// --- PINOS DO GPS ---
#define PINO_RX 20 
#define PINO_TX 21 

// --- PINOS DO RÁDIO (NRF24L01) ---
#define CE_PIN  D2
#define CSN_PIN D3

// --- OBJETOS ---
TinyGPSPlus gps;
Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28, &Wire);
RF24 radio(CE_PIN, CSN_PIN);
Preferences prefs; // Objeto para gerir a memória Flash

// --- ENDEREÇO DE RÁDIO ---
const byte address[6] = "BARC1";

// --- A MEGA ESTRUTURA DE DADOS (Payload: 24 Bytes) ---
struct DadosNavegacao {
  uint32_t timestamp;  // Tempo em Segundos
  float cog;           // Rumo real (GPS)
  float sog;           // Velocidade real (GPS)
  float roll;          // Inclinação lateral (BNO)
  float pitch;         // Inclinação frontal (BNO)
  float heading;       // Bússola/Proa (BNO)
};

DadosNavegacao estadoBarco;

// --- VARIÁVEIS DE CONTROLO ---
bool sensorLigado = false;
float taraY = 0;
float taraZ = 0;
bool taraFeita = false;
unsigned long ultimoPrint = 0;

// =========================================================================
// OS BOTÕES DE CONFIGURAÇÃO (Altera aqui o que precisas)

// 1. Alternar entre Atleta e Treinador
bool souAtleta = true; // true = Emissor com Sensores | false = Recetor

// 2. BOTÃO DE LIMPEZA DA BÚSSOLA
// Muda para 'true' UMA VEZ para apagar a calibração má. 
// Depois de calibrar bem, volta a meter 'false' e faz upload de novo!
bool forcarRecalibracao = false; 
// =========================================================================

void setup() {
  Serial.begin(115200);
  delay(3000); 
  
  if (souAtleta) {
    Serial.println("\n=== INICIAR ATLETA (MODO INTELIGENTE COM FLASH) ===");
    Serial1.begin(9600, SERIAL_8N1, PINO_RX, PINO_TX); 
    Wire.begin(D4, D5); 
    Wire.setClock(50000); // Evita o Erro -1 do I2C

    if (!bno.begin()) {
      Serial.println("[ERRO] BNO055 não detetado!");
      while (1);
    }
    sensorLigado = true;

    // --- LÓGICA DE CALIBRAÇÃO COM PREFERENCES ---
    prefs.begin("bno_storage", false); // Abre a "gaveta" na memória

    // ⚠️ O NOSSO CÓDIGO DE LIMPEZA:
    if (forcarRecalibracao) {
      prefs.remove("calib_dados"); // Destrói o ficheiro antigo!
      Serial.println("\n[!!!] MEMÓRIA APAGADA PROPOSITADAMENTE [!!!]\n");
    }

    if (prefs.isKey("calib_dados")) {
      adafruit_bno055_offsets_t calibData;
      prefs.getBytes("calib_dados", &calibData, sizeof(calibData));
      bno.setSensorOffsets(calibData);
      Serial.println("[FLASH] Calibração antiga carregada! Pronto a navegar.");
    } else {
      Serial.println("[AVISO] Nenhuma calibração na Flash. INICIA O MOVIMENTO EM 8!");
      uint8_t system, gyro, accel, mag = 0;
      
      // Bloqueia aqui até o sistema e o magnetómetro estarem calibrados (Nível 3)
      while (mag < 3 || system < 3) {
        bno.getCalibration(&system, &gyro, &accel, &mag);
        Serial.print("A Calibrar... Mag: "); Serial.print(mag); 
        Serial.print(" | Sistema: "); Serial.println(system);
        delay(1000);
      }

      // Quando estiver calibrado, guarda os 22 bytes na Flash
      adafruit_bno055_offsets_t novosOffsets;
      bno.getSensorOffsets(novosOffsets);
      prefs.putBytes("calib_dados", &novosOffsets, sizeof(novosOffsets));
      Serial.println("[FLASH] Nova calibração GRAVADA com sucesso!");
    }
    prefs.end();
    bno.setExtCrystalUse(true);

  } else {
    Serial.println("\n=== INICIAR TREINADOR (RECETOR DE TELEMETRIA) ===");
  }

  // 2. INICIAR O RÁDIO (MISO no D1 para segurança)
  SPI.begin(D8, D1, D10); 
  if (!radio.begin()) {
    Serial.println("[ERRO] Radio não encontrado!");
    while (1); 
  }
  
  radio.setDataRate(RF24_250KBPS); 
  radio.setChannel(115);
  radio.setPALevel(RF24_PA_MAX); // Muda para PA_MIN em testes de bancada próximos

  if (souAtleta) {
    radio.openWritingPipe(address);
    radio.stopListening(); 
    Serial.println("-> Rádio configurado para EMITIR.");
  } else {
    radio.openReadingPipe(0, address);
    radio.startListening(); 
    Serial.println("-> Rádio configurado para ESCUTAR.");
  }

  memset(&estadoBarco, 0, sizeof(estadoBarco));
}

void loop() {
  if (souAtleta) {
    while (Serial1.available() > 0) {
      gps.encode(Serial1.read());
    }

    // Tara relativa Roll/Pitch aos 20s
    if (sensorLigado && !taraFeita && millis() > 20000) {
      sensors_event_t orientacao;
      bno.getEvent(&orientacao);
      taraY = orientacao.orientation.y;
      taraZ = orientacao.orientation.z;
      taraFeita = true;
      Serial.println("\n[!] TARA RELATIVA CONCLUÍDA!\n");
    }

    if (millis() - ultimoPrint > 1000) {
      ultimoPrint = millis(); 
      estadoBarco.timestamp = millis() / 1000;
      
      if (gps.speed.isValid()) estadoBarco.sog = gps.speed.kmph();
      if (gps.course.isValid()) estadoBarco.cog = gps.course.deg();

      if (sensorLigado) {
        sensors_event_t orientacao;
        bno.getEvent(&orientacao);
        estadoBarco.heading = orientacao.orientation.x;
        estadoBarco.roll = taraFeita ? (orientacao.orientation.y - taraY) : orientacao.orientation.y;
        estadoBarco.pitch = taraFeita ? (orientacao.orientation.z - taraZ) : orientacao.orientation.z;
      }

      // 1. ESPELHO: Imprimir o que vai ser enviado (Visão do Atleta) no formato original
      Serial.println("\n=======================================");
      Serial.println("📤 [ATLETA] A ENVIAR PACOTE DE TELEMETRIA:");
      Serial.print("Tempo:        "); Serial.print(estadoBarco.timestamp); Serial.println(" s");
      Serial.print("SOG (Vel):    "); Serial.print(estadoBarco.sog); Serial.println(" km/h");
      Serial.print("COG (Rumo):   "); Serial.print(estadoBarco.cog); Serial.println(" º");
      Serial.print("Bússola:      "); Serial.print(estadoBarco.heading); Serial.println(" º");
      Serial.print("Inclinação -> Roll: "); Serial.print(estadoBarco.roll);
      Serial.print("º | Pitch: "); Serial.print(estadoBarco.pitch); Serial.println("º");
      
      // 2. Disparar pelo rádio
      bool sucesso = radio.write(&estadoBarco, sizeof(estadoBarco));
      if (!sucesso) {
         Serial.println("-> [ALERTA] Rádio falhou o envio do pacote.");
      } else {
         Serial.println("-> [OK] Pacote emitido para o ar!");
      }
      Serial.println("=======================================\n");
    }
  } 
  else {
    if (radio.available()) {
      radio.read(&estadoBarco, sizeof(estadoBarco));
      
      // Painel do Treinador no formato original
      Serial.println("\n=======================================");
      Serial.println("📡 [TREINADOR] PACOTE RECEBIDO:");
      Serial.print("Tempo Atleta: "); Serial.print(estadoBarco.timestamp); Serial.println(" s");
      Serial.print("SOG (Vel):    "); Serial.print(estadoBarco.sog); Serial.println(" km/h");
      Serial.print("COG (Rumo):   "); Serial.print(estadoBarco.cog); Serial.println(" º");
      Serial.print("Bússola:      "); Serial.print(estadoBarco.heading); Serial.println(" º");
      Serial.print("Inclinação -> Roll: "); Serial.print(estadoBarco.roll);
      Serial.print("º | Pitch: "); Serial.print(estadoBarco.pitch); Serial.println("º");
      Serial.println("=======================================\n");
    }
  }
}