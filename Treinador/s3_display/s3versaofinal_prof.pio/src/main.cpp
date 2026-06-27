#include <Arduino.h>
#include <time.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lvgl.h>
#include "UI/ui.h"


// =============================================================
//  ⚠️ ADIÇÕES PARA RECEBER DADOS DO C3
// =============================================================
#include "SerialTransfer.h"
#include "struct.h"

SerialTransfer myTransfer;
MainData dadosRecebidos;

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
// =============================================================
//  FLUSH COM ROTAÇÃO 270° (Inverte o teu retrato atual em 180°)
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

    Serial.printf("PSRAM Total: %d bytes\n", ESP.getPsramSize());
    Serial.printf("PSRAM Livre: %d bytes\n", ESP.getFreePsram());

    pinMode(38, OUTPUT);
    digitalWrite(38, LOW);  delay(50);
    digitalWrite(38, HIGH); delay(50);

    pinMode(2, OUTPUT);
    digitalWrite(2, HIGH);

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

    Serial.println("\n=== NTP desativado: tempo será tratado pela telemetria ===");


    // =============================================================
    // ⚠️ INICIAR A ESCUTA DOS DADOS DO C3 AQUI (Pino 19)
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
    updateTimer(); 
    
    // =========================================================
    // ⚠️ ESCUTAR DADOS DO C3 CONTINUAMENTE
    // =========================================================
    if (myTransfer.available()) {
        myTransfer.rxObj(dadosRecebidos);
        
        // Imprime no Monitor Serial para testares se os cabos estão bem ligados
        Serial.print("Velocidade (SOG) recebida do C3: ");
        Serial.print(dadosRecebidos.sog);
        Serial.println(" km/h");
        
        // 1. Converter os números para texto no formato esperado pelo ecrã.
        char textoKTS[12];
        char textoHEEL[12];
        char textoTRIM[12];
        char textoHDG[12];

        snprintf(textoKTS, sizeof(textoKTS), "%.1f", dadosRecebidos.sog);
        snprintf(textoHEEL, sizeof(textoHEEL), "%.1f", dadosRecebidos.heel);
        snprintf(textoTRIM, sizeof(textoTRIM), "%.1f", dadosRecebidos.trim);
        snprintf(textoHDG, sizeof(textoHDG), "%.1f", dadosRecebidos.heading);

        // 2. Injetar o texto no respetivo Label do SquareLine
        lv_label_set_text(ui_ValKTS, textoKTS);
        lv_label_set_text(ui_ValHEEL, textoHEEL);
        lv_label_set_text(ui_ValTRIM, textoTRIM);
        lv_label_set_text(ui_ValHDG, textoHDG);
    }// Atualiza cronômetro


    lv_timer_handler();      // Mantém a UI a correr
    delay(5);
}
