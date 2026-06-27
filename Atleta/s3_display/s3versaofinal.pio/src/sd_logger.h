/**
 * @file sd_logger.h
 * @brief Logger CSV para cartão SD via SPI (fallback SD_MMC) no ESP32-S3
 *
 * Arquitectura:
 *  - Core 1 (Arduino loop): aquisição de sensores → sdLogger_enqueue()
 *  - Core 0 (FreeRTOS task): lê a queue → escreve no SD em blocos
 *
 * Buffer em PSRAM → sem perdas mesmo com SD lento.
 * Escrita em batch (BATCH_SIZE amostras ou BATCH_TIMEOUT_MS ms).
 *
 * Board: Makerfabs E32S3RGB43 (ESP32-S3-WROOM-1-N16R8, 16MB Flash, 8MB PSRAM OPI)
 *
 * ⚠️  VERIFICA OS PINOS com o schematic da tua placa antes de compilar!
 */

#pragma once

#include <Arduino.h>
#include <cstdint>

// ============================================================
//  PINOS SD via SPI — usado apenas para um módulo SD externo.
//  Se o cartão SD está no slot integrado da placa, use SD_MMC.
// ============================================================
#define SD_USE_SPI 1
#if SD_USE_SPI
#define SD_SPI_SCLK      12   ///< SPI clock (SCLK)
#define SD_SPI_MISO      13   ///< SPI MISO
#define SD_SPI_MOSI      11   ///< SPI MOSI
#define SD_SPI_CS        10   ///< SPI chip select (CS)
#define SD_SPI_FREQUENCY 4000000u
#else
// ============================================================
//  PINOS SD_MMC — Makerfabs E32S3RGB43
//  Confirma no schematic: https://github.com/Makerfabs/Makerfabs-ESP32-S3-SPI-TFT-with-Touch
//  O modelo RGB usa um conector microSD dedicado ligado ao SD_MMC.
// ============================================================
#define SD_MMC_PIN_CLK   39   ///< SD_MMC CLK
#define SD_MMC_PIN_CMD   38   ///< SD_MMC CMD (MOSI analógico)
#define SD_MMC_PIN_D0    40   ///< SD_MMC DATA0 (MISO analógico)
// --- 4-bit mode (descomenta SD_4BIT abaixo se quiseres 4-bit) ---
#define SD_MMC_PIN_D1    41   ///< SD_MMC DATA1
#define SD_MMC_PIN_D2    42   ///< SD_MMC DATA2
#define SD_MMC_PIN_D3    2    ///< SD_MMC DATA3 (também usado como CS em SPI)

// Seleciona o modo: comenta para usar 1-bit (mais seguro/compatível)
// #define SD_4BIT_MODE
#endif

// ============================================================
//  LED de estado de erro
//
//  ⚠️  GPIO 48 no ESP32-S3-WROOM-1 está ligado a um LED WS2812 (NeoPixel),
//  NÃO a um LED convencional. digitalWrite() não tem efeito útil neste pino.
//  O código usa neopixelWrite() (disponível no Arduino ESP32 core >= 2.0.5).
//
//  Alternativa: define SD_ERROR_LED_EXTERNAL e liga um LED convencional
//  (com resistência) a qualquer GPIO livre (ex: 21), evitando depender
//  da biblioteca NeoPixel e libertando GPIO 48 para outros fins.
// ============================================================
#define SD_ERROR_LED_PIN  48   ///< GPIO do LED de erro. GPIO 48 = NeoPixel integrado no WROOM-1.

// Descomenta para usar LED convencional em vez do NeoPixel integrado:
// #define SD_ERROR_LED_EXTERNAL

// ============================================================
//  Parâmetros da queue / buffer
// ============================================================
#define SD_QUEUE_LENGTH      512   ///< Nº máximo de registos na fila (em PSRAM, ~40KB)
#define SD_BATCH_SIZE        10    ///< Escreve no SD a cada N registos...
#define SD_BATCH_TIMEOUT_MS  5000  ///< ...ou a cada N ms (o que ocorrer primeiro)
#define SD_REMOUNT_INTERVAL_MS 10000 ///< Intervalo entre tentativas de re-montagem após falha
#define SD_TASK_STACK_SIZE   16384 ///< Stack em bytes para a tarefa SD Logger no Core 0

// ============================================================
//  Estrutura de dados do sensor
// ============================================================
/**
 * @brief Um registo de aquisição.
 *
 * timestamp → string "YYYY-MM-DD HH:MM:SS" fornecida pelo RTC.
 * Todos os floats usam ponto como separador decimal.
 */
struct SensorRecord {
    char  timestamp[20]; ///< "2026-05-07 14:00:00\0"
    float lat;           ///< Latitude  (ex: 38.670021)
    float lon;           ///< Longitude (ex: -9.409976)
    float sog;           ///< Speed Over Ground (nós ou m/s)
    float cog;           ///< Course Over Ground (graus)
    float roll;          ///< Roll  (graus)
    float pitch;         ///< Pitch (graus)
    float heading;       ///< Heading magnético (graus)
};

// ============================================================
//  Estado público do logger
// ============================================================
enum class SDLoggerState : uint8_t {
    UNINITIALIZED = 0,
    OK,
    SD_ERROR,       ///< Falha de montagem / leitura/escrita
    SD_FULL,        ///< Cartão cheio
};

// ============================================================
//  API Pública
// ============================================================

/**
 * @brief Inicializa o SD (SPI ou SD_MMC, conforme a configuração) e cria a fila em PSRAM.
 *        Deve ser chamada antes de sdLogger_startTask().
 *
 * @return true  Inicialização bem-sucedida.
 * @return false Falha (SD não presente, PSRAM indisponível, etc.).
 */
bool sdLogger_init();

/**
 * @brief Inicia a tarefa FreeRTOS de logging, ancorada ao Core 0.
 *        Deve ser chamada após sdLogger_init().
 */
void sdLogger_startTask();

/**
 * @brief Enfileira um registo para escrita no SD.
 *        Chamada a partir do Core 1 (aquisição).
 *        Thread-safe. Não bloqueia — se a fila estiver cheia, descarta.
 *
 * @param record  Registo a guardar.
 * @return true   Registo aceite.
 * @return false  Fila cheia — registo descartado (conta em sdLogger_getDroppedCount()).
 */
bool sdLogger_enqueue(const SensorRecord& record);

/**
 * @brief Retorna o estado actual do logger.
 */
SDLoggerState sdLogger_getState();

/**
 * @brief Retorna quantos registos foram descartados por fila cheia.
 */
uint32_t sdLogger_getDroppedCount();

/**
 * @brief Retorna quantos registos foram escritos com sucesso.
 */
uint32_t sdLogger_getWrittenCount();

/**
 * @brief Encerramento cooperativo (graceful shutdown) do logger.
 *
 *  1. Sinaliza a tarefa Core 0 via flag atómica.
 *  2. A tarefa conclui o batch corrente, faz flush, fecha o ficheiro
 *     e chama SD_MMC.end() antes de se auto-destruir (vTaskDelete).
 *  3. Esta função bloqueia o chamador até a tarefa confirmar o fim
 *     (semáforo binário) ou até SD_SHUTDOWN_TIMEOUT_MS expirar —
 *     nunca usa vTaskSuspend(), eliminando o risco de deadlock e
 *     corrupção FAT.
 *
 *  @note Chamar antes de desligar a alimentação ou reiniciar o sistema.
 */
void sdLogger_safeUnmount();

// Timeout máximo de espera pelo shutdown cooperativo (ms)
#define SD_SHUTDOWN_TIMEOUT_MS  8000
