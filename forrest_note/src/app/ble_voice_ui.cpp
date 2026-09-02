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
static int gSendPct = 0;
static int gQueueCount = 0;

static int gRecLevelSmoothed = 0;
static int gRecDrawnR = -1;
static bool gRecBlinkOn = true;
static bool gLastCharging = false;

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

  int batt = readBatteryPercent(4);
  bool charging = isBatteryCharging();
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

static int levelRadius(int level) {
  int delta = level - gRecLevelSmoothed;
  if (delta > 0) gRecLevelSmoothed += (delta + 1) / 2;
  else gRecLevelSmoothed += delta / 5;

  float n = (float)gRecLevelSmoothed / 152.0f;
  if (n < 0) n = 0;
  if (n > 1) n = 1;
  n = n * n * (3.0f - 2.0f * n);
  int r = 22 + (int)(n * 40.0f + 0.5f);
  if (r < 20) r = 20;
  if (r > 64) r = 64;
  return r;
}

static void drawCheck(int cx, int cy) {
  strokeCircle(cx, cy, 36, 3, BLACK);
  for (int t = -2; t <= 2; t++) {
    line(cx - 18, cy - 2 + t, cx - 4, cy + 14 + t, BLACK);
    line(cx - 4, cy + 14 + t, cx + 24, cy - 16 + t, BLACK);
  }
}

static void drawProgressBar(int pct) {
  const int bx = 24, by = 118, bw = 152, bh = 14;
  strokeRoundRect(bx, by, bw, bh, 4, 2, BLACK);
  int fillW = (bw - 6) * constrain(pct, 0, 100) / 100;
  if (fillW > 0) fillRoundRect(bx + 3, by + 3, fillW, bh - 6, 2, BLACK);
}

static void drawQueueBadge() {
  if (gQueueCount <= 0) return;
  char q[16];
  snprintf(q, sizeof(q), "%d queued", gQueueCount);
  drawStrC(W / 2, 158, q, 1, BLACK);
}

static void paintScreen() {
  clearWhite();
  drawHeaderBar();

  switch (gUiState) {
    case BLE_UI_READY:
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
      fillRect(0, 26, W, H - 26, BLACK);
      drawStrC(W / 2, 40, "Listening", 1, WHITE);
      gRecDrawnR = -1;
      gRecLevelSmoothed = 0;
      {
        int r0 = levelRadius(0);
        gRecDrawnR = r0;
        gRecBlinkOn = true;
        fillCircle(W / 2, 100, r0, WHITE);
        fillCircle(178, 38, 6, WHITE);
        drawStrC(W / 2, 168, "0:00", 1, WHITE);
      }
      drawHint("release to send");
      break;

    case BLE_UI_SENDING:
      drawStrC(W / 2, 52, "Sending", 2, BLACK);
      drawProgressBar(gSendPct);
      if (gDetail[0]) drawStrC(W / 2, 148, gDetail, 1, BLACK);
      drawHint("");
      break;

    case BLE_UI_DONE:
      drawCheck(W / 2, 88);
      drawStrC(W / 2, 138, gResult[0] ? gResult : "Sent", 2, BLACK);
      drawHint("");
      break;

    case BLE_UI_ERROR:
      strokeCircle(W / 2, 82, 32, 3, BLACK);
      line(W / 2 - 18, 64, W / 2 + 18, 100, BLACK);
      line(W / 2 + 18, 64, W / 2 - 18, 100, BLACK);
      drawStrC(W / 2, 128, gDetail[0] ? gDetail : "Error", 1, BLACK);
      drawHint("hold REC to retry");
      break;
  }

  refresh();
}

static void drawListeningPartial(uint32_t elapsedMs, int radius, bool blink) {
  fillRect(0, 50, W, 120, BLACK);
  fillCircle(W / 2, 100, radius, WHITE);
  if (blink) fillCircle(178, 38, 6, WHITE);
  else strokeCircle(178, 38, 6, 2, WHITE);

  uint32_t sec = elapsedMs / 1000UL;
  char tbuf[8];
  snprintf(tbuf, sizeof(tbuf), "%lu:%02lu",
           (unsigned long)(sec / 60UL), (unsigned long)(sec % 60UL));
  drawStrC(W / 2, 168, tbuf, 1, WHITE);
  display->EPD_DisplayPartTrigger();
}

void bleVoiceUiInit() {
  gUiState = BLE_UI_READY;
  gPhoneConnected = false;
  gDetail[0] = gResult[0] = '\0';
  gSendPct = 0;
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
  paintScreen();
}

void bleVoiceUiUpdateListening(uint32_t elapsedMs, int level) {
  if (gUiState != BLE_UI_LISTENING) return;
  if (displayBusy()) return;

  int r = levelRadius(level);
  bool blink = ((elapsedMs / 850UL) & 1UL) == 0UL;
  if (r == gRecDrawnR && blink == gRecBlinkOn) return;

  gRecDrawnR = r;
  gRecBlinkOn = blink;
  drawListeningPartial(elapsedMs, r, blink);
}

void bleVoiceUiSetSendProgress(int percent) {
  gSendPct = constrain(percent, 0, 100);
  if (gUiState != BLE_UI_SENDING) return;
  if (displayBusy()) return;

  beginBufferDraw();
  drawProgressBar(gSendPct);
  endBufferDraw();
  refreshAsyncFromBuffer();
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
  uint32_t now = millis();
  if (now - lastCheckMs < 10000) return;
  lastCheckMs = now;

  bool charging = isBatteryCharging();
  if (charging != gLastCharging) paintScreen();
}

#endif
