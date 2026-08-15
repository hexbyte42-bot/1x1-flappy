/*
 * touch_test.ino — robust touch probe for Waveshare ESP32-S3-Touch-AMOLED-1.8
 *
 * Diagnostics: every init step logs to USB serial; I2C bus scan on boot;
 * touch init auto-retries; loop() never dereferences a null touch pointer.
 *
 * ENABLE_OFFICIAL_INIT: replicate the official 02_Drawing_board boot sequence
 * (TCA9554 pins 0,1,2 LOW->HIGH pulse + touch 0xFA=0x40 write) — toggle to
 * compare behaviour.
 */
#include <Arduino.h>
#include <Wire.h>
#include "pin_config.h"
#include "HWCDC.h"
#include "Arduino_GFX_Library.h"
#include "hw_panel.h"

#define ENABLE_OFFICIAL_INIT 1

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_OLED *gfx = nullptr;
TouchDrvInterface *touch = nullptr;

// Keypad geometry — identical to the app
#define KEY_X0     8
#define KEY_Y0     170
#define KEY_W      104
#define KEY_H      62
#define KEY_GAPX   10
#define KEY_GAPY   8

#if ENABLE_OFFICIAL_INIT
// TCA9554 @0x20: pins 0,1,2 pulsed LOW then HIGH (official example sequence)
static void officialExpanderInit() {
    const uint8_t addr = 0x20;
    Wire.beginTransmission(addr);
    Wire.write(0x03); Wire.write(0xF8);   // config: pins 0,1,2 output
    Wire.endTransmission();
    Wire.beginTransmission(addr);
    Wire.write(0x01); Wire.write(0x00);   // outputs LOW
    Wire.endTransmission();
    delay(20);
    Wire.beginTransmission(addr);
    Wire.write(0x01); Wire.write(0x07);   // pins 0,1,2 HIGH
    Wire.endTransmission();
}

static void officialIrqCtlWrite() {       // 0xFA = 0x40 periodic interrupt
    Wire.beginTransmission(0x15);
    Wire.write(0xFA); Wire.write(0x40);
    Wire.endTransmission();
}
#endif

static void drawGrid() {
    gfx->fillScreen(0xFFFF);
    const char *labels[12] = {
        "1","2","3",
        "4","5","6",
        "7","8","9",
        "" ,"0",""
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            if (labels[idx][0] == '\0') continue;
            int x = KEY_X0 + c * (KEY_W + KEY_GAPX);
            int y = KEY_Y0 + r * (KEY_H + KEY_GAPY);
            gfx->fillRoundRect(x, y, KEY_W, KEY_H, 8, 0x6B4D);
            gfx->setTextSize(2);
            gfx->setTextColor(0x0000);
            gfx->setCursor(x + KEY_W / 2 - 7, y + KEY_H / 2 - 9);
            gfx->print(labels[idx]);
        }
    }
    gfx->setTextSize(1);
    gfx->setTextColor(0xF800);
    gfx->setCursor(6, 6);
    gfx->print("red=raw green=corr");
    gfx->setCursor(6, 150);
    gfx->print("Row3: 7 8 9  Row4: 0");
}

void setup() {
    USBSerial.begin(115200);
    // Wait until a host opens the USB-CDC serial monitor, so no boot log is lost.
    while (!USBSerial) { delay(50); }
    delay(100);
    USBSerial.println("=== touch_test v4 ===");

    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);
    USBSerial.println("[ok] wire");

    // I2C bus scan (diagnostics)
    for (uint8_t a = 8; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0)
            USBSerial.printf("[i2c] 0x%02X\n", a);
    }

#if ENABLE_OFFICIAL_INIT
    officialExpanderInit();
    USBSerial.println("[ok] expander pulse");
#endif

    gfx = make_display(bus);
    if (!gfx->begin())
        USBSerial.println("[!!] display begin FAILED");
    gfx->setBrightness(255);
    USBSerial.println("[ok] display");

    // Touch init with retries (official example retries too)
    for (int i = 0; i < 10 && !touch; i++) {
        touch = make_touch();
        if (!touch) {
            USBSerial.printf("[..] touch retry %d\n", i);
            delay(300);
        }
    }
    if (touch)
        USBSerial.printf("[ok] touch: %s chip=0x%02X\n",
                         touch->getModelName(), (unsigned)touch->getChipID());
    else
        USBSerial.println("[!!] TOUCH INIT FAILED");

#if ENABLE_OFFICIAL_INIT
    officialIrqCtlWrite();
    USBSerial.println("[ok] irq ctl write");
#endif

    drawGrid();
    USBSerial.println("[ok] boot complete");
}

void loop() {
    if (!touch) {           // never crash on null
        delay(500);
        return;
    }
    int16_t x = -1, y = -1;
    bool present = touch->getPoint(&x, &y, 1);
    if (present && x >= 0 && y >= 0) {
        static uint32_t lastPrint = 0;
        if (millis() - lastPrint > 80) {
            lastPrint = millis();
            int16_t cx = x, cy = y;
            if (touch->getChipID() == 0xB7) {
                cy = (int16_t)(((int32_t)cy + 15) * 7 / 8);
                if (cy < 0) cy = 0;
            }
            USBSerial.printf("RAW %d %d  CORR %d %d\n",
                             (int)x, (int)y, (int)cx, (int)cy);
        }
        if (x < LCD_WIDTH && y < LCD_HEIGHT)
            gfx->fillCircle(x, y, 4, 0xF800);
        if (touch->getChipID() == 0xB7) {
            int16_t cy = (int16_t)(((int32_t)y + 15) * 7 / 8);
            if (cy >= 0 && cy < LCD_HEIGHT && x < LCD_WIDTH)
                gfx->fillCircle(x, cy, 4, 0x07E0);
        }
    }
    delay(15);
}
