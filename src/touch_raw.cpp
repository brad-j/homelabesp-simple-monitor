#include <Arduino.h>
#include <TFT_eSPI.h>

namespace {

constexpr uint8_t PIN_BACKLIGHT = 27;
constexpr uint8_t PIN_TOUCH_IRQ = 36;
constexpr uint8_t PIN_LED_RED = 22;
constexpr uint8_t PIN_LED_GREEN = 16;
constexpr uint8_t PIN_LED_BLUE = 17;

constexpr uint16_t COLOR_BACKGROUND = 0x0861;
constexpr uint16_t COLOR_PANEL = 0x10E3;
constexpr uint16_t COLOR_LINE = 0x39E7;
constexpr uint16_t COLOR_MUTED = 0x9CD3;
constexpr uint16_t COLOR_ACCENT = 0x3D7F;
constexpr uint16_t COLOR_OK = 0x4EEB;
constexpr uint16_t COLOR_BAD = 0xF986;

TFT_eSPI display;
uint32_t lastUpdateAt = 0;

void drawLabel(const char *text, int16_t x, int16_t y) {
  display.setTextDatum(TL_DATUM);
  display.setTextFont(2);
  display.setTextColor(COLOR_MUTED, COLOR_PANEL);
  display.drawString(text, x, y);
}

void drawValue(const char *text, int16_t x, int16_t y, uint16_t color) {
  display.fillRect(x, y, 190, 50, COLOR_PANEL);
  display.setTextDatum(TL_DATUM);
  display.setTextFont(6);
  display.setTextColor(color, COLOR_PANEL);
  display.drawString(text, x, y);
}

void drawPanel(int16_t x, int16_t y, int16_t width, const char *label) {
  display.fillRect(x, y, width, 92, COLOR_PANEL);
  display.drawRect(x, y, width, 92, COLOR_LINE);
  drawLabel(label, x + 12, y + 8);
}

void drawScreen() {
  display.fillScreen(COLOR_BACKGROUND);
  display.fillRect(0, 0, 480, 50, TFT_BLACK);

  display.setTextDatum(TL_DATUM);
  display.setTextFont(4);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("TOUCH RAW DIAGNOSTIC", 12, 10);

  display.setTextFont(2);
  display.setTextColor(COLOR_ACCENT, TFT_BLACK);
  display.drawString("XPT2046 / GPIO 33 CS / GPIO 36 IRQ", 244, 17);

  drawPanel(12, 64, 220, "RAW X");
  drawPanel(248, 64, 220, "RAW Y");
  drawPanel(12, 168, 220, "PRESSURE Z");
  drawPanel(248, 168, 220, "IRQ LEVEL");

  display.fillRect(0, 276, 480, 44, TFT_BLACK);
  display.setTextFont(2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("Press firmly with the stylus. Report X, Y, Z, and IRQ.", 12, 291);
}

void setLed(bool pressed) {
  // Common-anode RGB LED. Green means touch activity; red means idle.
  digitalWrite(PIN_LED_RED, pressed ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, pressed ? LOW : HIGH);
  digitalWrite(PIN_LED_BLUE, HIGH);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, HIGH);
  pinMode(PIN_TOUCH_IRQ, INPUT);

  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  setLed(false);

  display.init();
  display.setRotation(1);
  drawScreen();

  Serial.println("\nRaw XPT2046 diagnostic");
  Serial.println("Expected idle: IRQ=HIGH and Z near 0");
  Serial.println("Expected press: IRQ=LOW and Z increases");
}

void loop() {
  if (millis() - lastUpdateAt < 100) {
    delay(5);
    return;
  }
  lastUpdateAt = millis();

  const int irq = digitalRead(PIN_TOUCH_IRQ);
  const uint16_t pressure = display.getTouchRawZ();
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  display.getTouchRaw(&rawX, &rawY);

  const bool pressed = irq == LOW || pressure > 40;
  const uint16_t stateColor = pressed ? COLOR_OK : COLOR_BAD;
  setLed(pressed);

  char value[24];
  snprintf(value, sizeof(value), "%u", rawX);
  drawValue(value, 24, 94, pressed ? COLOR_OK : TFT_WHITE);

  snprintf(value, sizeof(value), "%u", rawY);
  drawValue(value, 260, 94, pressed ? COLOR_OK : TFT_WHITE);

  snprintf(value, sizeof(value), "%u", pressure);
  drawValue(value, 24, 198, stateColor);

  drawValue(irq == LOW ? "LOW" : "HIGH", 260, 198, stateColor);

  Serial.printf("raw=(%u,%u) z=%u irq=%s\n", rawX, rawY, pressure,
                irq == LOW ? "LOW" : "HIGH");
}
