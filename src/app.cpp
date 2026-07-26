/* Hallmark · pre-emit critique: P5 H5 E4 S5 R5 V4
 * genre: modern-minimal · macrostructure: Workbench
 * theme: Cobalt-dark · enrichment: live metrics · navigation: bottom instrument rail
 * audience: homelab operators · use: health and guarded recovery · tone: technical
 * Hardware constraint: LVGL built-in Montserrat avoids external font assets on 4 MB/no-PSRAM.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
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
constexpr uint32_t REFRESH_INTERVAL_MS = 30000;
constexpr uint32_t METRICS_REFRESH_INTERVAL_MS = 60000;
constexpr uint8_t MAX_HOSTS = 4;
constexpr uint8_t MAX_METRIC_SYSTEMS = 6;
constexpr uint8_t METRIC_HISTORY_POINTS = 60;
constexpr uint16_t DRAW_BUFFER_LINES = 20;

struct HostSummary {
  char name[20] = "—";
  bool reachable = false;
  uint8_t workloads = 0;
  uint8_t issues = 0;
};

struct Snapshot {
  bool ready = false;
  bool loading = true;
  char error[72] = "Waiting for the first snapshot";
  char generatedAt[32] = "—";
  HostSummary hosts[MAX_HOSTS];
  uint8_t hostCount = 0;
  uint8_t reachableHosts = 0;
  uint8_t workloads = 0;
  uint8_t issues = 0;
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

enum class Page : uint8_t { OVERVIEW, SYSTEMS, ACTIONS, DEVICE };

TFT_eSPI display;
lv_disp_draw_buf_t drawBuffer;
lv_color_t drawPixels[SCREEN_WIDTH * DRAW_BUFFER_LINES];
lv_obj_t *content = nullptr;
lv_obj_t *networkLabel = nullptr;
lv_obj_t *navButtons[4] = {};
Snapshot snapshot;
MetricsSnapshot metrics;
Page currentPage = Page::OVERVIEW;
int8_t selectedMetricSystem = -1;
uint32_t lastInteractionAt = 0;
uint32_t lastRefreshAt = 0;
uint32_t lastMetricsRefreshAt = 0;
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

void makeSignal(lv_obj_t *parent, int16_t x, int16_t y, uint32_t signalColor) {
  lv_obj_t *signal = lv_obj_create(parent);
  clearObjectStyle(signal);
  lv_obj_set_pos(signal, x, y);
  lv_obj_set_size(signal, 7, 7);
  lv_obj_set_style_bg_color(signal, color(signalColor), 0);
  lv_obj_set_style_bg_opa(signal, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(signal, 1, 0);
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

bool metricsHaveIssue() {
  if (!metrics.ready) return false;
  for (uint8_t index = 0; index < metrics.systemCount; ++index) {
    if (!metrics.systems[index].reachable) return true;
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
  for (uint8_t index = 0; index < 4; ++index) {
    const bool active = index == static_cast<uint8_t>(currentPage);
    lv_obj_set_style_text_color(navButtons[index],
                                color(active ? COLOR_ACCENT : COLOR_MUTED), 0);
    lv_obj_set_style_border_color(navButtons[index], color(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(navButtons[index], active ? 2 : 0, 0);
    lv_obj_set_style_border_side(navButtons[index], LV_BORDER_SIDE_BOTTOM, 0);
  }
}

void renderOverview() {
  lv_obj_t *statePanel = makePanel(10, 8, 278, 100);
  makeSignal(statePanel, 12, 13, overallColor());
  makeLabel(statePanel, "HOMELAB STATE", 28, 8, &lv_font_montserrat_14,
            COLOR_MUTED);
  makeLabel(statePanel, overallState(), 12, 31, &lv_font_montserrat_28,
            overallColor());

  char detail[52];
  if (snapshot.ready) {
    snprintf(detail, sizeof(detail), "%u/%u hosts · %u workloads",
             snapshot.reachableHosts, snapshot.hostCount, snapshot.workloads);
  } else {
    strlcpy(detail, snapshot.loading ? "Loading snapshot…" : "Snapshot unavailable",
            sizeof(detail));
  }
  makeLabel(statePanel, detail, 12, 73, &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *snapshotPanel = makePanel(298, 8, 172, 100);
  makeLabel(snapshotPanel, "SNAPSHOT", 12, 8, &lv_font_montserrat_14,
            COLOR_MUTED);
  makeLabel(snapshotPanel, snapshot.ready ? "LIVE" : "—", 12, 31,
            &lv_font_montserrat_24,
            snapshot.ready ? COLOR_ACCENT : COLOR_MUTED);
  makeLabel(snapshotPanel, snapshot.ready ? "30 s refresh" : "No data", 12, 72,
            &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *hostPanel = makePanel(10, 118, 278, 104);
  makeLabel(hostPanel, "HOST LEDGER", 12, 7, &lv_font_montserrat_14,
            COLOR_MUTED);
  if (!snapshot.ready) {
    makeLabel(hostPanel, "No host snapshot received.", 12, 43,
              &lv_font_montserrat_16, COLOR_INK);
  } else {
    for (uint8_t index = 0; index < snapshot.hostCount && index < 3; ++index) {
      const HostSummary &host = snapshot.hosts[index];
      const int16_t rowY = 30 + index * 23;
      makeSignal(hostPanel, 12, rowY + 5,
                 host.reachable ? (host.issues ? COLOR_WARNING : COLOR_OK)
                                : COLOR_DANGER);
      makeLabel(hostPanel, host.name, 29, rowY, &lv_font_montserrat_14,
                COLOR_INK);
      char count[20];
      snprintf(count, sizeof(count), "%u running", host.workloads);
      lv_obj_t *countLabel = makeLabel(hostPanel, count, 0, rowY,
                                       &lv_font_montserrat_14, COLOR_MUTED);
      lv_obj_align(countLabel, LV_ALIGN_TOP_RIGHT, -12, rowY);
    }
  }

  lv_obj_t *workloadPanel = makePanel(298, 118, 172, 104);
  makeLabel(workloadPanel, "WORKLOADS", 12, 7, &lv_font_montserrat_14,
            COLOR_MUTED);
  char total[12];
  snprintf(total, sizeof(total), "%u", snapshot.workloads);
  makeLabel(workloadPanel, snapshot.ready ? total : "—", 12, 31,
            &lv_font_montserrat_28, COLOR_INK);
  char issueText[28];
  snprintf(issueText, sizeof(issueText), "%u require attention", snapshot.issues);
  makeLabel(workloadPanel, snapshot.ready ? issueText : "Waiting for data", 12, 76,
            &lv_font_montserrat_14,
            snapshot.issues ? COLOR_WARNING : COLOR_MUTED);
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

void renderActions() {
  makeLabel(content, "ACTIONS", 10, 10, &lv_font_montserrat_20, COLOR_INK);
  makeLabel(content, "Control remains deliberately unavailable", 10, 38,
            &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *boundary = makePanel(10, 72, 460, 128);
  makeSignal(boundary, 16, 21, COLOR_WARNING);
  makeLabel(boundary, "READ-ONLY BOUNDARY", 34, 15,
            &lv_font_montserrat_16, COLOR_WARNING);
  makeLabel(boundary, "No Docker, Proxmox, UniFi, or NAS credentials are stored",
            16, 51, &lv_font_montserrat_14, COLOR_INK);
  makeLabel(boundary, "on this panel. Recovery actions require a separate allow-list",
            16, 74, &lv_font_montserrat_14, COLOR_INK);
  makeLabel(boundary, "and a hold-to-confirm interaction.", 16, 97,
            &lv_font_montserrat_14, COLOR_MUTED);
}

void renderDevice() {
  makeLabel(content, "DEVICE", 10, 10, &lv_font_montserrat_20, COLOR_INK);
  makeLabel(content, "Panel network and runtime state", 10, 38,
            &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *network = makePanel(10, 70, 278, 140);
  makeLabel(network, "NETWORK", 12, 9, &lv_font_montserrat_14, COLOR_MUTED);
  makeLabel(network, WiFi.isConnected() ? WiFi.SSID().c_str() : "Disconnected", 12,
            34, &lv_font_montserrat_20,
            WiFi.isConnected() ? COLOR_INK : COLOR_DANGER);
  makeLabel(network, WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "—",
            12, 70, &lv_font_montserrat_16, COLOR_ACCENT);
  char rssi[28];
  snprintf(rssi, sizeof(rssi), "Signal %d dBm", WiFi.RSSI());
  makeLabel(network, WiFi.isConnected() ? rssi : "Signal —", 12, 105,
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
  makeLabel(runtime, "No PSRAM", 12, 105, &lv_font_montserrat_14, COLOR_MUTED);
}

void renderCurrentPage() {
  lv_obj_clean(content);
  switch (currentPage) {
    case Page::OVERVIEW: renderOverview(); break;
    case Page::SYSTEMS: renderSystems(); break;
    case Page::ACTIONS: renderActions(); break;
    case Page::DEVICE: renderDevice(); break;
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

  constexpr const char *labels[] = {"OVERVIEW", "SYSTEMS", "ACTIONS", "DEVICE"};
  for (uint8_t index = 0; index < 4; ++index) {
    lv_obj_t *button = lv_btn_create(navigation);
    clearObjectStyle(button);
    lv_obj_set_pos(button, index * 120, 0);
    lv_obj_set_size(button, 120, NAV_HEIGHT);
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
  next.fetchedAt = millis();
  strlcpy(next.generatedAt, document["generatedAt"] | "—",
          sizeof(next.generatedAt));

  for (JsonObject hostObject : document["hosts"].as<JsonArray>()) {
    if (next.hostCount >= MAX_HOSTS) break;
    HostSummary &host = next.hosts[next.hostCount++];
    strlcpy(host.name, hostObject["name"] | "unknown", sizeof(host.name));
    host.reachable = strcmp(hostObject["status"] | "unreachable", "up") == 0;
    if (host.reachable) ++next.reachableHosts;

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
  if (refreshed) renderCurrentPage();

  handleIdleDisplay();
  delay(5);
}
