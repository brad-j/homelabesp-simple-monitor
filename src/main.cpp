#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

namespace {

constexpr uint8_t PIN_BACKLIGHT = 27;
constexpr uint8_t PIN_TOUCH_IRQ = 36;
constexpr uint8_t PIN_LED_RED = 22;
constexpr uint8_t PIN_LED_GREEN = 16;
constexpr uint8_t PIN_LED_BLUE = 17;

constexpr uint8_t BACKLIGHT_CHANNEL = 0;
constexpr uint16_t BACKLIGHT_FREQUENCY_HZ = 5000;
constexpr uint8_t BACKLIGHT_RESOLUTION_BITS = 8;
constexpr uint8_t BRIGHTNESS_LEVELS[] = {255, 128, 32};
constexpr uint32_t DIM_AFTER_MS = 30000;
constexpr uint32_t SCREEN_OFF_AFTER_MS = 60000;

// Verified E32R40T raw touch range. TFT_eSPI's calibrated getTouch() rejects
// this panel's normal ADC jitter, so use pressure-gated raw samples instead.
constexpr uint16_t TOUCH_PRESSURE_THRESHOLD = 100;
constexpr uint16_t TOUCH_RAW_X_LEFT = 223;
constexpr uint16_t TOUCH_RAW_X_RIGHT = 3761;
constexpr uint16_t TOUCH_RAW_Y_TOP = 3700;
constexpr uint16_t TOUCH_RAW_Y_BOTTOM = 264;

constexpr uint16_t COLOR_BACKGROUND = 0x0861;
constexpr uint16_t COLOR_PANEL = 0x10E3;
constexpr uint16_t COLOR_LINE = 0x39E7;
constexpr uint16_t COLOR_TEXT = TFT_WHITE;
constexpr uint16_t COLOR_MUTED = 0x9CD3;
constexpr uint16_t COLOR_OK = 0x4EEB;
constexpr uint16_t COLOR_ACCENT = 0x3D7F;
constexpr uint16_t COLOR_WARNING = 0xFD20;

constexpr int16_t SCREEN_WIDTH = 480;
constexpr int16_t SCREEN_HEIGHT = 320;
constexpr int16_t HEADER_HEIGHT = 48;
constexpr int16_t FOOTER_TOP = 264;

struct Button {
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
  const char *label;
};

constexpr Button BUTTON_DIM{12, 272, 136, 40, "DIM"};
constexpr Button BUTTON_RGB{172, 272, 136, 40, "RGB"};
constexpr Button BUTTON_CALIBRATE{332, 272, 136, 40, "TOUCH TEST"};

TFT_eSPI display;
Preferences preferences;
uint16_t touchCalibration[5] = {};
uint32_t lastInteractionAt = 0;
uint32_t touchCount = 0;
uint8_t brightnessIndex = 0;
uint8_t rgbIndex = 0;
bool screenOff = false;
bool touchWasDown = false;

bool contains(const Button &button, uint16_t x, uint16_t y) {
  return x >= button.x && x < button.x + button.width &&
         y >= button.y && y < button.y + button.height;
}

void setBacklight(uint8_t duty) {
  ledcWrite(BACKLIGHT_CHANNEL, duty);
}

void setRgb(uint8_t index) {
  // Common-anode LED: LOW is on.
  digitalWrite(PIN_LED_RED, index == 1 ? LOW : HIGH);
  digitalWrite(PIN_LED_GREEN, index == 2 ? LOW : HIGH);
  digitalWrite(PIN_LED_BLUE, index == 3 ? LOW : HIGH);
}

const char *rgbLabel() {
  switch (rgbIndex) {
    case 1: return "RED";
    case 2: return "GREEN";
    case 3: return "BLUE";
    default: return "OFF";
  }
}

void drawText(const char *text, int16_t x, int16_t y, uint8_t font,
              uint16_t foreground = COLOR_TEXT,
              uint16_t background = COLOR_BACKGROUND) {
  display.setTextFont(font);
  display.setTextColor(foreground, background);
  display.setTextDatum(TL_DATUM);
  display.drawString(text, x, y);
}

void drawButton(const Button &button) {
  display.fillRect(button.x, button.y, button.width, button.height, COLOR_PANEL);
  display.drawRect(button.x, button.y, button.width, button.height, COLOR_LINE);
  display.setTextFont(2);
  display.setTextColor(COLOR_TEXT, COLOR_PANEL);
  display.setTextDatum(MC_DATUM);
  display.drawString(button.label,
                     button.x + button.width / 2,
                     button.y + button.height / 2);
}

void drawStatusCard(int16_t x, int16_t y, int16_t width, const char *eyebrow,
                    const char *value, const char *detail,
                    uint16_t valueColor = COLOR_OK) {
  display.fillRect(x, y, width, 86, COLOR_PANEL);
  display.drawRect(x, y, width, 86, COLOR_LINE);
  drawText(eyebrow, x + 10, y + 8, 2, COLOR_MUTED, COLOR_PANEL);
  drawText(value, x + 10, y + 29, 4, valueColor, COLOR_PANEL);
  drawText(detail, x + 10, y + 64, 2, COLOR_MUTED, COLOR_PANEL);
}

String deviceId() {
  const uint64_t mac = ESP.getEfuseMac();
  char value[13];
  snprintf(value, sizeof(value), "%04X%08X",
           static_cast<uint16_t>(mac >> 32),
           static_cast<uint32_t>(mac));
  return String(value);
}

void drawDashboard() {
  display.fillScreen(COLOR_BACKGROUND);
  display.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, TFT_BLACK);
  drawText("HOMELAB PANEL", 12, 7, 4, COLOR_TEXT, TFT_BLACK);
  drawText("HARDWARE CHECK", 352, 15, 2, COLOR_ACCENT, TFT_BLACK);

  drawStatusCard(12, 60, 220, "DISPLAY", "PASS", "ST7796 / 480 x 320");
  drawStatusCard(248, 60, 220, "TOUCH", "READY", "Tap anywhere to verify");

  char memoryValue[24];
  snprintf(memoryValue, sizeof(memoryValue), "%u KB",
           static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  drawStatusCard(12, 158, 220, "FREE HEAP", memoryValue,
                 psramFound() ? "PSRAM detected" : "No PSRAM (expected)",
                 COLOR_ACCENT);

  const String id = deviceId();
  drawStatusCard(248, 158, 220, "DEVICE", id.c_str(), "ESP32-D0WD-V3 / 4 MB",
                 COLOR_ACCENT);

  display.drawFastHLine(0, FOOTER_TOP, SCREEN_WIDTH, COLOR_LINE);
  drawButton(BUTTON_DIM);
  drawButton(BUTTON_RGB);
  drawButton(BUTTON_CALIBRATE);
}

void updateTouchCard(uint16_t x, uint16_t y) {
  display.fillRect(249, 61, 218, 84, COLOR_PANEL);
  drawText("TOUCH", 258, 68, 2, COLOR_MUTED, COLOR_PANEL);

  char coordinates[32];
  snprintf(coordinates, sizeof(coordinates), "%u, %u", x, y);
  drawText(coordinates, 258, 89, 4, COLOR_OK, COLOR_PANEL);

  char detail[32];
  snprintf(detail, sizeof(detail), "Tap %lu / IRQ %s",
           static_cast<unsigned long>(touchCount),
           digitalRead(PIN_TOUCH_IRQ) == LOW ? "LOW" : "HIGH");
  drawText(detail, 258, 124, 2, COLOR_MUTED, COLOR_PANEL);
}

void updateFooterState() {
  char text[32];

  display.fillRect(BUTTON_DIM.x + 1, BUTTON_DIM.y + 1,
                   BUTTON_DIM.width - 2, BUTTON_DIM.height - 2, COLOR_PANEL);
  snprintf(text, sizeof(text), "DIM  %u%%",
           static_cast<unsigned>((BRIGHTNESS_LEVELS[brightnessIndex] * 100U) / 255U));
  display.setTextFont(2);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COLOR_TEXT, COLOR_PANEL);
  display.drawString(text, BUTTON_DIM.x + BUTTON_DIM.width / 2,
                     BUTTON_DIM.y + BUTTON_DIM.height / 2);

  display.fillRect(BUTTON_RGB.x + 1, BUTTON_RGB.y + 1,
                   BUTTON_RGB.width - 2, BUTTON_RGB.height - 2, COLOR_PANEL);
  snprintf(text, sizeof(text), "RGB  %s", rgbLabel());
  display.drawString(text, BUTTON_RGB.x + BUTTON_RGB.width / 2,
                     BUTTON_RGB.y + BUTTON_RGB.height / 2);
}

bool loadTouchCalibration() {
  preferences.begin("diagnostic", true);
  const size_t length = preferences.getBytesLength("touch-cal");
  if (length == sizeof(touchCalibration)) {
    preferences.getBytes("touch-cal", touchCalibration, sizeof(touchCalibration));
  }
  preferences.end();
  return length == sizeof(touchCalibration);
}

void calibrateTouch() {
  setBacklight(255);
  screenOff = false;
  display.fillScreen(TFT_BLACK);
  display.setTextDatum(TL_DATUM);
  display.setTextFont(2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("Touch the highlighted corners", 12, 12);
  display.calibrateTouch(touchCalibration, COLOR_ACCENT, TFT_BLACK, 15);
  display.setTouch(touchCalibration);

  preferences.begin("diagnostic", false);
  preferences.putBytes("touch-cal", touchCalibration, sizeof(touchCalibration));
  preferences.end();

  Serial.printf("Touch calibration: %u, %u, %u, %u, %u\n",
                touchCalibration[0], touchCalibration[1], touchCalibration[2],
                touchCalibration[3], touchCalibration[4]);
  lastInteractionAt = millis();
  drawDashboard();
  updateFooterState();
}

void runVisualTest() {
  constexpr uint16_t colors[] = {
      TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK};
  for (const uint16_t color : colors) {
    display.fillScreen(color);
    delay(220);
  }
}

void runRgbTest() {
  for (uint8_t index = 1; index <= 3; ++index) {
    setRgb(index);
    delay(180);
  }
  setRgb(0);
}

uint16_t mapTouchX(uint16_t rawY) {
  // Rotation 1: portrait Y becomes landscape X.
  return constrain(map(static_cast<int32_t>(rawY), TOUCH_RAW_Y_TOP,
                       TOUCH_RAW_Y_BOTTOM, 0, SCREEN_WIDTH),
                   0, SCREEN_WIDTH - 1);
}

uint16_t mapTouchY(uint16_t rawX) {
  // Rotation 1: portrait X becomes inverted landscape Y.
  return constrain(map(static_cast<int32_t>(rawX), TOUCH_RAW_X_LEFT,
                       TOUCH_RAW_X_RIGHT, SCREEN_HEIGHT - 1, 0),
                   0, SCREEN_HEIGHT - 1);
}

void handlePress(uint16_t x, uint16_t y) {
  ++touchCount;
  lastInteractionAt = millis();

  if (screenOff) {
    screenOff = false;
    setBacklight(BRIGHTNESS_LEVELS[brightnessIndex]);
    drawDashboard();
    updateFooterState();
    return;
  }

  if (contains(BUTTON_DIM, x, y)) {
    brightnessIndex = (brightnessIndex + 1) %
                      (sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]));
    setBacklight(BRIGHTNESS_LEVELS[brightnessIndex]);
    updateFooterState();
  } else if (contains(BUTTON_RGB, x, y)) {
    rgbIndex = (rgbIndex + 1) % 4;
    setRgb(rgbIndex);
    updateFooterState();
  } else {
    updateTouchCard(x, y);
  }

  Serial.printf("Touch #%lu: x=%u y=%u irq=%d\n",
                static_cast<unsigned long>(touchCount), x, y,
                digitalRead(PIN_TOUCH_IRQ));
}

void handleIdleDisplay() {
  const uint32_t idleFor = millis() - lastInteractionAt;
  if (!screenOff && idleFor >= SCREEN_OFF_AFTER_MS) {
    setBacklight(0);
    screenOff = true;
  } else if (!screenOff && idleFor >= DIM_AFTER_MS) {
    setBacklight(24);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\nHomelab Panel hardware diagnostic");

  pinMode(PIN_TOUCH_IRQ, INPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  setRgb(0);

  ledcSetup(BACKLIGHT_CHANNEL, BACKLIGHT_FREQUENCY_HZ,
            BACKLIGHT_RESOLUTION_BITS);
  ledcAttachPin(PIN_BACKLIGHT, BACKLIGHT_CHANNEL);
  setBacklight(255);

  display.init();
  display.setRotation(1);
  display.setSwapBytes(true);

  runVisualTest();
  runRgbTest();

  drawDashboard();
  updateFooterState();

  lastInteractionAt = millis();
  Serial.printf("Display: %d x %d\n", display.width(), display.height());
  Serial.printf("Free heap: %u bytes; PSRAM: %s\n", ESP.getFreeHeap(),
                psramFound() ? "yes" : "no");
  Serial.printf("Device ID: %s\n", deviceId().c_str());
}

void loop() {
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  display.getTouchRaw(&rawX, &rawY);
  const uint16_t pressure = display.getTouchRawZ();
  const bool touchDown =
      digitalRead(PIN_TOUCH_IRQ) == LOW && pressure > TOUCH_PRESSURE_THRESHOLD;

  if (touchDown && !touchWasDown) {
    // Let the resistive panel settle after initial contact, then sample again.
    delay(15);
    display.getTouchRaw(&rawX, &rawY);
    const uint16_t x = mapTouchX(rawY);
    const uint16_t y = mapTouchY(rawX);
    handlePress(x, y);
    Serial.printf("Raw touch: x=%u y=%u z=%u\n", rawX, rawY, pressure);
  }
  touchWasDown = touchDown;

  handleIdleDisplay();
  delay(12);
}
