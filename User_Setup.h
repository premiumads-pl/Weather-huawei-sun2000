// ==================== ST7789 2.8" + ESP32-S3 (wersja naprawcza) ====================

#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_OFF

// === PINY ===
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    8
#define TFT_RST   9
#define TFT_BL   14

#define TFT_MISO -1              // <-- TO JEST WAŻNE! (dodaj tę linię)

#define TFT_BACKLIGHT_ON HIGH

// Opcjonalnie - czasem pomaga na ESP32-S3
#define USE_HSPI_PORT

#define LOAD_GLCD
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY  80000000