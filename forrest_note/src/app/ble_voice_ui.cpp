#include "ble_voice_ui.h"

#ifdef FORREST_BLE_VOICE

#include "../../config.h"
#include "../../globals.h"
#include "battery.h"
#include "draw.h"

static BleVoiceUiState gUiState = BLE_UI_READY;
static bool gPhoneConnected = false;
static char gDetail[48] = "";
static char gResult[41] = "";
static int gQueueCount = 0;

static int gSendDrawnDeg = -1;
static char gSendDrawnSubtitle[32] = "";
static uint32_t gSendLastDrawMs = 0;

static bool gRecPulseOn = true;
static uint32_t gRecDrawnSec = UINT32_MAX;
static uint32_t gRecLastTimerDrawMs = 0;
static bool gLastCharging = false;

// Listening / sending layout — full screen, no header/footer
static constexpr int kListenCy = 92;
static constexpr int kListenRx = 100;
static constexpr int kListenTimerY = 168;
static constexpr int kListenPulseR = 36;
static constexpr int kSendRingR = 56;

static void drawBoltWhite(int x, int y) {
  fillTriangle(x + 7, y, x + 1, y + 9, x + 6, y + 9, WHITE);
  fillTriangle(x + 5, y + 8, x + 10, y + 8, x + 3, y + 18, WHITE);
}

static void drawBtDot(int x, int y, bool on) {
  if (on) fillCircle(x, y, 5, WHITE);
  else strokeCircle(x, y, 5, 2, WHITE);
}

static void drawHeaderBar() {
  fillRect(0, 0, W, 26, BLACK);
  drawStr(10, 8, "Forrest", 1, WHITE);

  float v = readBatteryVoltage(12);
  int batt = (v <= 0.1f) ? -1 : batteryPercentFromVoltage(v);
  bool charging = isBatteryChargingAt(v);
  gLastCharging = charging;

  char b[8];
  if (batt < 0) snprintf(b, sizeof(b), "--");
  else if (charging && batt >= 99) snprintf(b, sizeof(b), "full");
  else snprintf(b, sizeof(b), "%d%%", batt);

  int tw = textW(b, 1);
  const int btX = W - 10 - 7;
  const int boltW = charging ? 14 : 0;
  int x = btX - 8 - tw - boltW;
  if (charging) {
    drawBoltWhite(x, 6);
    x += boltW;
  }
  drawStr(x, 8, b, 1, WHITE);
  drawBtDot(btX, 13, gPhoneConnected);
}

static void drawHint(const char* line) {
  hline(0, 178, W, BLACK);
  drawStrC(W / 2, 186, line, 1, BLACK);
}

static void resetListeningDrawState() {
  gRecPulseOn = true;
  gRecDrawnSec = UINT32_MAX;
  gRecLastTimerDrawMs = 0;
}

static void drawListeningPulse(bool on) {
  if (on) fillCircle(kListenRx, kListenCy, kListenPulseR, WHITE);
}

static void drawListeningTimer(uint32_t elapsedMs) {
  uint32_t sec = elapsedMs / 1000UL;
  char tbuf[8];
  snprintf(tbuf, sizeof(tbuf), "%lu:%02lu",
           (unsigned long)(sec / 60UL), (unsigned long)(sec % 60UL));
  drawStrC(kListenRx, kListenTimerY, tbuf, 1, WHITE);
}

static void paintListeningFull(uint32_t elapsedMs) {
  fillRect(0, 0, W, H, BLACK);
  drawListeningPulse(true);
  drawListeningTimer(elapsedMs);
  gRecPulseOn = true;
  gRecDrawnSec = elapsedMs / 1000UL;
  gRecLastTimerDrawMs = elapsedMs;
}

static void refreshListeningPulseBand() {
  int clearR = kListenPulseR + 4;
  int y0 = kListenCy - clearR;
  int h = clearR * 2;
  if (y0 < 0) y0 = 0;
  if (y0 + h > kListenTimerY - 8) h = (kListenTimerY - 8) - y0;
  fillRect(0, y0, W, h, BLACK);
}

static void refreshListeningTimerBand() {
  fillRect(0, kListenTimerY - 10, W, H - (kListenTimerY - 10), BLACK);
}

static void resetSendingDrawState() {
  gSendDrawnDeg = -1;
  gSendDrawnSubtitle[0] = '\0';
  gSendLastDrawMs = 0;
}

static void formatSendSubtitle(char* buf, size_t len,
                               uint32_t sent, uint32_t total, uint32_t elapsedMs) {
  unsigned sentKb = sent / 1024;
  unsigned totalKb = (total + 1023) / 1024;
  if (sent == 0 || total == 0) {
    snprintf(buf, len, "0/%u KB · -- left", totalKb);
    return;
  }
  if (sent >= total) {
    snprintf(buf, len, "%u/%u KB · 0s left", sentKb, totalKb);
    return;
  }
  uint32_t secLeft = 0;
  if (elapsedMs > 0) {
    uint64_t rem = (uint64_t)(total - sent) * elapsedMs / sent;
    secLeft = (uint32_t)((rem + 999) / 1000);
  }
  snprintf(buf, len, "%u/%u KB · %lus left", sentKb, totalKb, (unsigned long)secLeft);
}

static void drawSendTrack(int cx, int cy, int r) {
  strokeCircle(cx, cy, r, 2, WHITE);
}

static void drawSendArc(int cx, int cy, int r, int endDeg) {
  if (endDeg <= 0) return;
  if (endDeg > 360) endDeg = 360;
  for (int deg = 0; deg <= endDeg; deg += 2)
    drawThickArcDot(cx, cy, r, deg, 4, WHITE);
}

static void drawSendSubtitle(const char* text) {
  drawStrC(kListenRx, kListenTimerY, text, 1, WHITE);
}

static void refreshSendRingBand() {
  int clearR = kSendRingR + 8;
  int y0 = kListenCy - clearR;
  int h = clearR * 2;
  if (y0 < 0) y0 = 0;
  if (y0 + h > kListenTimerY - 8) h = (kListenTimerY - 8) - y0;
  fillRect(0, y0, W, h, BLACK);
}

static void refreshSendTextBand() {
  fillRect(0, kListenTimerY - 10, W, H - (kListenTimerY - 10), BLACK);
}

static void paintSendingFull(uint32_t sent, uint32_t total, uint32_t elapsedMs) {
  char subtitle[32];
  formatSendSubtitle(subtitle, sizeof(subtitle), sent, total, elapsedMs);
  int endDeg = (total > 0) ? (int)((360ULL * sent) / total) : 0;

  fillRect(0, 0, W, H, BLACK);
  drawSendTrack(kListenRx, kListenCy, kSendRingR);
  drawSendArc(kListenRx, kListenCy, kSendRingR, endDeg);
  drawSendSubtitle(subtitle);

  gSendDrawnDeg = endDeg;
  strncpy(gSendDrawnSubtitle, subtitle, sizeof(gSendDrawnSubtitle) - 1);
  gSendDrawnSubtitle[sizeof(gSendDrawnSubtitle) - 1] = '\0';
  gSendLastDrawMs = elapsedMs;
}

static void drawCheck(int cx, int cy) {
  strokeCircle(cx, cy, 36, 3, BLACK);
  for (int t = -2; t <= 2; t++) {
    line(cx - 18, cy - 2 + t, cx - 4, cy + 14 + t, BLACK);
    line(cx - 4, cy + 14 + t, cx + 24, cy - 16 + t, BLACK);
  }
}

static void drawQueueBadge() {
  if (gQueueCount <= 0) return;
  char q[16];
  snprintf(q, sizeof(q), "%d queued", gQueueCount);
  drawStrC(W / 2, 158, q, 1, BLACK);
}

static void paintScreen() {
  clearWhite();

  switch (gUiState) {
    case BLE_UI_READY:
      drawHeaderBar();
      fillCircle(W / 2, 92, 28, BLACK);
      fillRect(W / 2 - 10, 92 - 36, 20, 32, WHITE);
      fillCircle(W / 2, 92 - 36, 10, WHITE);
      fillCircle(W / 2, 92 + 8, 10, WHITE);
      drawStrC(W / 2, 138, gPhoneConnected ? "Ready" : "No phone", 2, BLACK);
      if (gDetail[0]) drawStrC(W / 2, 158, gDetail, 1, BLACK);
      else drawQueueBadge();
      drawHint("hold REC · speak · release");
      break;

    case BLE_UI_LISTENING:
      resetListeningDrawState();
      paintListeningFull(0);
      break;

    case BLE_UI_SENDING:
      resetSendingDrawState();
      fillRect(0, 0, W, H, BLACK);
      break;

    case BLE_UI_DONE:
      drawCheck(W / 2, 88);
      drawStrC(W / 2, 138, gResult[0] ? gResult : "Sent", 2, BLACK);
      break;

    case BLE_UI_ERROR:
      strokeCircle(W / 2, 82, 32, 3, BLACK);
      line(W / 2 - 18, 64, W / 2 + 18, 100, BLACK);
      line(W / 2 + 18, 64, W / 2 - 18, 100, BLACK);
      drawStrC(W / 2, 128, gDetail[0] ? gDetail : "Error", 1, BLACK);
      break;
  }

  refresh();
}

void bleVoiceUiInit() {
  gUiState = BLE_UI_READY;
  gPhoneConnected = false;
  gDetail[0] = gResult[0] = '\0';
  resetSendingDrawState();
  paintScreen();
}

void bleVoiceUiSetPhoneConnected(bool connected) {
  if (gPhoneConnected == connected && gUiState == BLE_UI_READY) return;
  gPhoneConnected = connected;
  if (gUiState == BLE_UI_READY) paintScreen();
}

void bleVoiceUiSetQueueCount(int count) {
  if (count < 0) count = 0;
  if (gQueueCount == count && gUiState == BLE_UI_READY && !gDetail[0]) return;
  gQueueCount = count;
  if (gUiState == BLE_UI_READY) paintScreen();
}

void bleVoiceUiSetState(BleVoiceUiState state, const char* detail) {
  gUiState = state;
  gDetail[0] = '\0';
  if (detail) {
    strncpy(gDetail, detail, sizeof(gDetail) - 1);
    gDetail[sizeof(gDetail) - 1] = '\0';
  }
  if (state != BLE_UI_DONE) gResult[0] = '\0';
  if (state == BLE_UI_SENDING) resetSendingDrawState();
  paintScreen();
}

void bleVoiceUiUpdateListening(uint32_t elapsedMs) {
  if (gUiState != BLE_UI_LISTENING) return;
  if (displayBusy()) return;

  bool pulseOn = ((elapsedMs / 850UL) & 1UL) == 0UL;
  uint32_t sec = elapsedMs / 1000UL;

  bool needPulse = pulseOn != gRecPulseOn;
  bool needTimer = (sec != gRecDrawnSec)
      || (elapsedMs - gRecLastTimerDrawMs >= 500);

  if (!needPulse && !needTimer) return;

  if (needPulse) {
    refreshListeningPulseBand();
    drawListeningPulse(pulseOn);
    gRecPulseOn = pulseOn;
  }

  if (needTimer) {
    refreshListeningTimerBand();
    drawListeningTimer(elapsedMs);
    gRecDrawnSec = sec;
    gRecLastTimerDrawMs = elapsedMs;
  }

  display->EPD_DisplayPartTrigger();
}

void bleVoiceUiSetSendProgress(uint32_t sentBytes, uint32_t totalBytes, uint32_t elapsedMs) {
  if (gUiState != BLE_UI_SENDING) return;
  if (displayBusy()) return;
  if (totalBytes == 0) totalBytes = 1;

  if (gSendDrawnDeg < 0) {
    paintSendingFull(sentBytes, totalBytes, elapsedMs);
    display->EPD_DisplayPartTrigger();
    return;
  }

  int endDeg = (int)((360ULL * sentBytes) / totalBytes);
  if (endDeg > 360) endDeg = 360;

  char subtitle[32];
  formatSendSubtitle(subtitle, sizeof(subtitle), sentBytes, totalBytes, elapsedMs);

  bool needArc = (endDeg != gSendDrawnDeg);
  bool needText = strcmp(subtitle, gSendDrawnSubtitle) != 0
      || (elapsedMs - gSendLastDrawMs >= 250);
  if (!needArc && !needText) return;

  if (needArc) {
    refreshSendRingBand();
    drawSendTrack(kListenRx, kListenCy, kSendRingR);
    drawSendArc(kListenRx, kListenCy, kSendRingR, endDeg);
    gSendDrawnDeg = endDeg;
  }

  if (needText) {
    refreshSendTextBand();
    drawSendSubtitle(subtitle);
    strncpy(gSendDrawnSubtitle, subtitle, sizeof(gSendDrawnSubtitle) - 1);
    gSendDrawnSubtitle[sizeof(gSendDrawnSubtitle) - 1] = '\0';
    gSendLastDrawMs = elapsedMs;
  }

  display->EPD_DisplayPartTrigger();
}

void bleVoiceUiFinishSending(uint32_t totalBytes, uint32_t elapsedMs) {
  if (gUiState != BLE_UI_SENDING) return;

  while (displayBusy()) delay(10);
  paintSendingFull(totalBytes, totalBytes, elapsedMs);
  display->EPD_DisplayPartTrigger();
  while (displayBusy()) delay(10);
  delay(200);
}

void bleVoiceUiShowResult(const char* text) {
  if (!text) return;
  strncpy(gResult, text, sizeof(gResult) - 1);
  gResult[sizeof(gResult) - 1] = '\0';
  gUiState = BLE_UI_DONE;
  paintScreen();
}

void bleVoiceUiTick() {
  if (gUiState != BLE_UI_READY) return;

  static uint32_t lastCheckMs = 0;
  static int lastBatt = -1;
  uint32_t now = millis();
  if (now - lastCheckMs < 5000) return;
  lastCheckMs = now;

  float v = readBatteryVoltage(12);
  int batt = (v <= 0.1f) ? -1 : batteryPercentFromVoltage(v);
  bool charging = isBatteryChargingAt(v);

  if (charging != gLastCharging || batt != lastBatt) {
    gLastCharging = charging;
    lastBatt = batt;
    paintScreen();
  }
}

#endif
