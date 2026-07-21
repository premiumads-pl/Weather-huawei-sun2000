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
// PODSWIETLENIE — CELOWO NIE ODDAJEMY GO BIBLIOTECE.
// Bylo tu '#define TFT_BL 14' razem z TFT_BACKLIGHT_ON HIGH. Skutek: TFT_eSPI::init()
// robi na tym pinie pinMode()+digitalWrite(HIGH) (TFT_eSPI.cpp ~786). W rdzeniu esp32 3.x
// kazde digitalWrite/pinMode idzie przez Peripheral Manager, ktory ODPINA od pinu
// dotychczasowa funkcje — czyli nasz kanal LEDC. Od tej chwili pin siedzi na sztywno
// HIGH (podswietlenie 100%), a kazde pozniejsze ledcWrite() trafia w kanal, ktory nie
// jest juz podlaczony do zadnej nogi. Kod raportuje wtedy poprawne wartosci PWM
// (45/130/255), a ekran swieci caly czas tak samo — awaria wyglada IDENTYCZNIE jak
// przerwany obwod, dlatego dlugo szukalismy jej w sprzecie. Sprzet byl sprawny.
// Pinem 14 steruje wylacznie WeatherUi::begin()/tickBacklight() przez ledcAttach.
// NIE PRZYWRACAJ TFT_BL bez przeniesienia ledcAttach za tft_.init().
// #define TFT_BL   14

#define TFT_MISO -1              // <-- TO JEST WAŻNE! (dodaj tę linię)

// #define TFT_BACKLIGHT_ON HIGH   // patrz komentarz przy TFT_BL powyzej

// Opcjonalnie - czasem pomaga na ESP32-S3
#define USE_HSPI_PORT

#define LOAD_GLCD
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY  80000000