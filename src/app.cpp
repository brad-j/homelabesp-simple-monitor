/* Hallmark · pre-emit critique: P5 H5 E5 S5 R5 V5
 * genre: modern-minimal · macrostructure: Workbench
 * theme: Cobalt-dark · enrichment: live topology + vector weather glyphs · navigation: bottom instrument rail
 * audience: homelab operators · use: health and guarded recovery · tone: technical
 * Hardware constraint: LVGL built-in Montserrat avoids external font assets on 4 MB/no-PSRAM.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_system.h>
#include <lvgl.h>

#include "config.h"

namespace {

constexpr uint8_t PIN_BACKLIGHT = 27;
constexpr uint8_t PIN_TOUCH_IRQ = 36;
constexpr uint8_t BACKLIGHT_CHANNEL = 0;
constexpr uint16_t BACKLIGHT_FREQUENCY_HZ = 5000;
constexpr uint8_t BACKLIGHT_RESOLUTION_BITS = 8;
constexpr uint8_t BACKLIGHT_ACTIVE = 220;
constexpr uint8_t BACKLIGHT_DIMMED = 28;
constexpr uint32_t DIM_AFTER_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t SCREEN_OFF_AFTER_MS = 10UL * 60UL * 1000UL;

constexpr uint16_t SCREEN_WIDTH = 480;
constexpr uint16_t SCREEN_HEIGHT = 320;
constexpr uint16_t HEADER_HEIGHT = 42;
constexpr uint16_t NAV_HEIGHT = 48;
constexpr uint16_t CONTENT_HEIGHT = SCREEN_HEIGHT - HEADER_HEIGHT - NAV_HEIGHT;

constexpr uint16_t TOUCH_PRESSURE_THRESHOLD = 100;
constexpr uint16_t TOUCH_RAW_X_LEFT = 223;
constexpr uint16_t TOUCH_RAW_X_RIGHT = 3761;
constexpr uint16_t TOUCH_RAW_Y_TOP = 3700;
constexpr uint16_t TOUCH_RAW_Y_BOTTOM = 264;

// Locked Cobalt-dark tokens. RGB888 values are converted by LVGL/TFT_eSPI.
constexpr uint32_t COLOR_PAPER = 0x0B1018;
constexpr uint32_t COLOR_SURFACE = 0x131B26;
constexpr uint32_t COLOR_SURFACE_RAISED = 0x192331;
constexpr uint32_t COLOR_RULE = 0x2D3A4C;
constexpr uint32_t COLOR_INK = 0xEDF3F8;
constexpr uint32_t COLOR_MUTED = 0x91A0B2;
constexpr uint32_t COLOR_ACCENT = 0x4C8DFF;
constexpr uint32_t COLOR_OK = 0x56D3A2;
constexpr uint32_t COLOR_WARNING = 0xF0B35A;
constexpr uint32_t COLOR_DANGER = 0xFF7373;

constexpr char SNAPSHOT_URL[] = HOMELAB_DOCKER_SNAPSHOT_URL;
constexpr char METRICS_URL[] = HOMELAB_BESZEL_METRICS_URL;
constexpr char WEATHER_URL[] =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=42.55843&longitude=-71.26895"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
    "weather_code,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
    "precipitation_probability_max"
    "&temperature_unit=fahrenheit&wind_speed_unit=mph"
    "&timezone=America%2FNew_York&forecast_days=5";
constexpr uint32_t REFRESH_INTERVAL_MS = 30000;
constexpr uint32_t METRICS_REFRESH_INTERVAL_MS = 60000;
constexpr uint32_t WEATHER_REFRESH_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_INTERVAL_MS = 60UL * 1000UL;
constexpr uint8_t MAX_HOSTS = 4;
constexpr uint8_t MAX_METRIC_SYSTEMS = 6;
constexpr uint8_t MAX_DOCKER_ISSUES = 8;
constexpr uint8_t METRIC_HISTORY_POINTS = 60;
constexpr uint8_t FORECAST_DAYS = 5;
constexpr uint16_t DRAW_BUFFER_LINES = 20;

struct HostSummary {
  char name[20] = "—";
  bool reachable = false;
  uint8_t workloads = 0;
  uint8_t issues = 0;
};

struct DockerIssue {
  char subject[20] = "—";
  char detail[36] = "—";
  uint32_t severityColor = COLOR_WARNING;
};

struct Snapshot {
  bool ready = false;
  bool loading = true;
  char error[72] = "Waiting for the first snapshot";
  char generatedAt[32] = "—";
  HostSummary hosts[MAX_HOSTS];
  DockerIssue issueDetails[MAX_DOCKER_ISSUES];
  uint8_t hostCount = 0;
  uint8_t reachableHosts = 0;
  uint8_t workloads = 0;
  uint8_t issues = 0;
  uint8_t issueDetailCount = 0;
  uint32_t fetchedAt = 0;
};

struct MetricSystem {
  char name[20] = "—";
  char updatedAt[32] = "—";
  bool reachable = false;
  float cpu = -1.0f;
  float memory = -1.0f;
  float disk = -1.0f;
  float temperature = -1.0f;
  float load1 = -1.0f;
  float networkRx = -1.0f;
  float networkTx = -1.0f;
  int16_t cpuHistory[METRIC_HISTORY_POINTS] = {};
  int16_t memoryHistory[METRIC_HISTORY_POINTS] = {};
  uint8_t sampleCount = 0;
};

struct MetricsSnapshot {
  bool ready = false;
  bool loading = true;
  char error[72] = "Waiting for Beszel metrics";
  char generatedAt[32] = "—";
  MetricSystem systems[MAX_METRIC_SYSTEMS];
  uint8_t systemCount = 0;
  uint32_t fetchedAt = 0;
};

struct DayForecast {
  char date[11] = "";
  int16_t code = -1;
  float high = NAN;
  float low = NAN;
  int16_t precipitation = -1;
};

struct WeatherSnapshot {
  bool ready = false;
  bool loading = true;
  char error[72] = "Waiting for weather data";
  char observedAt[17] = "";
  float temperature = NAN;
  float apparent = NAN;
  float humidity = NAN;
  float wind = NAN;
  float gust = NAN;
  float pressure = NAN;
  int16_t windDirection = -1;
  int16_t code = -1;
  DayForecast days[FORECAST_DAYS];
  uint32_t fetchedAt = 0;
};

enum class Page : uint8_t { OVERVIEW, SYSTEMS, ISSUES, DEVICE, WEATHER };

TFT_eSPI display;
lv_disp_draw_buf_t drawBuffer;
lv_color_t drawPixels[SCREEN_WIDTH * DRAW_BUFFER_LINES];
lv_obj_t *content = nullptr;
lv_obj_t *networkLabel = nullptr;
lv_obj_t *navButtons[5] = {};
Snapshot snapshot;
MetricsSnapshot metrics;
WeatherSnapshot weather;
Page currentPage = Page::OVERVIEW;
int8_t selectedMetricSystem = -1;
uint32_t lastInteractionAt = 0;
uint32_t lastRefreshAt = 0;
uint32_t lastMetricsRefreshAt = 0;
uint32_t lastWeatherAttemptAt = 0;
bool screenOff = false;
bool suppressTouchUntilRelease = false;

lv_color_t color(uint32_t token) {
  return lv_color_hex(token);
}

uint16_t color565(uint32_t token) {
  return display.color565((token >> 16) & 0xFF, (token >> 8) & 0xFF,
                          token & 0xFF);
}

uint16_t mapTouchX(uint16_t rawY) {
  return constrain(map(static_cast<int32_t>(rawY), TOUCH_RAW_Y_TOP,
                       TOUCH_RAW_Y_BOTTOM, 0, SCREEN_WIDTH),
                   0, SCREEN_WIDTH - 1);
}

uint16_t mapTouchY(uint16_t rawX) {
  return constrain(map(static_cast<int32_t>(rawX), TOUCH_RAW_X_LEFT,
                       TOUCH_RAW_X_RIGHT, SCREEN_HEIGHT - 1, 0),
                   0, SCREEN_HEIGHT - 1);
}

void setBacklight(uint8_t duty) {
  ledcWrite(BACKLIGHT_CHANNEL, duty);
}

void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area,
                  lv_color_t *pixels) {
  const uint32_t width = area->x2 - area->x1 + 1;
  const uint32_t height = area->y2 - area->y1 + 1;
  display.startWrite();
  display.setAddrWindow(area->x1, area->y1, width, height);
  display.pushColors(reinterpret_cast<uint16_t *>(&pixels->full),
                     width * height, true);
  display.endWrite();
  lv_disp_flush_ready(driver);
}

void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  display.getTouchRaw(&rawX, &rawY);
  const uint16_t pressure = display.getTouchRawZ();
  const bool pressed = digitalRead(PIN_TOUCH_IRQ) == LOW &&
                       pressure > TOUCH_PRESSURE_THRESHOLD;

  if (screenOff && pressed) {
    screenOff = false;
    suppressTouchUntilRelease = true;
    setBacklight(BACKLIGHT_ACTIVE);
    lastInteractionAt = millis();
  }

  if (suppressTouchUntilRelease) {
    data->state = LV_INDEV_STATE_REL;
    if (!pressed) suppressTouchUntilRelease = false;
    return;
  }

  if (pressed) {
    data->point.x = mapTouchX(rawY);
    data->point.y = mapTouchY(rawX);
    data->state = LV_INDEV_STATE_PR;
    lastInteractionAt = millis();
    if (!screenOff) setBacklight(BACKLIGHT_ACTIVE);
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void clearObjectStyle(lv_obj_t *object) {
  lv_obj_remove_style_all(object);
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, int16_t x, int16_t y,
                    const lv_font_t *font, uint32_t textColor) {
  lv_obj_t *label = lv_label_create(parent);
  clearObjectStyle(label);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color(textColor), 0);
  return label;
}

lv_obj_t *makePanel(int16_t x, int16_t y, int16_t width, int16_t height) {
  lv_obj_t *panel = lv_obj_create(content);
  clearObjectStyle(panel);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_size(panel, width, height);
  lv_obj_set_style_bg_color(panel, color(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(panel, color(COLOR_RULE), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_radius(panel, 5, 0);
  return panel;
}

lv_obj_t *makeSignal(lv_obj_t *parent, int16_t x, int16_t y,
                     uint32_t signalColor) {
  lv_obj_t *signal = lv_obj_create(parent);
  clearObjectStyle(signal);
  lv_obj_set_pos(signal, x, y);
  lv_obj_set_size(signal, 7, 7);
  lv_obj_set_style_bg_color(signal, color(signalColor), 0);
  lv_obj_set_style_bg_opa(signal, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(signal, 1, 0);
  return signal;
}

void formatMetric(char *output, size_t size, float value, const char *suffix) {
  if (value < 0.0f) {
    strlcpy(output, "—", size);
    return;
  }
  snprintf(output, size, value >= 99.95f ? "%.0f%s" : "%.1f%s", value,
           suffix);
}

void formatRate(char *output, size_t size, float bytesPerSecond) {
  if (bytesPerSecond < 0.0f) {
    strlcpy(output, "—", size);
  } else if (bytesPerSecond >= 1000000.0f) {
    snprintf(output, size, "%.1f MB/s", bytesPerSecond / 1000000.0f);
  } else if (bytesPerSecond >= 1000.0f) {
    snprintf(output, size, "%.1f KB/s", bytesPerSecond / 1000.0f);
  } else {
    snprintf(output, size, "%.0f B/s", bytesPerSecond);
  }
}

void renderCurrentPage();

uint32_t metricIssueColor(const MetricSystem &system) {
  if (!system.reachable) return COLOR_DANGER;
  if (system.disk >= 95.0f || system.temperature >= 85.0f ||
      system.memory >= 97.0f || system.cpu >= 98.0f)
    return COLOR_DANGER;
  if (system.disk >= 85.0f || system.temperature >= 75.0f ||
      system.memory >= 90.0f || system.cpu >= 95.0f)
    return COLOR_WARNING;
  return 0;
}

void formatMetricIssue(char *output, size_t size, const MetricSystem &system) {
  if (!system.reachable) {
    strlcpy(output, "System unreachable", size);
  } else if (system.disk >= 95.0f) {
    snprintf(output, size, "Disk critical %.0f%%", system.disk);
  } else if (system.temperature >= 85.0f) {
    snprintf(output, size, "Temperature critical %.0f°", system.temperature);
  } else if (system.memory >= 97.0f) {
    snprintf(output, size, "Memory critical %.0f%%", system.memory);
  } else if (system.cpu >= 98.0f) {
    snprintf(output, size, "CPU critical %.0f%%", system.cpu);
  } else if (system.disk >= 85.0f) {
    snprintf(output, size, "Disk high %.0f%%", system.disk);
  } else if (system.temperature >= 75.0f) {
    snprintf(output, size, "Temperature high %.0f°", system.temperature);
  } else if (system.memory >= 90.0f) {
    snprintf(output, size, "Memory high %.0f%%", system.memory);
  } else if (system.cpu >= 95.0f) {
    snprintf(output, size, "CPU high %.0f%%", system.cpu);
  } else {
    output[0] = '\0';
  }
}

bool feedIsStale(bool ready, uint32_t fetchedAt, uint32_t interval) {
  return ready && fetchedAt != 0 && millis() - fetchedAt > interval * 3;
}

void formatFeedStatus(char *output, size_t size, bool ready, uint32_t fetchedAt,
                      uint32_t interval) {
  if (!ready || fetchedAt == 0) {
    strlcpy(output, "OFFLINE", size);
    return;
  }
  const uint32_t ageSeconds = (millis() - fetchedAt) / 1000UL;
  if (ageSeconds > (interval * 3) / 1000UL) {
    snprintf(output, size, "STALE · %lu min",
             static_cast<unsigned long>(ageSeconds / 60UL));
  } else {
    snprintf(output, size, "LIVE · %lu s",
             static_cast<unsigned long>(ageSeconds));
  }
}

bool metricsHaveIssue() {
  if (!metrics.ready || feedIsStale(metrics.ready, metrics.fetchedAt,
                                    METRICS_REFRESH_INTERVAL_MS))
    return true;
  for (uint8_t index = 0; index < metrics.systemCount; ++index) {
    if (metricIssueColor(metrics.systems[index]) != 0) return true;
  }
  return false;
}

const char *overallState() {
  if (!WiFi.isConnected() || !snapshot.ready) return "OFFLINE";
  if (snapshot.reachableHosts == snapshot.hostCount && snapshot.issues == 0 &&
      !metricsHaveIssue())
    return "NOMINAL";
  return "ATTENTION";
}

uint32_t overallColor() {
  if (!WiFi.isConnected() || !snapshot.ready) return COLOR_DANGER;
  if (snapshot.reachableHosts == snapshot.hostCount && snapshot.issues == 0 &&
      !metricsHaveIssue())
    return COLOR_OK;
  return COLOR_WARNING;
}

void updateNavigation() {
  for (uint8_t index = 0; index < 5; ++index) {
    const bool active = index == static_cast<uint8_t>(currentPage);
    lv_obj_set_style_text_color(navButtons[index],
                                color(active ? COLOR_ACCENT : COLOR_MUTED), 0);
    lv_obj_set_style_border_color(navButtons[index], color(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(navButtons[index], active ? 2 : 0, 0);
    lv_obj_set_style_border_side(navButtons[index], LV_BORDER_SIDE_BOTTOM, 0);
  }
}

void setOpacityAnimationValue(void *target, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(target),
                       static_cast<lv_opa_t>(value), 0);
}

void animateReveal(lv_obj_t *object, uint32_t delayMs) {
  lv_obj_set_style_opa(object, LV_OPA_20, 0);
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, object);
  lv_anim_set_exec_cb(&animation, setOpacityAnimationValue);
  lv_anim_set_values(&animation, LV_OPA_20, LV_OPA_COVER);
  lv_anim_set_time(&animation, 220);
  lv_anim_set_delay(&animation, delayMs);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
  lv_anim_start(&animation);
}

void animateSignal(lv_obj_t *signal, uint32_t delayMs) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, signal);
  lv_anim_set_exec_cb(&animation, setOpacityAnimationValue);
  lv_anim_set_values(&animation, LV_OPA_COVER, LV_OPA_40);
  lv_anim_set_time(&animation, 720);
  lv_anim_set_playback_time(&animation, 720);
  lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_delay(&animation, delayMs);
  lv_anim_start(&animation);
}

void renderOverview() {
  makeLabel(content, "LIVE TOPOLOGY", 10, 3, &lv_font_montserrat_16,
            COLOR_INK);
  lv_obj_t *feedLabel =
      makeLabel(content, snapshot.ready && metrics.ready ? "FEEDS · LIVE"
                                                        : "FEEDS · DEGRADED",
                0, 5, &lv_font_montserrat_14,
                snapshot.ready && metrics.ready ? COLOR_OK : COLOR_WARNING);
  lv_obj_align(feedLabel, LV_ALIGN_TOP_RIGHT, -10, 5);

  const uint8_t reachability =
      snapshot.ready && snapshot.hostCount
          ? static_cast<uint8_t>((snapshot.reachableHosts * 100U) /
                                 snapshot.hostCount)
          : 0;
  lv_obj_t *healthCore = lv_obj_create(content);
  clearObjectStyle(healthCore);
  lv_obj_set_pos(healthCore, 24, 27);
  lv_obj_set_size(healthCore, 158, 158);
  constexpr uint8_t RING_SEGMENTS = 24;
  const uint8_t activeSegments =
      static_cast<uint8_t>((reachability * RING_SEGMENTS) / 100U);
  for (uint8_t index = 0; index < RING_SEGMENTS; ++index) {
    const float angle = (120.0f + index * (300.0f / (RING_SEGMENTS - 1))) *
                        PI / 180.0f;
    const int16_t segmentX =
        76 + static_cast<int16_t>(cosf(angle) * 67.0f) - 3;
    const int16_t segmentY =
        76 + static_cast<int16_t>(sinf(angle) * 67.0f) - 3;
    lv_obj_t *segment = lv_obj_create(healthCore);
    clearObjectStyle(segment);
    lv_obj_set_pos(segment, segmentX, segmentY);
    lv_obj_set_size(segment, 7, 7);
    lv_obj_set_style_bg_color(
        segment,
        color(index < activeSegments ? overallColor() : COLOR_RULE), 0);
    lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(segment, 1, 0);
    if (index < activeSegments) animateReveal(segment, index * 24U);
  }

  lv_obj_t *coreSignal = makeSignal(content, 99, 53, overallColor());
  animateSignal(coreSignal, 0);
  lv_obj_t *stateLabel = makeLabel(content, overallState(), 0, 0,
                                   &lv_font_montserrat_20, overallColor());
  lv_obj_align_to(stateLabel, healthCore, LV_ALIGN_CENTER, 0, -7);
  char hostRatio[20];
  if (snapshot.ready) {
    snprintf(hostRatio, sizeof(hostRatio), "%u / %u HOSTS",
             snapshot.reachableHosts, snapshot.hostCount);
  } else {
    strlcpy(hostRatio, "NO SNAPSHOT", sizeof(hostRatio));
  }
  lv_obj_t *ratioLabel = makeLabel(content, hostRatio, 0, 0,
                                   &lv_font_montserrat_14, COLOR_MUTED);
  lv_obj_align_to(ratioLabel, healthCore, LV_ALIGN_CENTER, 0, 19);

  auto makeTrace = [&](int16_t x, int16_t y, int16_t width, int16_t height) {
    lv_obj_t *trace = lv_obj_create(content);
    clearObjectStyle(trace);
    lv_obj_set_pos(trace, x, y);
    lv_obj_set_size(trace, width, height);
    lv_obj_set_style_bg_color(trace, color(COLOR_RULE), 0);
    lv_obj_set_style_bg_opa(trace, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(trace, 1, 0);
  };

  constexpr int16_t NODE_X = 230;
  constexpr int16_t NODE_START_Y = 34;
  constexpr int16_t NODE_STEP = 37;
  constexpr int16_t NODE_HEIGHT = 31;
  const uint8_t visibleHosts = min<uint8_t>(snapshot.hostCount, MAX_HOSTS);
  if (snapshot.ready && visibleHosts) {
    const int16_t firstCenter = NODE_START_Y + NODE_HEIGHT / 2;
    const int16_t lastCenter = firstCenter + (visibleHosts - 1) * NODE_STEP;
    const int16_t trunkTop = min<int16_t>(firstCenter, 106);
    const int16_t trunkBottom = max<int16_t>(lastCenter, 106);
    makeTrace(177, 105, 32, 2);
    makeTrace(207, trunkTop, 2, trunkBottom - trunkTop + 2);

    for (uint8_t index = 0; index < visibleHosts; ++index) {
      const HostSummary &host = snapshot.hosts[index];
      const int16_t nodeY = NODE_START_Y + index * NODE_STEP;
      const int16_t nodeCenter = nodeY + NODE_HEIGHT / 2;
      makeTrace(208, nodeCenter, NODE_X - 208, 2);

      lv_obj_t *node = makePanel(NODE_X, nodeY, 240, NODE_HEIGHT);
      const uint32_t nodeColor =
          !host.reachable
              ? COLOR_DANGER
              : (host.issues ? COLOR_WARNING : COLOR_OK);
      lv_obj_t *nodeSignal = makeSignal(node, 10, 12, nodeColor);
      animateSignal(nodeSignal, 140U + index * 170U);
      makeLabel(node, host.name, 25, 6, &lv_font_montserrat_16, COLOR_INK);
      char nodeDetail[24];
      if (host.reachable) {
        snprintf(nodeDetail, sizeof(nodeDetail), "%u RUNNING", host.workloads);
      } else {
        strlcpy(nodeDetail, "UNREACHABLE", sizeof(nodeDetail));
      }
      lv_obj_t *detailLabel = makeLabel(node, nodeDetail, 0, 8,
                                        &lv_font_montserrat_14, nodeColor);
      lv_obj_align(detailLabel, LV_ALIGN_TOP_RIGHT, -10, 8);
    }
  } else {
    makeTrace(177, 105, 32, 2);
    lv_obj_t *waiting = makePanel(NODE_X, 88, 240, 42);
    makeSignal(waiting, 12, 17, COLOR_DANGER);
    makeLabel(waiting, "WAITING FOR HOST SNAPSHOT", 29, 11,
              &lv_font_montserrat_14, COLOR_MUTED);
  }

  float peakCpu = -1.0f;
  float hottest = -1.0f;
  uint8_t activeIssues = 0;
  if (snapshot.ready) {
    activeIssues = snapshot.issues + snapshot.hostCount - snapshot.reachableHosts;
  } else {
    ++activeIssues;
  }
  if (metrics.ready) {
    for (uint8_t index = 0; index < metrics.systemCount; ++index) {
      const MetricSystem &system = metrics.systems[index];
      if (system.cpu > peakCpu) peakCpu = system.cpu;
      if (system.temperature > hottest) hottest = system.temperature;
      if (metricIssueColor(system) != 0) ++activeIssues;
    }
  } else {
    ++activeIssues;
  }

  lv_obj_t *rail = makePanel(10, 188, 460, 38);
  constexpr const char *RAIL_LABELS[] = {"HOSTS", "WORKLOADS", "ISSUES",
                                         "PEAK CPU", "HOTTEST"};
  char railValues[5][14];
  if (snapshot.ready) {
    snprintf(railValues[0], sizeof(railValues[0]), "%u/%u",
             snapshot.reachableHosts, snapshot.hostCount);
    snprintf(railValues[1], sizeof(railValues[1]), "%u", snapshot.workloads);
  } else {
    strlcpy(railValues[0], "—", sizeof(railValues[0]));
    strlcpy(railValues[1], "—", sizeof(railValues[1]));
  }
  snprintf(railValues[2], sizeof(railValues[2]), "%u", activeIssues);
  if (peakCpu >= 0.0f) {
    snprintf(railValues[3], sizeof(railValues[3]), "%.0f%%", peakCpu);
  } else {
    strlcpy(railValues[3], "—", sizeof(railValues[3]));
  }
  if (hottest >= 0.0f) {
    snprintf(railValues[4], sizeof(railValues[4]), "%.0f°", hottest);
  } else {
    strlcpy(railValues[4], "—", sizeof(railValues[4]));
  }

  const uint32_t railColors[] = {
      snapshot.ready ? overallColor() : COLOR_MUTED, COLOR_INK,
      activeIssues ? COLOR_WARNING : COLOR_OK, COLOR_ACCENT,
      hottest >= 75.0f ? COLOR_WARNING : COLOR_INK};
  for (uint8_t index = 0; index < 5; ++index) {
    const int16_t x = 8 + index * 90;
    makeLabel(rail, RAIL_LABELS[index], x, 2, &lv_font_montserrat_14,
              COLOR_MUTED);
    makeLabel(rail, railValues[index], x, 19, &lv_font_montserrat_16,
              railColors[index]);
    if (index > 0) {
      lv_obj_t *divider = lv_obj_create(rail);
      clearObjectStyle(divider);
      lv_obj_set_pos(divider, x - 8, 7);
      lv_obj_set_size(divider, 1, 24);
      lv_obj_set_style_bg_color(divider, color(COLOR_RULE), 0);
      lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    }
  }
}

void metricSystemEvent(lv_event_t *event) {
  selectedMetricSystem = static_cast<int8_t>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  renderCurrentPage();
}

void metricBackEvent(lv_event_t *) {
  selectedMetricSystem = -1;
  renderCurrentPage();
}

lv_obj_t *makeMetricCard(const char *label, const char *value, int16_t x,
                         uint32_t valueColor = COLOR_INK) {
  lv_obj_t *card = makePanel(x, 50, 104, 54);
  makeLabel(card, label, 9, 6, &lv_font_montserrat_14, COLOR_MUTED);
  makeLabel(card, value, 9, 26, &lv_font_montserrat_20, valueColor);
  return card;
}

void renderMetricDetail() {
  if (!metrics.ready || selectedMetricSystem < 0 ||
      selectedMetricSystem >= metrics.systemCount) {
    selectedMetricSystem = -1;
    return;
  }

  const MetricSystem &system = metrics.systems[selectedMetricSystem];
  makeLabel(content, system.name, 10, 4, &lv_font_montserrat_20, COLOR_INK);

  char rx[16];
  char tx[16];
  formatRate(rx, sizeof(rx), system.networkRx);
  formatRate(tx, sizeof(tx), system.networkTx);
  char detail[72];
  if (system.load1 >= 0.0f) {
    snprintf(detail, sizeof(detail), "%s · load %.2f · RX %s · TX %s",
             system.reachable ? "UP" : "DOWN", system.load1, rx, tx);
  } else {
    snprintf(detail, sizeof(detail), "%s · RX %s · TX %s",
             system.reachable ? "UP" : "DOWN", rx, tx);
  }
  makeLabel(content, detail, 10, 28, &lv_font_montserrat_14,
            system.reachable ? COLOR_MUTED : COLOR_DANGER);

  lv_obj_t *back = lv_btn_create(content);
  clearObjectStyle(back);
  lv_obj_set_pos(back, 394, 4);
  lv_obj_set_size(back, 76, 34);
  lv_obj_set_style_bg_color(back, color(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_color(back, color(COLOR_SURFACE_RAISED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(back, color(COLOR_RULE), 0);
  lv_obj_set_style_border_width(back, 1, 0);
  lv_obj_set_style_radius(back, 4, 0);
  lv_obj_add_event_cb(back, metricBackEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = makeLabel(back, "BACK", 0, 0, &lv_font_montserrat_14,
                                  COLOR_ACCENT);
  lv_obj_center(backLabel);

  char cpu[12];
  char memory[12];
  char disk[12];
  char temperature[12];
  formatMetric(cpu, sizeof(cpu), system.cpu, "%");
  formatMetric(memory, sizeof(memory), system.memory, "%");
  formatMetric(disk, sizeof(disk), system.disk, "%");
  formatMetric(temperature, sizeof(temperature), system.temperature, "°");
  makeMetricCard("CPU", cpu, 10, COLOR_ACCENT);
  makeMetricCard("MEMORY", memory, 122, COLOR_WARNING);
  makeMetricCard("DISK", disk, 234);
  makeMetricCard("HOTTEST", temperature, 346);

  lv_obj_t *chartPanel = makePanel(10, 112, 460, 114);
  makeLabel(chartPanel, "60 MIN", 10, 7, &lv_font_montserrat_14, COLOR_MUTED);
  makeSignal(chartPanel, 79, 12, COLOR_ACCENT);
  makeLabel(chartPanel, "CPU", 92, 7, &lv_font_montserrat_14, COLOR_MUTED);
  makeSignal(chartPanel, 139, 12, COLOR_WARNING);
  makeLabel(chartPanel, "MEM", 152, 7, &lv_font_montserrat_14, COLOR_MUTED);

  if (system.sampleCount < 2) {
    makeLabel(chartPanel, "History unavailable", 10, 52,
              &lv_font_montserrat_16, COLOR_MUTED);
    return;
  }

  lv_obj_t *chart = lv_chart_create(chartPanel);
  clearObjectStyle(chart);
  lv_obj_set_pos(chart, 10, 30);
  lv_obj_set_size(chart, 438, 72);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_line_color(chart, color(COLOR_RULE), LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_point_count(chart, system.sampleCount);
  lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);

  lv_chart_series_t *cpuSeries =
      lv_chart_add_series(chart, color(COLOR_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_series_t *memorySeries =
      lv_chart_add_series(chart, color(COLOR_WARNING), LV_CHART_AXIS_PRIMARY_Y);
  for (uint8_t index = 0; index < system.sampleCount; ++index) {
    lv_chart_set_next_value(chart, cpuSeries, system.cpuHistory[index]);
    lv_chart_set_next_value(chart, memorySeries, system.memoryHistory[index]);
  }
}

void renderSystems() {
  if (selectedMetricSystem >= 0 && metrics.ready &&
      selectedMetricSystem < metrics.systemCount) {
    renderMetricDetail();
    return;
  }
  selectedMetricSystem = -1;

  makeLabel(content, "SYSTEMS", 10, 4, &lv_font_montserrat_20, COLOR_INK);
  lv_obj_t *statusLabel =
      makeLabel(content, metrics.ready ? "BESZEL · 60 s" : "BESZEL · OFFLINE",
                0, 8, &lv_font_montserrat_14,
                metrics.ready ? COLOR_OK : COLOR_DANGER);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_RIGHT, -10, 8);

  if (!metrics.ready) {
    lv_obj_t *empty = makePanel(10, 48, 460, 150);
    makeLabel(empty, "Metrics snapshot unavailable", 16, 22,
              &lv_font_montserrat_20, COLOR_INK);
    makeLabel(empty, metrics.error, 16, 62, &lv_font_montserrat_14,
              COLOR_MUTED);
    makeLabel(empty, "Docker state remains available on Overview.", 16, 98,
              &lv_font_montserrat_14, COLOR_MUTED);
    return;
  }

  for (uint8_t index = 0; index < metrics.systemCount; ++index) {
    const MetricSystem &system = metrics.systems[index];
    const int16_t column = index % 2;
    const int16_t rowIndex = index / 2;
    lv_obj_t *row = makePanel(10 + column * 234, 42 + rowIndex * 61, 226, 54);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, color(COLOR_SURFACE_RAISED),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, metricSystemEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
    makeSignal(row, 10, 12, system.reachable ? COLOR_OK : COLOR_DANGER);
    makeLabel(row, system.name, 24, 6, &lv_font_montserrat_16, COLOR_INK);

    char cpu[12];
    char memory[12];
    char cpuText[24];
    char memoryText[24];
    formatMetric(cpu, sizeof(cpu), system.cpu, "%");
    formatMetric(memory, sizeof(memory), system.memory, "%");
    snprintf(cpuText, sizeof(cpuText), "CPU %s", cpu);
    snprintf(memoryText, sizeof(memoryText), "MEM %s", memory);
    makeLabel(row, cpuText, 10, 31, &lv_font_montserrat_14, COLOR_ACCENT);
    lv_obj_t *memoryLabel = makeLabel(row, memoryText, 0, 31,
                                      &lv_font_montserrat_14, COLOR_WARNING);
    lv_obj_align(memoryLabel, LV_ALIGN_TOP_RIGHT, -10, 31);
  }
}

void renderIssues() {
  struct ActiveIssue {
    const char *subject;
    char detail[36];
    uint32_t severityColor;
  };

  ActiveIssue active[MAX_DOCKER_ISSUES + MAX_METRIC_SYSTEMS + 2];
  uint8_t activeCount = 0;
  auto addIssue = [&](const char *subject, const char *detail,
                      uint32_t severityColor) {
    if (activeCount >= sizeof(active) / sizeof(active[0])) return;
    active[activeCount].subject = subject;
    strlcpy(active[activeCount].detail, detail,
            sizeof(active[activeCount].detail));
    active[activeCount].severityColor = severityColor;
    ++activeCount;
  };

  if (!snapshot.ready) {
    addIssue("DOCKER FEED", snapshot.error, COLOR_DANGER);
  } else {
    for (uint8_t index = 0; index < snapshot.issueDetailCount; ++index) {
      const DockerIssue &issue = snapshot.issueDetails[index];
      addIssue(issue.subject, issue.detail, issue.severityColor);
    }
    if (feedIsStale(snapshot.ready, snapshot.fetchedAt, REFRESH_INTERVAL_MS))
      addIssue("DOCKER FEED", "Snapshot is stale", COLOR_WARNING);
  }

  if (!metrics.ready) {
    addIssue("BESZEL FEED", metrics.error, COLOR_DANGER);
  } else {
    for (uint8_t index = 0; index < metrics.systemCount; ++index) {
      const MetricSystem &system = metrics.systems[index];
      const uint32_t severityColor = metricIssueColor(system);
      if (severityColor == 0) continue;
      char detail[36];
      formatMetricIssue(detail, sizeof(detail), system);
      addIssue(system.name, detail, severityColor);
    }
    if (feedIsStale(metrics.ready, metrics.fetchedAt,
                    METRICS_REFRESH_INTERVAL_MS))
      addIssue("BESZEL FEED", "Snapshot is stale", COLOR_WARNING);
  }

  uint32_t summaryColor = activeCount ? COLOR_WARNING : COLOR_OK;
  for (uint8_t index = 0; index < activeCount; ++index) {
    if (active[index].severityColor == COLOR_DANGER) {
      summaryColor = COLOR_DANGER;
      break;
    }
  }

  makeLabel(content, "ISSUES", 10, 4, &lv_font_montserrat_20, COLOR_INK);
  char count[20];
  snprintf(count, sizeof(count), "%u ACTIVE", activeCount);
  lv_obj_t *countLabel = makeLabel(content, count, 0, 8,
                                   &lv_font_montserrat_14, summaryColor);
  lv_obj_align(countLabel, LV_ALIGN_TOP_RIGHT, -10, 8);

  if (activeCount == 0) {
    lv_obj_t *nominal = makePanel(10, 48, 460, 150);
    makeSignal(nominal, 16, 25, COLOR_OK);
    makeLabel(nominal, "NO ACTIVE ISSUES", 34, 18,
              &lv_font_montserrat_20, COLOR_OK);
    makeLabel(nominal, "Docker and Beszel are reporting normally.", 16, 61,
              &lv_font_montserrat_16, COLOR_INK);
    char dockerStatus[24];
    char metricsStatus[24];
    formatFeedStatus(dockerStatus, sizeof(dockerStatus), snapshot.ready,
                     snapshot.fetchedAt, REFRESH_INTERVAL_MS);
    formatFeedStatus(metricsStatus, sizeof(metricsStatus), metrics.ready,
                     metrics.fetchedAt, METRICS_REFRESH_INTERVAL_MS);
    char freshness[60];
    snprintf(freshness, sizeof(freshness), "Docker %s · Beszel %s",
             dockerStatus, metricsStatus);
    makeLabel(nominal, freshness, 16, 101, &lv_font_montserrat_14,
              COLOR_MUTED);
    return;
  }

  const uint8_t visibleCount = min<uint8_t>(activeCount, 4);
  for (uint8_t index = 0; index < visibleCount; ++index) {
    lv_obj_t *row = makePanel(10, 42 + index * 43, 460, 38);
    makeSignal(row, 10, 15, active[index].severityColor);
    makeLabel(row, active[index].subject, 24, 10, &lv_font_montserrat_14,
              COLOR_INK);
    lv_obj_t *detail = makeLabel(row, active[index].detail, 0, 10,
                                 &lv_font_montserrat_14,
                                 active[index].severityColor);
    lv_obj_align(detail, LV_ALIGN_TOP_RIGHT, -10, 10);
  }
  if (activeCount > visibleCount) {
    char more[28];
    snprintf(more, sizeof(more), "+ %u additional issues",
             activeCount - visibleCount);
    makeLabel(content, more, 10, 216, &lv_font_montserrat_14, COLOR_MUTED);
  }
}

const char *weatherCondition(int code) {
  if (code == 0) return "Clear";
  if (code == 1) return "Mostly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code == 85 || code == 86) return "Snow showers";
  if (code >= 95) return "Thunderstorms";
  return "Unknown";
}

const char *windCompass(int degrees) {
  static constexpr const char *DIRECTIONS[] = {"N", "NE", "E", "SE",
                                                "S", "SW", "W", "NW"};
  if (degrees < 0) return "—";
  return DIRECTIONS[((degrees + 22) / 45) % 8];
}

int weekdayForDate(const char *isoDate) {
  if (!isoDate || strlen(isoDate) < 10) return -1;
  int year = atoi(isoDate);
  int month = atoi(isoDate + 5);
  const int day = atoi(isoDate + 8);
  static constexpr int OFFSETS[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) --year;
  return (year + year / 4 - year / 100 + year / 400 + OFFSETS[month - 1] +
          day) % 7;
}

const char *weekday(const char *isoDate) {
  static constexpr const char *DAYS[] = {"SUN", "MON", "TUE", "WED",
                                         "THU", "FRI", "SAT"};
  const int index = weekdayForDate(isoDate);
  return index >= 0 ? DAYS[index] : "—";
}

lv_obj_t *makeWeatherPart(lv_obj_t *parent, int16_t x, int16_t y,
                          int16_t width, int16_t height, uint32_t partColor,
                          int16_t radius) {
  lv_obj_t *part = lv_obj_create(parent);
  clearObjectStyle(part);
  lv_obj_set_pos(part, x, y);
  lv_obj_set_size(part, width, height);
  lv_obj_set_style_bg_color(part, color(partColor), 0);
  lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(part, radius, 0);
  return part;
}

void makeSunGlyph(lv_obj_t *parent, int16_t x, int16_t y, uint8_t scale) {
  const int16_t s = scale;
  makeWeatherPart(parent, x + 8 * s, y + 8 * s, 10 * s, 10 * s,
                  COLOR_WARNING, LV_RADIUS_CIRCLE);
  makeWeatherPart(parent, x + 12 * s, y + 1 * s, 2 * s, 5 * s,
                  COLOR_WARNING, s);
  makeWeatherPart(parent, x + 12 * s, y + 20 * s, 2 * s, 5 * s,
                  COLOR_WARNING, s);
  makeWeatherPart(parent, x + 1 * s, y + 12 * s, 5 * s, 2 * s,
                  COLOR_WARNING, s);
  makeWeatherPart(parent, x + 20 * s, y + 12 * s, 5 * s, 2 * s,
                  COLOR_WARNING, s);
  makeWeatherPart(parent, x + 4 * s, y + 4 * s, 3 * s, 3 * s,
                  COLOR_WARNING, LV_RADIUS_CIRCLE);
  makeWeatherPart(parent, x + 19 * s, y + 4 * s, 3 * s, 3 * s,
                  COLOR_WARNING, LV_RADIUS_CIRCLE);
  makeWeatherPart(parent, x + 4 * s, y + 19 * s, 3 * s, 3 * s,
                  COLOR_WARNING, LV_RADIUS_CIRCLE);
  makeWeatherPart(parent, x + 19 * s, y + 19 * s, 3 * s, 3 * s,
                  COLOR_WARNING, LV_RADIUS_CIRCLE);
}

void makeCloudGlyph(lv_obj_t *parent, int16_t x, int16_t y, uint8_t scale) {
  const int16_t s = scale;
  makeWeatherPart(parent, x + 3 * s, y + 8 * s, 10 * s, 10 * s,
                  COLOR_MUTED, LV_RADIUS_CIRCLE);
  makeWeatherPart(parent, x + 9 * s, y + 3 * s, 13 * s, 13 * s,
                  COLOR_INK, LV_RADIUS_CIRCLE);
  makeWeatherPart(parent, x + 17 * s, y + 8 * s, 10 * s, 10 * s,
                  COLOR_MUTED, LV_RADIUS_CIRCLE);
  makeWeatherPart(parent, x + 3 * s, y + 12 * s, 24 * s, 8 * s,
                  COLOR_MUTED, 4 * s);
}

void makeWeatherGlyph(lv_obj_t *parent, int16_t x, int16_t y, int code,
                      uint8_t scale) {
  const int16_t s = scale;
  const bool fog = code == 45 || code == 48;
  const bool drizzle = code >= 51 && code <= 57;
  const bool rain = (code >= 61 && code <= 67) ||
                    (code >= 80 && code <= 82);
  const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  const bool thunder = code >= 95;

  if (code == 0) {
    makeSunGlyph(parent, x, y, scale);
    return;
  }
  if (code == 1 || code == 2) {
    makeSunGlyph(parent, x, y, scale);
    makeCloudGlyph(parent, x + 6 * s, y + 7 * s, scale);
    return;
  }

  makeCloudGlyph(parent, x, y, scale);
  if (fog) {
    makeWeatherPart(parent, x + 4 * s, y + 23 * s, 22 * s, 2 * s,
                    COLOR_MUTED, s);
    makeWeatherPart(parent, x + 8 * s, y + 27 * s, 18 * s, 2 * s,
                    COLOR_MUTED, s);
  } else if (drizzle || rain) {
    const int16_t dropHeight = (rain ? 7 : 4) * s;
    makeWeatherPart(parent, x + 6 * s, y + 23 * s, 2 * s, dropHeight,
                    COLOR_ACCENT, s);
    makeWeatherPart(parent, x + 14 * s, y + 22 * s, 2 * s, dropHeight,
                    COLOR_ACCENT, s);
    makeWeatherPart(parent, x + 22 * s, y + 23 * s, 2 * s, dropHeight,
                    COLOR_ACCENT, s);
  } else if (snow) {
    makeWeatherPart(parent, x + 5 * s, y + 23 * s, 4 * s, 4 * s,
                    COLOR_ACCENT, LV_RADIUS_CIRCLE);
    makeWeatherPart(parent, x + 13 * s, y + 27 * s, 4 * s, 4 * s,
                    COLOR_INK, LV_RADIUS_CIRCLE);
    makeWeatherPart(parent, x + 21 * s, y + 23 * s, 4 * s, 4 * s,
                    COLOR_ACCENT, LV_RADIUS_CIRCLE);
  } else if (thunder) {
    makeWeatherPart(parent, x + 14 * s, y + 20 * s, 5 * s, 6 * s,
                    COLOR_WARNING, s);
    makeWeatherPart(parent, x + 10 * s, y + 25 * s, 6 * s, 5 * s,
                    COLOR_WARNING, s);
    makeWeatherPart(parent, x + 9 * s, y + 29 * s, 4 * s, 5 * s,
                    COLOR_WARNING, s);
  }
}

void renderDevice() {
  makeLabel(content, "DEVICE", 10, 10, &lv_font_montserrat_20, COLOR_INK);
  makeLabel(content, "Panel hardware and runtime state", 10, 38,
            &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *hardware = makePanel(10, 70, 278, 140);
  makeLabel(hardware, "HARDWARE", 12, 9, &lv_font_montserrat_14, COLOR_MUTED);
  makeLabel(hardware, "ESP32-WROOM-32E", 12, 34, &lv_font_montserrat_20,
            COLOR_INK);
  makeLabel(hardware, "ST7796S · 480 × 320", 12, 76,
            &lv_font_montserrat_14, COLOR_ACCENT);
  makeLabel(hardware, "LVGL 8 · No PSRAM", 12, 105,
            &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *runtime = makePanel(298, 70, 172, 140);
  makeLabel(runtime, "RUNTIME", 12, 9, &lv_font_montserrat_14, COLOR_MUTED);
  char uptime[24];
  snprintf(uptime, sizeof(uptime), "%lu min",
           static_cast<unsigned long>(millis() / 60000UL));
  makeLabel(runtime, uptime, 12, 34, &lv_font_montserrat_24, COLOR_INK);
  char heap[28];
  snprintf(heap, sizeof(heap), "%u KB heap",
           static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  makeLabel(runtime, heap, 12, 78, &lv_font_montserrat_14, COLOR_MUTED);
  makeLabel(runtime, "Read-only client", 12, 105, &lv_font_montserrat_14,
            COLOR_MUTED);
}

void renderWeather() {
  makeLabel(content, "WEATHER", 10, 4, &lv_font_montserrat_20, COLOR_INK);
  const char *statusText = weather.loading
                               ? "UPDATING"
                               : (!weather.ready
                                      ? "OFFLINE"
                                      : (weather.error[0] ? "STALE" : "15 MIN"));
  const uint32_t statusColor = !weather.ready
                                   ? COLOR_DANGER
                                   : (weather.error[0] ? COLOR_WARNING : COLOR_OK);
  lv_obj_t *status = makeLabel(content, statusText, 0, 8,
                               &lv_font_montserrat_14, statusColor);
  lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -10, 8);

  if (!weather.ready) {
    lv_obj_t *empty = makePanel(10, 48, 460, 150);
    makeLabel(empty, "Weather unavailable", 16, 22, &lv_font_montserrat_20,
              COLOR_INK);
    makeLabel(empty, weather.error, 16, 62, &lv_font_montserrat_14,
              COLOR_MUTED);
    makeLabel(empty, "Open-Meteo retries every 60 seconds.", 16, 98,
              &lv_font_montserrat_14, COLOR_MUTED);
    return;
  }

  lv_obj_t *now = makePanel(10, 40, 190, 102);
  makeLabel(now, "BILLERICA · NOW", 12, 8, &lv_font_montserrat_14,
            COLOR_MUTED);
  char temperature[16];
  snprintf(temperature, sizeof(temperature), "%.0f°F", weather.temperature);
  makeLabel(now, temperature, 12, 31, &lv_font_montserrat_28, COLOR_ACCENT);
  makeLabel(now, weatherCondition(weather.code), 12, 73,
            &lv_font_montserrat_14, COLOR_INK);
  makeWeatherGlyph(now, 122, 30, weather.code, 2);

  lv_obj_t *conditions = makePanel(210, 40, 260, 102);
  makeLabel(conditions, "CURRENT CONDITIONS", 12, 8, &lv_font_montserrat_14,
            COLOR_MUTED);
  char detail[64];
  snprintf(detail, sizeof(detail), "Feels %.0f°F · Humidity %.0f%%",
           weather.apparent, weather.humidity);
  makeLabel(conditions, detail, 12, 31, &lv_font_montserrat_16, COLOR_INK);
  snprintf(detail, sizeof(detail), "Wind %.0f mph %s · Gust %.0f mph",
           weather.wind, windCompass(weather.windDirection), weather.gust);
  makeLabel(conditions, detail, 12, 57, &lv_font_montserrat_14, COLOR_MUTED);
  snprintf(detail, sizeof(detail), "Pressure %.0f hPa · Observed %.5s",
           weather.pressure,
           strlen(weather.observedAt) >= 16 ? weather.observedAt + 11 : "—");
  makeLabel(conditions, detail, 12, 79, &lv_font_montserrat_14, COLOR_MUTED);

  for (uint8_t index = 0; index < FORECAST_DAYS; ++index) {
    const DayForecast &day = weather.days[index];
    lv_obj_t *forecast = makePanel(10 + index * 92, 150, 84, 76);
    makeLabel(forecast, index == 0 ? "TODAY" : weekday(day.date), 8, 6,
              &lv_font_montserrat_14, COLOR_MUTED);
    makeWeatherGlyph(forecast, 50, 3, day.code, 1);
    char highLow[20];
    snprintf(highLow, sizeof(highLow), "%.0f / %.0f", day.high, day.low);
    makeLabel(forecast, highLow, 8, 35, &lv_font_montserrat_16, COLOR_INK);
    char rain[20];
    snprintf(rain, sizeof(rain), "RAIN %d%%", day.precipitation);
    makeLabel(forecast, rain, 8, 56, &lv_font_montserrat_14,
              day.precipitation >= 40 ? COLOR_ACCENT : COLOR_MUTED);
  }
}

void renderCurrentPage() {
  lv_obj_clean(content);
  switch (currentPage) {
    case Page::OVERVIEW: renderOverview(); break;
    case Page::SYSTEMS: renderSystems(); break;
    case Page::ISSUES: renderIssues(); break;
    case Page::DEVICE: renderDevice(); break;
    case Page::WEATHER: renderWeather(); break;
  }
  updateNavigation();
}

void navigationEvent(lv_event_t *event) {
  const Page destination = static_cast<Page>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (destination != Page::SYSTEMS || currentPage == Page::SYSTEMS) {
    selectedMetricSystem = -1;
  }
  currentPage = destination;
  renderCurrentPage();
}

void buildUi() {
  lv_obj_t *root = lv_scr_act();
  clearObjectStyle(root);
  lv_obj_set_style_bg_color(root, color(COLOR_PAPER), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  lv_obj_t *header = lv_obj_create(root);
  clearObjectStyle(header);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_size(header, SCREEN_WIDTH, HEADER_HEIGHT);
  lv_obj_set_style_bg_color(header, color(COLOR_PAPER), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(header, color(COLOR_RULE), 0);
  lv_obj_set_style_border_width(header, 1, 0);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
  makeLabel(header, "HALF.BLACK / HOMELAB", 10, 11, &lv_font_montserrat_16,
            COLOR_INK);

  networkLabel = makeLabel(header, "WIFI", 0, 13, &lv_font_montserrat_14,
                           COLOR_OK);
  lv_obj_align(networkLabel, LV_ALIGN_TOP_RIGHT, -10, 13);

  content = lv_obj_create(root);
  clearObjectStyle(content);
  lv_obj_set_pos(content, 0, HEADER_HEIGHT);
  lv_obj_set_size(content, SCREEN_WIDTH, CONTENT_HEIGHT);
  lv_obj_set_style_bg_color(content, color(COLOR_PAPER), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

  lv_obj_t *navigation = lv_obj_create(root);
  clearObjectStyle(navigation);
  lv_obj_set_pos(navigation, 0, SCREEN_HEIGHT - NAV_HEIGHT);
  lv_obj_set_size(navigation, SCREEN_WIDTH, NAV_HEIGHT);
  lv_obj_set_style_bg_color(navigation, color(COLOR_SURFACE_RAISED), 0);
  lv_obj_set_style_bg_opa(navigation, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(navigation, color(COLOR_RULE), 0);
  lv_obj_set_style_border_width(navigation, 1, 0);
  lv_obj_set_style_border_side(navigation, LV_BORDER_SIDE_TOP, 0);

  constexpr const char *labels[] = {"OVERVIEW", "SYSTEMS", "ISSUES", "DEVICE",
                                    "WEATHER"};
  for (uint8_t index = 0; index < 5; ++index) {
    lv_obj_t *button = lv_btn_create(navigation);
    clearObjectStyle(button);
    lv_obj_set_pos(button, index * 96, 0);
    lv_obj_set_size(button, 96, NAV_HEIGHT);
    lv_obj_set_style_bg_color(button, color(COLOR_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(button, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(button, color(COLOR_MUTED), 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, navigationEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
    lv_obj_t *label = lv_label_create(button);
    clearObjectStyle(label);
    lv_label_set_text(label, labels[index]);
    lv_obj_center(label);
    navButtons[index] = button;
  }

  renderCurrentPage();
}

bool loadSnapshot() {
  snapshot.loading = true;
  snapshot.error[0] = '\0';

  if (!WiFi.isConnected()) {
    snapshot.ready = false;
    snapshot.loading = false;
    strlcpy(snapshot.error, "Wi-Fi is disconnected", sizeof(snapshot.error));
    return false;
  }

  // LAN-only read endpoint. No credential crosses this connection. The future
  // control API will use authenticated transport and a narrowly scoped token.
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  if (!http.begin(client, SNAPSHOT_URL)) {
    snapshot.ready = false;
    snapshot.loading = false;
    strlcpy(snapshot.error, "Could not initialize the snapshot request",
            sizeof(snapshot.error));
    return false;
  }

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    snapshot.ready = false;
    snapshot.loading = false;
    snprintf(snapshot.error, sizeof(snapshot.error),
             "Snapshot returned HTTP %d", status);
    Serial.printf("Snapshot request failed: HTTP %d, %s\n", status,
                  http.errorToString(status).c_str());
    http.end();
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) {
    snapshot.ready = false;
    snapshot.loading = false;
    snprintf(snapshot.error, sizeof(snapshot.error), "Snapshot JSON: %s",
             error.c_str());
    return false;
  }

  Snapshot next;
  next.loading = false;
  next.ready = true;
  next.error[0] = '\0';
  next.fetchedAt = millis();
  strlcpy(next.generatedAt, document["generatedAt"] | "—",
          sizeof(next.generatedAt));

  for (JsonObject hostObject : document["hosts"].as<JsonArray>()) {
    if (next.hostCount >= MAX_HOSTS) break;
    HostSummary &host = next.hosts[next.hostCount++];
    strlcpy(host.name, hostObject["name"] | "unknown", sizeof(host.name));
    host.reachable = strcmp(hostObject["status"] | "unreachable", "up") == 0;
    if (host.reachable) {
      ++next.reachableHosts;
    } else if (next.issueDetailCount < MAX_DOCKER_ISSUES) {
      DockerIssue &issue = next.issueDetails[next.issueDetailCount++];
      strlcpy(issue.subject, host.name, sizeof(issue.subject));
      strlcpy(issue.detail, "Host unreachable", sizeof(issue.detail));
      issue.severityColor = COLOR_DANGER;
    }

    for (JsonObject workload : hostObject["containers"].as<JsonArray>()) {
      ++host.workloads;
      ++next.workloads;
      const char *state = workload["state"] | "unknown";
      const char *health = workload["health"] | "";
      const bool issue = strcmp(state, "running") != 0 ||
                         strcmp(health, "unhealthy") == 0;
      if (issue) {
        ++host.issues;
        ++next.issues;
        if (next.issueDetailCount < MAX_DOCKER_ISSUES) {
          DockerIssue &detail = next.issueDetails[next.issueDetailCount++];
          strlcpy(detail.subject, workload["name"] | "container",
                  sizeof(detail.subject));
          snprintf(detail.detail, sizeof(detail.detail), "%s · %s", host.name,
                   strcmp(state, "running") != 0 ? state : "unhealthy");
          detail.severityColor = COLOR_DANGER;
        }
      }
    }
  }

  snapshot = next;
  return true;
}

bool loadMetricsSnapshot() {
  metrics.loading = true;
  metrics.error[0] = '\0';

  if (!WiFi.isConnected()) {
    metrics.loading = false;
    strlcpy(metrics.error, "Wi-Fi is disconnected", sizeof(metrics.error));
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  if (!http.begin(client, METRICS_URL)) {
    metrics.loading = false;
    strlcpy(metrics.error, "Could not initialize the metrics request",
            sizeof(metrics.error));
    return false;
  }

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    metrics.loading = false;
    snprintf(metrics.error, sizeof(metrics.error), "Metrics returned HTTP %d",
             status);
    Serial.printf("Metrics request failed: HTTP %d, %s\n", status,
                  http.errorToString(status).c_str());
    http.end();
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) {
    metrics.loading = false;
    snprintf(metrics.error, sizeof(metrics.error), "Metrics JSON: %s",
             error.c_str());
    return false;
  }

  MetricsSnapshot next;
  next.loading = false;
  next.ready = true;
  next.error[0] = '\0';
  next.fetchedAt = millis();
  strlcpy(next.generatedAt, document["generatedAt"] | "—",
          sizeof(next.generatedAt));

  for (JsonObject systemObject : document["systems"].as<JsonArray>()) {
    if (next.systemCount >= MAX_METRIC_SYSTEMS) break;
    MetricSystem &system = next.systems[next.systemCount++];
    strlcpy(system.name, systemObject["name"] | "unknown", sizeof(system.name));
    strlcpy(system.updatedAt, systemObject["updatedAt"] | "—",
            sizeof(system.updatedAt));
    system.reachable = strcmp(systemObject["status"] | "down", "up") == 0;
    system.cpu = systemObject["cpu"] | -1.0f;
    system.memory = systemObject["memory"] | -1.0f;
    system.disk = systemObject["disk"] | -1.0f;
    system.temperature = systemObject["temperature"] | -1.0f;
    system.load1 = systemObject["load1"] | -1.0f;
    system.networkRx = systemObject["networkRx"] | -1.0f;
    system.networkTx = systemObject["networkTx"] | -1.0f;

    JsonArray cpuHistory = systemObject["history"]["cpu"].as<JsonArray>();
    JsonArray memoryHistory =
        systemObject["history"]["memory"].as<JsonArray>();
    system.sampleCount = min<uint8_t>(
        METRIC_HISTORY_POINTS,
        max(static_cast<uint8_t>(cpuHistory.size()),
            static_cast<uint8_t>(memoryHistory.size())));
    for (uint8_t index = 0; index < system.sampleCount; ++index) {
      JsonVariant cpuPoint = cpuHistory[index];
      JsonVariant memoryPoint = memoryHistory[index];
      system.cpuHistory[index] =
          cpuPoint.isNull()
              ? LV_CHART_POINT_NONE
              : constrain(static_cast<int>(lroundf(cpuPoint.as<float>())), 0, 100);
      system.memoryHistory[index] =
          memoryPoint.isNull()
              ? LV_CHART_POINT_NONE
              : constrain(static_cast<int>(lroundf(memoryPoint.as<float>())), 0,
                          100);
    }
  }

  if (next.systemCount == 0) {
    metrics.loading = false;
    strlcpy(metrics.error, "Metrics snapshot contains no systems",
            sizeof(metrics.error));
    return false;
  }

  metrics = next;
  return true;
}

bool readWeatherNumber(JsonVariant value, float &destination) {
  if (value.isNull()) return false;
  destination = value.as<float>();
  return !isnan(destination);
}

bool loadWeather() {
  weather.loading = true;
  weather.error[0] = '\0';

  if (!WiFi.isConnected()) {
    weather.loading = false;
    strlcpy(weather.error, "Wi-Fi is disconnected", sizeof(weather.error));
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(12000);
  http.useHTTP10(true);
  if (!http.begin(client, WEATHER_URL)) {
    weather.loading = false;
    strlcpy(weather.error, "Could not initialize weather request",
            sizeof(weather.error));
    return false;
  }
  http.addHeader("User-Agent", "HomelabPanel/1.0");

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    weather.loading = false;
    snprintf(weather.error, sizeof(weather.error), "Weather returned HTTP %d",
             status);
    Serial.printf("Weather request failed: HTTP %d, %s\n", status,
                  http.errorToString(status).c_str());
    http.end();
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) {
    weather.loading = false;
    snprintf(weather.error, sizeof(weather.error), "Weather JSON: %s",
             error.c_str());
    return false;
  }

  WeatherSnapshot next;
  JsonObject current = document["current"];
  if (current.isNull() ||
      !readWeatherNumber(current["temperature_2m"], next.temperature) ||
      !readWeatherNumber(current["apparent_temperature"], next.apparent) ||
      !readWeatherNumber(current["relative_humidity_2m"], next.humidity) ||
      !readWeatherNumber(current["wind_speed_10m"], next.wind) ||
      !readWeatherNumber(current["wind_gusts_10m"], next.gust) ||
      !readWeatherNumber(current["pressure_msl"], next.pressure)) {
    weather.loading = false;
    strlcpy(weather.error, "Weather response omitted current conditions",
            sizeof(weather.error));
    return false;
  }

  strlcpy(next.observedAt, current["time"] | "", sizeof(next.observedAt));
  next.code = current["weather_code"] | -1;
  next.windDirection = current["wind_direction_10m"] | -1;

  JsonObject daily = document["daily"];
  JsonArray dates = daily["time"];
  JsonArray codes = daily["weather_code"];
  JsonArray highs = daily["temperature_2m_max"];
  JsonArray lows = daily["temperature_2m_min"];
  JsonArray rain = daily["precipitation_probability_max"];
  if (dates.size() < FORECAST_DAYS || codes.size() < FORECAST_DAYS ||
      highs.size() < FORECAST_DAYS || lows.size() < FORECAST_DAYS ||
      rain.size() < FORECAST_DAYS) {
    weather.loading = false;
    strlcpy(weather.error, "Weather response omitted the five-day forecast",
            sizeof(weather.error));
    return false;
  }

  for (uint8_t index = 0; index < FORECAST_DAYS; ++index) {
    DayForecast &day = next.days[index];
    strlcpy(day.date, dates[index] | "", sizeof(day.date));
    day.code = codes[index] | -1;
    day.high = highs[index] | NAN;
    day.low = lows[index] | NAN;
    day.precipitation = rain[index] | -1;
  }

  next.ready = true;
  next.loading = false;
  next.error[0] = '\0';
  next.fetchedAt = millis();
  weather = next;
  Serial.printf("Weather updated: %.1f F, %s\n", weather.temperature,
                weatherCondition(weather.code));
  return true;
}

String randomHex(size_t length) {
  constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  String value;
  value.reserve(length);
  uint32_t randomValue = 0;
  for (size_t index = 0; index < length; ++index) {
    if (index % 8 == 0) randomValue = esp_random();
    value += HEX_DIGITS[randomValue & 0x0F];
    randomValue >>= 4;
  }
  return value;
}

void drawProvisioning(const String &accessPoint, const String &password) {
  display.fillScreen(color565(COLOR_PAPER));
  display.setTextDatum(TL_DATUM);
  display.setTextColor(color565(COLOR_INK), color565(COLOR_PAPER));
  display.setTextFont(4);
  display.drawString("CONNECT THIS PANEL", 18, 20);

  display.setTextFont(2);
  display.setTextColor(color565(COLOR_MUTED), color565(COLOR_PAPER));
  display.drawString("Join the temporary Wi-Fi network, then select your LAN.",
                     18, 66);

  display.fillRect(18, 104, 444, 118, color565(COLOR_SURFACE));
  display.drawRect(18, 104, 444, 118, color565(COLOR_RULE));
  display.setTextColor(color565(COLOR_MUTED), color565(COLOR_SURFACE));
  display.drawString("NETWORK", 34, 121);
  display.setTextFont(4);
  display.setTextColor(color565(COLOR_ACCENT), color565(COLOR_SURFACE));
  display.drawString(accessPoint, 34, 145);
  display.setTextFont(2);
  display.setTextColor(color565(COLOR_MUTED), color565(COLOR_SURFACE));
  display.drawString("PASSWORD", 34, 184);
  display.setTextColor(color565(COLOR_INK), color565(COLOR_SURFACE));
  display.drawString(password, 124, 184);

  display.setTextColor(color565(COLOR_MUTED), color565(COLOR_PAPER));
  display.drawString("Portal: 192.168.4.1", 18, 250);
  display.drawString("Waiting for Wi-Fi configuration…", 18, 282);
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  const String accessPoint = "HomelabPanel-" + randomHex(4);
  const String password = "panel-" + randomHex(12);
  drawProvisioning(accessPoint, password);

  WiFiManager manager;
  manager.setConnectTimeout(20);
  manager.setConfigPortalBlocking(true);
  manager.setConfigPortalTimeout(0);
  manager.setHostname("homelab-panel");

  if (!manager.autoConnect(accessPoint.c_str(), password.c_str())) {
    delay(1000);
    ESP.restart();
  }

  // USB-powered panel: favor predictable request latency over modem sleep.
  WiFi.setSleep(false);
}

void initializeLvgl() {
  lv_init();
  lv_disp_draw_buf_init(&drawBuffer, drawPixels, nullptr,
                        SCREEN_WIDTH * DRAW_BUFFER_LINES);

  static lv_disp_drv_t displayDriver;
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_WIDTH;
  displayDriver.ver_res = SCREEN_HEIGHT;
  displayDriver.flush_cb = flushDisplay;
  displayDriver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&displayDriver);

  static lv_indev_drv_t touchDriver;
  lv_indev_drv_init(&touchDriver);
  touchDriver.type = LV_INDEV_TYPE_POINTER;
  touchDriver.read_cb = readTouch;
  lv_indev_drv_register(&touchDriver);
}

void handleIdleDisplay() {
  const uint32_t idleFor = millis() - lastInteractionAt;
  if (!screenOff && idleFor >= SCREEN_OFF_AFTER_MS) {
    setBacklight(0);
    screenOff = true;
  } else if (!screenOff && idleFor >= DIM_AFTER_MS) {
    setBacklight(BACKLIGHT_DIMMED);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  pinMode(PIN_TOUCH_IRQ, INPUT);
  ledcSetup(BACKLIGHT_CHANNEL, BACKLIGHT_FREQUENCY_HZ,
            BACKLIGHT_RESOLUTION_BITS);
  ledcAttachPin(PIN_BACKLIGHT, BACKLIGHT_CHANNEL);
  setBacklight(BACKLIGHT_ACTIVE);

  display.init();
  display.setRotation(1);
  display.setSwapBytes(true);

  connectWifi();
  initializeLvgl();
  buildUi();
  lv_timer_handler();

  loadSnapshot();
  lastRefreshAt = millis();
  loadMetricsSnapshot();
  lastMetricsRefreshAt = millis();
  loadWeather();
  lastWeatherAttemptAt = millis();
  renderCurrentPage();
  lv_label_set_text(networkLabel, WiFi.isConnected() ? "WIFI" : "OFFLINE");
  lv_obj_set_style_text_color(networkLabel,
                              color(WiFi.isConnected() ? COLOR_OK : COLOR_DANGER),
                              0);

  lastInteractionAt = millis();
  Serial.printf("Homelab Panel ready: %s / %s · Docker %u hosts · Beszel %u systems\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                snapshot.hostCount, metrics.systemCount);
}

void loop() {
  lv_timer_handler();

  bool refreshed = false;
  if (millis() - lastRefreshAt >= REFRESH_INTERVAL_MS) {
    loadSnapshot();
    lastRefreshAt = millis();
    refreshed = true;
  }
  if (millis() - lastMetricsRefreshAt >= METRICS_REFRESH_INTERVAL_MS) {
    loadMetricsSnapshot();
    lastMetricsRefreshAt = millis();
    refreshed = true;
  }
  const uint32_t weatherInterval =
      weather.error[0] ? WEATHER_RETRY_INTERVAL_MS : WEATHER_REFRESH_INTERVAL_MS;
  if (millis() - lastWeatherAttemptAt >= weatherInterval) {
    loadWeather();
    lastWeatherAttemptAt = millis();
    refreshed = true;
  }
  if (refreshed) renderCurrentPage();

  handleIdleDisplay();
  delay(5);
}
