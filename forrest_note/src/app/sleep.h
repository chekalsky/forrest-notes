#pragma once

// Deep-sleep: paint the sleep screen, cut radios/rails, wake on REC or PWR.
// resetActivity() is called from the button FSM. Sleep is not an AppState.
// Control flow: see ARCHITECTURE.md "UI state machine".

void resetActivity();
void enterUltraSleep();
#ifdef FORREST_BLE_VOICE
void enterBleVoiceSleep();
// RTC latch: survives USB re-enumeration after deep-sleep wake so we don't
// immediately re-sleep when the wake button has already been released.
bool bleVoiceWakeLatchGet();
void bleVoiceWakeLatchSet();
void bleVoiceWakeLatchClear();
#endif
