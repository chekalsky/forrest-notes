#include "Arduino.h"
#include "../../globals.h"
#include "../../types.h"
#include "battery.h"
#include "draw.h"
#include "../../config.h"
#include <math.h>

static bool palaAdcReady = false;

void batteryInit() {
  if (palaAdcReady) return;
  pinMode(BAT_ADC_PIN, INPUT);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  analogReadMilliVolts(BAT_ADC_PIN);
  palaAdcReady = true;
}

float readBatteryVoltage() {
  if (!palaAdcReady) batteryInit();
  const int samples = 16;
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) { sum += analogReadMilliVolts(BAT_ADC_PIN); delay(2); }
  float mv = (float)sum / (float)samples;
  return (mv / 1000.0f) * 2.0f;
}

int batteryPercentFromVoltage(float v) {
  if (v >= 4.35f) return 100;
  if (v <= 3.20f) return 0;
  if (v >= 4.20f) return 100;
  const float volts[] = {3.20f, 3.40f, 3.70f, 3.90f, 4.20f};
  const int   pct[]   = {0,     25,    50,    75,    100};
  for (int i = 1; i < 5; i++) {
    if (v <= volts[i]) {
      float t = (v - volts[i-1]) / (volts[i] - volts[i-1]);
      int p = pct[i-1] + (int)((pct[i] - pct[i-1]) * t + 0.5f);
      p = ((p + 2) / 5) * 5;
      return constrain(p, 0, 100);
    }
  }
  return 100;
}

int readBatteryPercent() {
  float v = readBatteryVoltage();
  if (v <= 0.1f) return -1;
  return batteryPercentFromVoltage(v);
}

static float gChargeLastV = 0;
static uint32_t gChargeLastMs = 0;

static bool isUsbAttached() {
#if ARDUINO_USB_CDC_ON_BOOT
  // True when a USB host is connected (Mac, charger with data lines, etc.)
  return (bool)Serial;
#else
  return false;
#endif
}

// No dedicated CHRG pin on this board. Combine voltage + USB attach + rise detection.
bool isBatteryCharging() {
  float v = readBatteryVoltage();

  if (v > 4.22f) return true;

#if ARDUINO_USB_CDC_ON_BOOT
  // USB cable to host/charger while battery rail is alive → show charging bolt.
  if (isUsbAttached() && v > 3.5f) return true;
#endif

  uint32_t now = millis();
  if (gChargeLastMs != 0 && (now - gChargeLastMs) >= 3000) {
    if (v > gChargeLastV + 0.012f) {
      gChargeLastV = v;
      gChargeLastMs = now;
      return true;
    }
    gChargeLastV = v;
    gChargeLastMs = now;
  } else if (gChargeLastMs == 0) {
    gChargeLastV = v;
    gChargeLastMs = now;
  }

  return false;
}

void drawThickArcDot(int cx, int cy, int r, int deg, int thickness, uint8_t color) {
  float a = ((float)deg - 90.0f) * PI / 180.0f;
  int x = cx + (int)roundf(cosf(a) * r);
  int y = cy + (int)roundf(sinf(a) * r);
  if (thickness <= 1) px(x, y, color);
  else fillCircle(x, y, thickness / 2, color);
}

void drawBatteryRing(int percent) {
  const int cx = 100, cy = 100, r = 82;
  strokeCircle(cx, cy, r, 1, BLACK);
  if (percent < 0) return;
  percent = constrain(percent, 0, 100);
  int endDeg = (360 * percent) / 100;
  for (int deg = 0; deg <= endDeg; deg += 2)
    drawThickArcDot(cx, cy, r, deg, 3, BLACK);
}
