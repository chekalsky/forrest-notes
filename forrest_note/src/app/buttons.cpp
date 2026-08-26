#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "buttons.h"
#include "flows.h"
#include "sleep.h"

bool isDown(int pin) { return digitalRead(pin) == LOW; }

// Non-blocking per-button state machine, sampled once per loop (no busy-wait).
// Emits:
//   EV_SINGLE  on release, if held < BTN_LONG_MS  (acts the instant you let go)
//   EV_LONG    the moment the hold crosses BTN_LONG_MS (fires once, no wait-for-release)
// Double-press was removed: it forced every tap to wait ~200 ms for a possible second
// press, which is what made the UI feel laggy.
namespace {
  enum Phase { PH_IDLE, PH_DEBOUNCE, PH_DOWN, PH_LONGFIRED };
  struct BtnState { Phase phase; uint32_t tDown; };
  BtnState st[2] = {{PH_IDLE, 0}, {PH_IDLE, 0}};
  inline int idx(int pin) { return pin == BTN_REC ? 0 : 1; }
}

ButtonEvent readButtonEvent(int pin) {
  BtnState& b = st[idx(pin)];
  bool down = isDown(pin);
  uint32_t now = millis();

  switch (b.phase) {
    case PH_IDLE:
      if (down) { b.phase = PH_DEBOUNCE; b.tDown = now; }
      return EV_NONE;

    case PH_DEBOUNCE:
      if (!down) { b.phase = PH_IDLE; return EV_NONE; }      // bounce / too brief
      if (now - b.tDown >= BTN_DEBOUNCE_MS) b.phase = PH_DOWN;
      return EV_NONE;

    case PH_DOWN:
      if (now - b.tDown >= BTN_LONG_MS) {                    // crossed the long threshold
        b.phase = PH_LONGFIRED;
        resetActivity();
        return EV_LONG;
      }
      if (!down) {                                           // released as a tap
        b.phase = PH_IDLE;
        resetActivity();
        return EV_SINGLE;
      }
      return EV_NONE;

    case PH_LONGFIRED:                                       // long already fired; await release
      if (!down) b.phase = PH_IDLE;
      return EV_NONE;
  }
  return EV_NONE;
}

// IDLE record gestures:
//   hold  ≥ REC_HOLD_MS  → short note (release to stop)
//   3× tap within REC_TRIPLE_GAP_MS of each other → meeting (3× again to stop)
bool handleIdleRec() {
  static bool tracking = false;
  static uint32_t t0 = 0;
  static bool wasDown = false;
  static int taps = 0;
  static uint32_t lastTapMs = 0;

  bool down = isDown(BTN_REC);
  uint32_t now = millis();

  if (taps > 0 && now - lastTapMs > REC_TRIPLE_GAP_MS) taps = 0;

  if (down && !wasDown) {
    tracking = true;
    t0 = now;
    resetActivity();
  }

  if (!down && wasDown && tracking) {
    uint32_t held = now - t0;
    tracking = false;
    if (held >= BTN_DEBOUNCE_MS && held < REC_HOLD_MS) {
      taps++;
      lastTapMs = now;
      Serial.printf("[Rec] start tap %d/3\n", taps);
      if (taps >= 3) {
        taps = 0;
        wasDown = down;
        startMeetingRecordFlow();
        return true;
      }
    }
  }
  wasDown = down;

  if (!down) return taps > 0;   // stay awake while a triple is in progress

  if (now - t0 >= REC_HOLD_MS) {
    tracking = false;
    taps = 0;
    startRecordFlow();
  }
  return true;   // consume the press while it's held
}
