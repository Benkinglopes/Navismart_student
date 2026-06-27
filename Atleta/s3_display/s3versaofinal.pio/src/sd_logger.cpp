/**
 * @file sd_logger.cpp
 * @brief Implementação do logger CSV para cartão SD via SPI (fallback SD_MMC) — ESP32-S3
 *
 * Fluxo:
 *   Core 1  →  sdLogger_enqueue()  →  [Ring Buffer em PSRAM]
 *   Core 0  →  sd_logger_task()    →  lê buffer → batch → ficheiro CSV no SD
 *
 * Escrita em batch: acumula SD_BATCH_SIZE registos OU aguarda SD_BATCH_TIMEOUT_MS ms.
 *
 * ─── ALTERAÇÕES v2 ────────────────────────────────────────────────────────────
 *  1. Ficheiro NOVO a cada arranque:
 *       dados_001.csv, dados_002.csv, … dados_999.csv
 *     Nunca acrescenta ao ficheiro de um arranque anterior.
 *     O próximo índice disponível é determinado uma única vez em sdLogger_init().
 *  2. Formato CSV confirmado:
 *       timestamp,lat,lon,sog,cog,roll,pitch,heading
 *       2026-05-07 14:00:01,38.670021,-9.409976,6.01,41.4,15.7,1.0,45.1
 *  3. Remoção de buildFilePath() + openDailyFile() (baseados na data).
 *     Substituídos por findNextBootFilePath() + createBootLogFile() + openLogFileForAppend().
 * ──────────────────────────────────────────────────────────────────────────────
 */

#include "sd_logger.h"

#include <SD.h>
#if SD_USE_SPI
#include <SPI.h>
#endif
#include <SD_MMC.h>
#include <FS.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

// ============================================================
//  Tag de log
// ============================================================
static const char* TAG = "SD_LOGGER";

// ============================================================
//  Anel circular em PSRAM
//  Protegido por mutex — produtor (Core 1) e consumidor (Core 0)
// ============================================================
struct RingBuffer {
    SensorRecord* data;   ///< Array alocado em PSRAM
    uint32_t      head;   ///< Índice de escrita (produtor)
    uint32_t      tail;   ///< Índice de leitura (consumidor)
    uint32_t      count;  ///< Nº de itens actuais
    uint32_t      capacity;
};

static RingBuffer        s_ring       = {};
static SemaphoreHandle_t s_ringMutex  = nullptr;

// ============================================================
//  Estado interno
// ============================================================
static volatile SDLoggerState s_state        = SDLoggerState::UNINITIALIZED;
static volatile uint32_t      s_droppedCount = 0;
static volatile uint32_t      s_writtenCount = 0;

static TaskHandle_t      s_taskHandle      = nullptr;
static volatile bool     s_shutdownReq     = false;
static SemaphoreHandle_t s_shutdownDoneSem = nullptr;

// Buffer de texto para batch (stack local na tarefa)
static char s_lineBuffer[SD_BATCH_SIZE][96];

// ============================================================
//  Caminho do ficheiro deste arranque
//  Determinado UMA VEZ em sdLogger_init() e nunca mais alterado.
//  Ex: "/dados_007.csv"
// ============================================================
static char s_bootFilePath[32] = {};

// ============================================================
//  Helpers internos
// ============================================================

/**
 * @brief Acende/apaga o LED de estado de erro (NeoPixel ou LED convencional).
 */
static void setErrorLED(bool on) {
#ifdef SD_ERROR_LED_EXTERNAL
    digitalWrite(SD_ERROR_LED_PIN, on ? HIGH : LOW);
#else
    // WS2812 integrado (GPIO 48)
    if (on) {
        neopixelWrite(SD_ERROR_LED_PIN, 32, 0, 0); // vermelho suave
    } else {
        neopixelWrite(SD_ERROR_LED_PIN, 0, 0, 0);
    }
#endif
}

/**
 * @brief Procura o próximo índice disponível no SD e constrói o caminho.
 *        Tenta /dados_001.csv, /dados_002.csv, … até /dados_999.csv.
 *        Retorna true e preenche outPath se encontrar um slot livre.
 */
static bool findNextBootFilePath(char* outPath, size_t maxLen) {
    for (int i = 1; i <= 999; i++) {
        snprintf(outPath, maxLen, "/dados_%03d.csv", i);
#if SD_USE_SPI
        if (!SD.exists(outPath)) return true;
#else
        if (!SD_MMC.exists(outPath)) return true;
#endif
    }
    ESP_LOGE(TAG, "Todos os 999 slots de ficheiro estão ocupados no SD!");
    return false;
}

/**
 * @brief Cria um NOVO ficheiro CSV e escreve o cabeçalho.
 *        Usa FILE_WRITE — garante que é sempre um ficheiro fresco,
 *        independentemente de existir algum com o mesmo nome.
 *        Chamado apenas em sdLogger_init().
 */
static bool createBootLogFile(const char* path) {
#if SD_USE_SPI
    File f = SD.open(path, FILE_WRITE);
#else
    File f = SD_MMC.open(path, FILE_WRITE);
#endif
    if (!f) {
        ESP_LOGE(TAG, "Falha ao criar ficheiro de log: %s", path);
        return false;
    }
    f.println("timestamp,lat,lon,sog,cog,roll,pitch,heading");
    f.flush();
    f.close();
    ESP_LOGI(TAG, "Ficheiro de log criado: %s", path);
    return true;
}

/**
 * @brief Abre o ficheiro de log em modo append para escrita de dados.
 *        O cabeçalho já foi escrito por createBootLogFile().
 *        Chamado pela tarefa SD para cada sessão ou após re-montagem.
 */
static File openLogFileForAppend(const char* path) {
#if SD_USE_SPI
    File f = SD.open(path, FILE_APPEND);
#else
    File f = SD_MMC.open(path, FILE_APPEND);
#endif
    if (!f) {
        ESP_LOGE(TAG, "Falha ao abrir ficheiro em append: %s", path);
    }
    return f;
}

/**
 * @brief Formata um SensorRecord numa linha CSV.
 *        Formato: timestamp,lat,lon,sog,cog,roll,pitch,heading\n
 *        Retorna o número de chars escritos (sem '\0').
 */
static int formatCSVLine(const SensorRecord& r, char* buf, size_t bufLen) {
    return snprintf(buf, bufLen,
        "%s,%.6f,%.6f,%.2f,%.1f,%.1f,%.1f,%.1f\n",
        r.timestamp,
        r.lat, r.lon,
        r.sog, r.cog,
        r.roll, r.pitch, r.heading);
}

static uint64_t getSDFreeMB() {
#if SD_USE_SPI
    return (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL);
#else
    return (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL * 1024ULL);
#endif
}

// ============================================================
//  Ring Buffer — operações (chamadas sempre com mutex detido)
// ============================================================

static bool rb_push(const SensorRecord& rec) {
    if (s_ring.count >= s_ring.capacity) return false;
    s_ring.data[s_ring.head] = rec;
    s_ring.head = (s_ring.head + 1) % s_ring.capacity;
    s_ring.count++;
    return true;
}

static bool rb_pop(SensorRecord& rec) {
    if (s_ring.count == 0) return false;
    rec = s_ring.data[s_ring.tail];
    s_ring.tail = (s_ring.tail + 1) % s_ring.capacity;
    s_ring.count--;
    return true;
}

static uint32_t rb_available() {
    return s_ring.count;
}

// ============================================================
//  Inicialização SD card
// ============================================================

static bool mountSD() {
#if SD_USE_SPI
    SPI.begin(SD_SPI_SCLK, SD_SPI_MISO, SD_SPI_MOSI, SD_SPI_CS);
    bool ok = SD.begin(SD_SPI_CS, SPI, SD_SPI_FREQUENCY, "/sdcard", 5, false);
    if (!ok) {
        ESP_LOGE(TAG, "SD.begin() falhou — cartão não detectado ou não suportado");
        return false;
    }

    sdcard_type_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        ESP_LOGE(TAG, "Nenhum cartão SD detectado");
        SD.end();
        return false;
    }

    const char* typeStr = (cardType == CARD_MMC)  ? "MMC"  :
                          (cardType == CARD_SD)   ? "SDSC" :
                          (cardType == CARD_SDHC) ? "SDHC" : "Desconhecido";

    uint64_t cardSize   = SD.cardSize()  / (1024ULL * 1024ULL);
    uint64_t usedBytes  = SD.usedBytes() / (1024ULL * 1024ULL);
    uint64_t totalBytes = SD.totalBytes()/ (1024ULL * 1024ULL);
#else
    #ifdef SD_4BIT_MODE
    if (!SD_MMC.setPins(SD_MMC_PIN_CLK, SD_MMC_PIN_CMD, SD_MMC_PIN_D0,
                        SD_MMC_PIN_D1,  SD_MMC_PIN_D2,  SD_MMC_PIN_D3)) {
        ESP_LOGE(TAG, "Falha ao configurar pinos SD_MMC (4-bit)");
        return false;
    }
    bool ok = SD_MMC.begin("/sdcard", false);
    #else
    if (!SD_MMC.setPins(SD_MMC_PIN_CLK, SD_MMC_PIN_CMD, SD_MMC_PIN_D0)) {
        ESP_LOGE(TAG, "Falha ao configurar pinos SD_MMC (1-bit)");
        return false;
    }
    bool ok = SD_MMC.begin("/sdcard", true);
    #endif

    if (!ok) {
        ESP_LOGE(TAG, "SD_MMC.begin() falhou — cartão não detectado ou não suportado");
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        ESP_LOGE(TAG, "Nenhum cartão SD detectado");
        SD_MMC.end();
        return false;
    }

    const char* typeStr = (cardType == CARD_MMC)  ? "MMC"  :
                          (cardType == CARD_SD)   ? "SDSC" :
                          (cardType == CARD_SDHC) ? "SDHC" : "Desconhecido";

    uint64_t cardSize   = SD_MMC.cardSize()  / (1024ULL * 1024ULL);
    uint64_t usedBytes  = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
    uint64_t totalBytes = SD_MMC.totalBytes()/ (1024ULL * 1024ULL);
#endif

    ESP_LOGI(TAG, "SD montado: tipo=%s  tamanho=%llu MB  usado=%llu/%llu MB",
             typeStr, cardSize, usedBytes, totalBytes);

    uint64_t freeBytes = totalBytes - usedBytes;
    if (freeBytes < 10) {
        ESP_LOGW(TAG, "Espaço livre no SD muito reduzido: %llu MB", freeBytes);
    }

    return true;
}

// ============================================================
//  Tarefa FreeRTOS — Core 0
// ============================================================

static void sd_logger_task(void* pvParameters) {
    ESP_LOGI(TAG, "Tarefa SD Logger iniciada no Core %d", xPortGetCoreID());

    File     currentFile;
    uint32_t lastWriteTime      = millis();
    bool     sdMounted          = false;
    uint32_t lastRemountAttempt = 0;

    // Monta o SD na entrada da tarefa (pode já estar montado por sdLogger_init;
    // SD.begin() é idempotente no arduino-esp32)
    sdMounted = mountSD();
    if (sdMounted) {
        s_state = SDLoggerState::OK;
        setErrorLED(false);
    } else {
        s_state = SDLoggerState::SD_ERROR;
        setErrorLED(true);
    }

    for (;;) {
        uint32_t now = millis();

        // ──────────────────────────────────────────────────────────────────
        //  Ponto de saída cooperativo — verifica pedido de shutdown
        // ──────────────────────────────────────────────────────────────────
        if (s_shutdownReq) {
            ESP_LOGI(TAG, "Shutdown cooperativo solicitado. A fechar SD...");
            if (currentFile) {
                currentFile.flush();
                currentFile.close();
            }
            if (sdMounted) {
#if SD_USE_SPI
                SD.end();
#else
                SD_MMC.end();
#endif
            }
            setErrorLED(false);
            s_taskHandle = nullptr;
            if (s_shutdownDoneSem) xSemaphoreGive(s_shutdownDoneSem);
            ESP_LOGI(TAG, "Tarefa SD Logger terminada de forma limpa.");
            vTaskDelete(NULL);
            return;
        }

        // ──────────────────────────────────────────────────────────────────
        //  Se SD em erro, tenta re-montar periodicamente
        // ──────────────────────────────────────────────────────────────────
        if (!sdMounted) {
            if (now - lastRemountAttempt >= SD_REMOUNT_INTERVAL_MS) {
                lastRemountAttempt = now;
                ESP_LOGW(TAG, "A tentar re-montar o cartão SD...");

                if (currentFile) { currentFile.close(); }
#if SD_USE_SPI
                SD.end();
#else
                SD_MMC.end();
#endif
                vTaskDelay(pdMS_TO_TICKS(500));

                sdMounted = mountSD();
                if (sdMounted) {
                    s_state = SDLoggerState::OK;
                    setErrorLED(false);
                    // currentFile está fechado — será reaberto na próxima iteração
                    ESP_LOGI(TAG, "SD re-montado com sucesso.");
                } else {
                    ESP_LOGE(TAG, "Re-montagem falhou. Próxima tentativa em %d ms",
                             SD_REMOUNT_INTERVAL_MS);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // ──────────────────────────────────────────────────────────────────
        //  Extrai batch do ring buffer (único lock, sem I/O dentro)
        // ──────────────────────────────────────────────────────────────────
        bool timeoutReached = (now - lastWriteTime) >= SD_BATCH_TIMEOUT_MS;

        SensorRecord batch[SD_BATCH_SIZE];
        uint32_t batchCount = 0;

        if (xSemaphoreTake(s_ringMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            uint32_t available  = rb_available();
            bool     batchReady = (available >= SD_BATCH_SIZE);

            if (available > 0 && (batchReady || timeoutReached)) {
                while (batchCount < SD_BATCH_SIZE && rb_available() > 0) {
                    rb_pop(batch[batchCount++]);
                }
            }
            xSemaphoreGive(s_ringMutex);
        }

        if (batchCount == 0) {
            if (timeoutReached) lastWriteTime = now;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ──────────────────────────────────────────────────────────────────
        //  Abre o ficheiro de arranque UMA VEZ (ou após re-montagem)
        //  ← ALTERAÇÃO PRINCIPAL: substituído o bloco de path por data
        // ──────────────────────────────────────────────────────────────────
        if (!currentFile) {
            currentFile = openLogFileForAppend(s_bootFilePath);
            if (!currentFile) {
                ESP_LOGE(TAG, "Não foi possível abrir '%s' — erro no SD", s_bootFilePath);
                s_state   = SDLoggerState::SD_ERROR;
                sdMounted = false;
                setErrorLED(true);
#if SD_USE_SPI
                SD.end();
#else
                SD_MMC.end();
#endif
                continue;
            }
            ESP_LOGI(TAG, "Ficheiro aberto para escrita: %s", s_bootFilePath);
        }

        // ──────────────────────────────────────────────────────────────────
        //  Formata e escreve o batch num único write
        // ──────────────────────────────────────────────────────────────────
        bool writeOk = true;

        for (uint32_t i = 0; i < batchCount && writeOk; i++) {
            int len = formatCSVLine(batch[i], s_lineBuffer[i], sizeof(s_lineBuffer[i]));
            if (len <= 0) {
                ESP_LOGW(TAG, "Falha ao formatar linha %u", i);
                continue;
            }

            size_t written = currentFile.write(
                reinterpret_cast<const uint8_t*>(s_lineBuffer[i]),
                static_cast<size_t>(len));

            if (written != static_cast<size_t>(len)) {
                ESP_LOGE(TAG, "Escrita incompleta: %u/%d bytes", written, len);
                writeOk = false;
            }
        }

        if (writeOk) {
            currentFile.flush();
            s_writtenCount += batchCount;
            lastWriteTime   = millis();
            ESP_LOGD(TAG, "Batch de %u registos escrito. Total: %u", batchCount, s_writtenCount);
        } else {
            ESP_LOGE(TAG, "Erro de escrita no SD. Verificando espaço...");
            uint64_t freeBytes = getSDFreeMB();
            if (freeBytes < 1) {
                s_state = SDLoggerState::SD_FULL;
                ESP_LOGE(TAG, "Cartão SD cheio! Escrita suspensa.");
            } else {
                s_state = SDLoggerState::SD_ERROR;
            }
            setErrorLED(true);
            if (currentFile) { currentFile.close(); }
            sdMounted = false;
#if SD_USE_SPI
            SD.end();
#else
            SD_MMC.end();
#endif
        }
    } // for(;;)
}

// ============================================================
//  API Pública — Implementação
// ============================================================

bool sdLogger_init() {
#ifdef SD_ERROR_LED_EXTERNAL
    pinMode(SD_ERROR_LED_PIN, OUTPUT);
#endif
    setErrorLED(false);

    s_shutdownDoneSem = xSemaphoreCreateBinary();
    if (!s_shutdownDoneSem) {
        ESP_LOGE(TAG, "Falha ao criar semáforo de shutdown");
        return false;
    }
    s_shutdownReq = false;

    if (!psramFound()) {
        ESP_LOGE(TAG, "PSRAM não detectada! Verifica board_build.arduino.memory_type=qio_opi");
        return false;
    }

    size_t allocSize = SD_QUEUE_LENGTH * sizeof(SensorRecord);
    s_ring.data = static_cast<SensorRecord*>(
        heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (!s_ring.data) {
        ESP_LOGE(TAG, "Falha ao alocar %u bytes em PSRAM para ring buffer", allocSize);
        return false;
    }

    s_ring.head     = 0;
    s_ring.tail     = 0;
    s_ring.count    = 0;
    s_ring.capacity = SD_QUEUE_LENGTH;

    ESP_LOGI(TAG, "Ring buffer alocado em PSRAM: %u registos × %u bytes = %u KB",
             SD_QUEUE_LENGTH, sizeof(SensorRecord), allocSize / 1024);

    s_ringMutex = xSemaphoreCreateMutex();
    if (!s_ringMutex) {
        ESP_LOGE(TAG, "Falha ao criar mutex");
        heap_caps_free(s_ring.data);
        s_ring.data = nullptr;
        return false;
    }

    s_state        = SDLoggerState::UNINITIALIZED;
    s_droppedCount = 0;
    s_writtenCount = 0;

    // Monta o SD
    ESP_LOGI(TAG, "SD Logger a inicializar. A montar cartão SD...");
    if (!mountSD()) {
        ESP_LOGE(TAG, "Falha ao montar o SD durante a inicialização.");
        s_state = SDLoggerState::SD_ERROR;
#if SD_USE_SPI
        SD.end();
#else
        SD_MMC.end();
#endif
        heap_caps_free(s_ring.data);
        s_ring.data = nullptr;
        vSemaphoreDelete(s_ringMutex);
        s_ringMutex = nullptr;
        return false;
    }

    s_state = SDLoggerState::OK;

    // ── ALTERAÇÃO: determina o caminho sequencial e cria o ficheiro ──────────
    if (!findNextBootFilePath(s_bootFilePath, sizeof(s_bootFilePath))) {
        ESP_LOGE(TAG, "Sem slots livres no SD para ficheiro de log.");
        s_state = SDLoggerState::SD_ERROR;
#if SD_USE_SPI
        SD.end();
#else
        SD_MMC.end();
#endif
        heap_caps_free(s_ring.data);
        s_ring.data = nullptr;
        vSemaphoreDelete(s_ringMutex);
        s_ringMutex = nullptr;
        return false;
    }

    if (!createBootLogFile(s_bootFilePath)) {
        ESP_LOGE(TAG, "Falha ao criar o ficheiro de log: %s", s_bootFilePath);
        s_state = SDLoggerState::SD_ERROR;
#if SD_USE_SPI
        SD.end();
#else
        SD_MMC.end();
#endif
        heap_caps_free(s_ring.data);
        s_ring.data = nullptr;
        vSemaphoreDelete(s_ringMutex);
        s_ringMutex = nullptr;
        return false;
    }
    // ──────────────────────────────────────────────────────────────────────────

    ESP_LOGI(TAG, "SD Logger pronto. Ficheiro deste arranque: %s", s_bootFilePath);
    return true;
}

void sdLogger_startTask() {
    if (s_taskHandle != nullptr) {
        ESP_LOGW(TAG, "Tarefa já iniciada.");
        return;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        sd_logger_task,
        "SD_Logger",
        SD_TASK_STACK_SIZE,
        nullptr,
        5,
        &s_taskHandle,
        0   // Core 0
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa SD Logger (código %d)", result);
        s_taskHandle = nullptr;
    } else {
        ESP_LOGI(TAG, "Tarefa SD Logger criada no Core 0, prioridade 5, stack %d bytes",
                 SD_TASK_STACK_SIZE);
    }
}

bool sdLogger_enqueue(const SensorRecord& record) {
    if (!s_ring.data || !s_ringMutex) return false;

    bool accepted = false;

    if (xSemaphoreTake(s_ringMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        accepted = rb_push(record);
        xSemaphoreGive(s_ringMutex);
    }

    if (!accepted) {
        s_droppedCount++;
        ESP_LOGW(TAG, "Queue cheia! Registo descartado. Total descartados: %u", s_droppedCount);
    }

    return accepted;
}

SDLoggerState sdLogger_getState() {
    return s_state;
}

uint32_t sdLogger_getDroppedCount() {
    return s_droppedCount;
}

uint32_t sdLogger_getWrittenCount() {
    return s_writtenCount;
}

void sdLogger_safeUnmount() {
    ESP_LOGI(TAG, "A solicitar shutdown cooperativo do SD Logger...");

    if (!s_taskHandle) {
#if SD_USE_SPI
        SD.end();
#else
        SD_MMC.end();
#endif
        ESP_LOGI(TAG, "SD desmontado (tarefa inactiva).");
        return;
    }

    s_shutdownReq = true;

    const TickType_t timeout = pdMS_TO_TICKS(SD_SHUTDOWN_TIMEOUT_MS);
    if (xSemaphoreTake(s_shutdownDoneSem, timeout) == pdTRUE) {
        ESP_LOGI(TAG, "SD Logger encerrado de forma limpa. Seguro desligar.");
    } else {
        ESP_LOGW(TAG,
            "Timeout no shutdown cooperativo (%d ms). "
            "A forçar desmontagem SD sem garantia de flush completo.",
            SD_SHUTDOWN_TIMEOUT_MS);
#if SD_USE_SPI
        SD.end();
#else
        SD_MMC.end();
#endif
    }
}