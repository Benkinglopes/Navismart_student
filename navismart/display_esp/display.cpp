#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <cstring>
#include <esp_heap_caps.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lvgl.h>
#include "ui/ui.h"

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

static bool sdLogger_init() {
    return false;
}

static void sdLogger_startTask() {
}

// =============================================================
//  FLUSH COM ROTAÇÃO 90°
//
//  LVGL portrait:   x = 0..479,  y = 0..799
//  Painel físico:   x = 0..799,  y = 0..479
//
//  Rotação 90° horária:
//    phys_x = lv_y
//    phys_y = (480 - 1) - lv_x   →   479 - lv_x
//
//  O LVGL com LV_COLOR_16_SWAP=1 entrega pixels com bytes trocados.
//  pushImage com lgfx::swap565_t desfaz isso automaticamente.
//
//  Enviamos linha a linha do buffer LVGL, cada "linha LVGL" 
//  corresponde a uma coluna no painel físico.
// =============================================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    int32_t lv_w = area->x2 - area->x1 + 1;
    int32_t lv_h = area->y2 - area->y1 + 1;

    tft.startWrite();

    // Cada linha LVGL (y fixo, x varia) → coluna no painel físico
    // phys_x = area->y1 + row  (incrementa com as linhas LVGL)
    // phys_y começa em 479 - area->x1 e decrementa com as colunas LVGL
    // → empurramos cada linha LVGL como uma coluna vertical no painel

    for (int32_t row = 0; row < lv_h; row++) {
        int32_t phys_x = area->y1 + row;          // coluna física
        int32_t phys_y = 479 - area->x2;          // linha física (topo da coluna)
        // cada linha LVGL tem lv_w pixels; no painel é uma coluna de lv_w pixels
        tft.setAddrWindow(phys_x, phys_y, 1, lv_w);
        // os pixels desta linha estão em color_p[row * lv_w .. row * lv_w + lv_w - 1]
        // mas precisam de ir em ordem inversa (479-x2 até 479-x1)
        // → iteramos de trás para a frente
        for (int32_t col = lv_w - 1; col >= 0; col--) {
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
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = 479 - ty;
        data->point.y = tx;
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
void syncNTP();
void updateClock();

// =============================================================
//  SETUP
// =============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.printf("PSRAM Total: %d bytes\n", ESP.getPsramSize());
    Serial.printf("PSRAM Livre: %d bytes\n", ESP.getFreePsram());

    pinMode(38, OUTPUT);
    digitalWrite(38, LOW);  delay(50);
    digitalWrite(38, HIGH); delay(50);

    pinMode(2, OUTPUT);
    digitalWrite(2, HIGH);

    if (sdLogger_init()) {
        sdLogger_startTask();
        Serial.println("SD Logger iniciado.");
    } else {
        Serial.println("SD Logger: inicialização falhou ou SD não disponível.");
    }

    tft.init();
    tft.setRotation(0);  // painel físico no seu modo nativo 800×480

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
    disp_drv.hor_res  = 480;  // portrait
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

    // Botão Play → navegar para ScreenRace E iniciar cronômetro
    lv_obj_add_event_cb(ui_Panel7, [](lv_event_t *e) {
        lv_disp_load_scr(ui_ScreenRace);
        startTimer();  // Inicia o cronômetro
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_set_style_text_color(ui_SdReady, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(ui_SdReady, "SD Card Ready");

    // Sincronizar hora via NTP
    Serial.println("\n=== Sincronizando hora com NTP ===");
    syncNTP();
}

// =============================================================
//  NTP SYNCHRONIZATION FUNCTION
// =============================================================
void syncNTP() {
    // Credenciais WiFi - AJUSTE CONFORME NECESSÁRIO
    const char* ssid = "YOUR_SSID";
    const char* password = "YOUR_PASSWORD";

    if (strcmp(ssid, "YOUR_SSID") == 0) {
        Serial.println("NTP ignorado: credenciais WiFi por configurar.");
        return;
    }
    
    Serial.printf("Conectando a WiFi: %s\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi conectado!");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        
        // Configurar timezone (Portugal = UTC+0, ajuste conforme necessário)
        // Para horário de verão: "WET0WEST,M3.5.0/1,M10.5.0"
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        
        Serial.println("Sincronizando hora com NTP...");
        time_t now = time(nullptr);
        int ntpAttempts = 0;
        while (now < 24 * 3600 && ntpAttempts < 20) {
            delay(500);
            Serial.print(".");
            now = time(nullptr);
            ntpAttempts++;
        }
        Serial.println();
        
        if (now > 24 * 3600) {
            struct tm timeinfo = *localtime(&now);
            Serial.printf("Hora sincronizada: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        }
    } else {
        Serial.println("\nNão foi possível conectar a WiFi.");
        Serial.println("Usando hora padrão (1970-01-01 00:00:00)");
    }
    
    // Desligar WiFi para economizar energia
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi desligado.\n");
}

// =============================================================
//  CLOCK UPDATE FUNCTION
// =============================================================
void updateClock() {
    time_t now = time(nullptr);
    struct tm timeinfo = *localtime(&now);
    
    // Atualiza o label de hora no ecrã
    lv_label_set_text_fmt(ui_ValTimer, "%02d:%02d:%02d",
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

// =============================================================
//  TIMER VARIABLES AND FUNCTIONS
// =============================================================
unsigned long timerStartTime = 0;  // Renamed to avoid conflict with ESP32's timerStart()
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
        // Se o timer não está a correr, mostra a hora real
        updateClock();
        return;
    }
    
    unsigned long elapsed = millis() - timerStartTime;
    
    uint32_t seconds = elapsed / 1000;
    uint32_t minutes = seconds / 60;
    uint32_t hours   = minutes / 60;
    
    seconds = seconds % 60;
    minutes = minutes % 60;
    
    // Atualiza o label no ecrã com o cronômetro
    lv_label_set_text_fmt(ui_ValTimer, "%02lu:%02lu:%02lu", hours, minutes, seconds);
}

// =============================================================
//  LOOP
// =============================================================
void loop() {
    updateTimer();           // Atualiza cronômetro
    lv_timer_handler();      // Mantém a UI a correr
    delay(5);
}
