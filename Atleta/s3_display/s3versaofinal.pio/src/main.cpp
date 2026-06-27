#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lvgl.h>
#include "UI/ui.h"

#define ENABLE_SD_LOGGER 1
#if ENABLE_SD_LOGGER
#include "sd_logger.h"
#endif


// =============================================================
//  ADIÇÕES PARA RECEBER DADOS DO C3
// =============================================================
#include "SerialTransfer.h"
#include "struct.h"

SerialTransfer myTransfer;
TelemetryPayload dadosRecebidos;

// =============================================================
//  CLASSE LGFX — Makerfabs MaTouch 4.3'' ESP32-S3 RGB
// =============================================================
class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_RGB     _bus_instance;
    lgfx::Panel_RGB   _panel_instance;
    lgfx::Touch_GT911 _touch_instance;
public:
    LGFX(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;
            cfg.pin_d0  = GPIO_NUM_8;  cfg.pin_d1  = GPIO_NUM_3;  cfg.pin_d2  = GPIO_NUM_46;
            cfg.pin_d3  = GPIO_NUM_9;  cfg.pin_d4  = GPIO_NUM_1;  cfg.pin_d5  = GPIO_NUM_5;
            cfg.pin_d6  = GPIO_NUM_6;  cfg.pin_d7  = GPIO_NUM_7;  cfg.pin_d8  = GPIO_NUM_15;
            cfg.pin_d9  = GPIO_NUM_16; cfg.pin_d10 = GPIO_NUM_4;  cfg.pin_d11 = GPIO_NUM_45;
            cfg.pin_d12 = GPIO_NUM_48; cfg.pin_d13 = GPIO_NUM_47; cfg.pin_d14 = GPIO_NUM_21;
            cfg.pin_d15 = GPIO_NUM_14;
            cfg.pin_henable = GPIO_NUM_40; cfg.pin_vsync = GPIO_NUM_41;
            cfg.pin_hsync   = GPIO_NUM_39; cfg.pin_pclk  = GPIO_NUM_42;
            cfg.freq_write = 14000000;
            cfg.hsync_pulse_width = 4; cfg.hsync_back_porch = 8; cfg.hsync_front_porch = 8;
            cfg.vsync_pulse_width = 4; cfg.vsync_back_porch = 8; cfg.vsync_front_porch = 8;
            _bus_instance.config(cfg);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width  = 800; cfg.memory_height = 480;
            cfg.panel_width   = 800; cfg.panel_height  = 480;
            cfg.rgb_order = false;
            cfg.invert    = false;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _touch_instance.config();
            cfg.pin_sda = 17; cfg.pin_scl = 18;
            cfg.pin_rst = 38; cfg.pin_int = -1;
            cfg.i2c_port = 1; cfg.freq = 400000;
            cfg.offset_rotation = 0;
            cfg.x_min = 0;
            cfg.x_max = 799;
            cfg.y_min = 0;
            cfg.y_max = 479;
            _touch_instance.config(cfg);
        }
        _panel_instance.setBus(&_bus_instance);
        _panel_instance.setTouch(&_touch_instance);
        setPanel(&_panel_instance);
    }
};

LGFX tft;

// =============================================================
//  TIMESTAMP
//  Devolve "YYYY-MM-DD HH:MM:SS" se o relógio estiver sincronizado,
//  ou "1970-01-01 00:00:00" caso contrário (sem NTP/GPS ainda).
// =============================================================
static void getCurrentTimestamp(char* buf, size_t len) {
    time_t now = time(nullptr);
    if (now < 24L * 3600L) {
        snprintf(buf, len, "1970-01-01 00:00:00");
        return;
    }
    struct tm timeinfo = *localtime(&now);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);
}

// =============================================================
//  SINCRONIZAÇÃO DA HORA VIA GPS (C3)
//
//  O campo main.time é enviado pelo C3. Para que isto funcione,
//  o C3 precisa de enviar um Unix timestamp (segundos desde
//  1970-01-01 00:00:00 UTC). Um valor válido para 2026 é
//  aproximadamente 1 750 000 000.
//
//  Se o teu C3 já envia o tempo GPS noutro formato (ex: milissegundos,
//  ou GPS Week + TOW), converte para Unix timestamp antes de chamar
//  settimeofday().
//
//  Só sincroniza uma vez por arranque (flag timeSyncedFromGPS).
// =============================================================
static bool timeSyncedFromGPS = false;

static void tryGPSTimeSync(uint32_t gpsUnixTime) {
    if (timeSyncedFromGPS) return;

    // Valor mínimo plausível: 1 Jan 2020 = 1577836800
    // Valor máximo para uint32_t: ano 2106 (~4 294 967 295)
    if (gpsUnixTime < 1577836800UL) return; // ainda não temos fix GPS válido

    struct timeval tv;
    tv.tv_sec  = (time_t)gpsUnixTime;
    tv.tv_usec = 0;

    if (settimeofday(&tv, nullptr) == 0) {
        timeSyncedFromGPS = true;
        char buf[20];
        getCurrentTimestamp(buf, sizeof(buf));
        Serial.printf("⏱ Hora sincronizada via GPS: %s\n", buf);
    } else {
        Serial.println("AVISO: settimeofday() falhou");
    }
}

#if ENABLE_SD_LOGGER
static void telemetryToSensorRecord(const TelemetryPayload& src, SensorRecord& dst) {
    getCurrentTimestamp(dst.timestamp, sizeof(dst.timestamp));
    dst.lat     = src.comp.latitude;
    dst.lon     = src.comp.longitude;
    dst.sog     = src.main.sog;
    dst.cog     = src.comp.cog;
    dst.roll    = src.main.heel;
    dst.pitch   = src.main.trim;
    dst.heading = src.main.heading;
}
#endif

// =============================================================
//  FLUSH COM ROTAÇÃO 90°
// =============================================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    int32_t lv_w = area->x2 - area->x1 + 1;
    int32_t lv_h = area->y2 - area->y1 + 1;

    tft.startWrite();

    for (int32_t row = 0; row < lv_h; row++) {
        // phys_x inverte o eixo y do LVGL
        int32_t phys_x = 799 - (area->y1 + row);
        // phys_y mapeia diretamente o início do eixo x do LVGL
        int32_t phys_y = area->x1; 
        
        tft.setAddrWindow(phys_x, phys_y, 1, lv_w);
        
        // Agora escrevemos na mesma direção física (da esquerda para a direita no eixo Y físico)
        for (int32_t col = 0; col < lv_w; col++) {
            uint16_t raw = color_p[row * lv_w + col].full;
            uint16_t pixel = (raw >> 8) | (raw << 8);  // desfaz LV_COLOR_16_SWAP
            tft.writeColor(pixel, 1);
        }
    }

    tft.endWrite();
    lv_disp_flush_ready(disp);
}

// =============================================================
//  TOUCH — converte coordenadas físicas landscape → portrait LVGL
//  Rotação 90° inversa:
//    lv_x = (480 - 1) - phys_y  =  479 - phys_y
//    lv_y = phys_x
// =============================================================
// =============================================================
//  TOUCH — converte coordenadas físicas para portrait invertido
// =============================================================
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
        data->state   = LV_INDEV_STATE_PR;
        
        // Inverte a lógica em relação ao modo retrato anterior
        data->point.x = ty;
        data->point.y = 799 - tx;
        
        // Podes manter o Serial.printf para debug se quiseres validar os toques
        Serial.printf("Touch físico(%d,%d) → LVGL(%d,%d)\n", tx, ty, data->point.x, data->point.y);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// =============================================================
//  TIMER FUNCTION DECLARATIONS (forward declarations)
// =============================================================
void startTimer();
void stopTimer();
void updateTimer();
void updateClock();

// =============================================================
//  SETUP
// =============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    // =========================================================
    // CONFIGURAR FUSO HORÁRIO DE PORTUGAL (WET/WEST)
    // =========================================================
    setenv("TZ", "WET0WEST,M3.5.0/1,M10.5.0/2", 1);
    tzset();
    // =========================================================

    Serial.printf("PSRAM Total: %d bytes\n", ESP.getPsramSize());
    Serial.printf("PSRAM Livre: %d bytes\n", ESP.getFreePsram());

    pinMode(38, OUTPUT);
    digitalWrite(38, LOW);  delay(50);
    digitalWrite(38, HIGH); delay(50);

    pinMode(2, OUTPUT);
    digitalWrite(2, HIGH);

    tft.init();
    tft.setRotation(0);

    lv_init();

    static lv_color_t *buf = (lv_color_t *)heap_caps_malloc(
        480 * 80 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM
    );
    if (!buf) {
        Serial.println("ERRO: sem PSRAM para buffer LVGL!");
        while (1) delay(1000);
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 480 * 80);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = 480;
    disp_drv.ver_res  = 800;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    ui_init();

#if ENABLE_SD_LOGGER
    bool sdReady = false;
    if (!sdLogger_init()) {
        Serial.println("ERRO: falha ao inicializar o SD Logger");
    } else {
        sdLogger_startTask();
        Serial.println("SD Logger iniciado no Core 0");
        sdReady = true;
    }
#endif

    lv_obj_add_event_cb(ui_Panel7, [](lv_event_t *e) {
        lv_disp_load_scr(ui_ScreenRace);
        startTimer();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_set_style_text_color(ui_SdReady, lv_color_hex(0xFFFFFF), 0);
#if ENABLE_SD_LOGGER
    if (sdReady) {
        lv_label_set_text(ui_SdReady, "SD Card Ready");
    } else {
        lv_label_set_text(ui_SdReady, "SD Card ERROR");
    }
#else
    lv_label_set_text(ui_SdReady, "SD Logging disabled");
#endif

    // =============================================================
    // INICIAR A ESCUTA DOS DADOS DO C3 (Pino 19)
    // =============================================================
    Serial1.begin(460800, SERIAL_8N1, 19, -1);
    myTransfer.begin(Serial1);
    Serial.println("=== PRONTO A RECEBER TELEMETRIA DO C3 ===");
}

// =============================================================
//  CLOCK UPDATE FUNCTION
// =============================================================
void updateClock() {
    time_t now = time(nullptr);
    struct tm timeinfo = *localtime(&now);
    lv_label_set_text_fmt(ui_ValTimer, "%02d:%02d:%02d",
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

// =============================================================
//  TIMER VARIABLES AND FUNCTIONS
// =============================================================
unsigned long timerStartTime = 0;
bool timerRunning = false;

void startTimer() {
    timerStartTime = millis();
    timerRunning = true;
}

void stopTimer() {
    timerRunning = false;
}

void updateTimer() {
    if (!timerRunning) {
        updateClock();
        return;
    }

    unsigned long elapsed = millis() - timerStartTime;

    uint32_t seconds = elapsed / 1000;
    uint32_t minutes = seconds / 60;
    uint32_t hours   = minutes / 60;

    seconds = seconds % 60;
    minutes = minutes % 60;

    lv_label_set_text_fmt(ui_ValTimer, "%02lu:%02lu:%02lu", hours, minutes, seconds);
}

// =============================================================
//  LOOP
// =============================================================
void loop() {
    updateTimer();

    // =========================================================
    // ESCUTAR DADOS DO C3 CONTINUAMENTE
    // =========================================================
    if (myTransfer.available()) {
        myTransfer.rxObj(dadosRecebidos);

        Serial.print("Velocidade (SOG) recebida do C3: ");
        Serial.print(dadosRecebidos.main.sog);
        Serial.println(" km/h");

        // ─────────────────────────────────────────────────────
        //  Sincroniza o relógio do ESP32-S3 via GPS (C3)
        //
        //  ATENÇÃO: main.time tem de ser Unix timestamp (segundos
        //  desde 1970). Se o C3 enviar noutro formato, converte
        //  antes de chamar esta função.
        //  Exemplo de como o C3 deve enviar:
        //    main.time = (uint32_t)gps.time.value(); // se for GPS library unix ts
        // ─────────────────────────────────────────────────────
        tryGPSTimeSync(dadosRecebidos.main.time);

        // Converte valores para texto
        String textoKTS  = String(dadosRecebidos.main.sog, 1);
        String textoHEEL = String(dadosRecebidos.main.heel, 1);
        String textoTRIM = String(dadosRecebidos.main.trim, 1);
        String textoHDG  = String(dadosRecebidos.main.heading, 1);

        // Atualiza labels no SquareLine
        lv_label_set_text(ui_ValKTS,  textoKTS.c_str());
        lv_label_set_text(ui_ValHEEL, textoHEEL.c_str());
        lv_label_set_text(ui_ValTRIM, textoTRIM.c_str());
        lv_label_set_text(ui_ValHDG,  textoHDG.c_str());

#if ENABLE_SD_LOGGER
        // SÓ ENVIA PARA O CARTÃO SD SE O GPS JÁ TIVER SINCRONIZADO A HORA CORRETA
        if (timeSyncedFromGPS) {
            SensorRecord logRecord;
            telemetryToSensorRecord(dadosRecebidos, logRecord);
            if (!sdLogger_enqueue(logRecord)) {
                Serial.println("AVISO: sdLogger_enqueue falhou — fila cheia ou não inicializada.");
            }
        }
#endif
    }

    lv_timer_handler();
    delay(5);
}