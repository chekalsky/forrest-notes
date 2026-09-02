#pragma once

// BLE voice bridge: hold REC → record WAV → release → transfer to iPhone.
// Enable with -DFORREST_BLE_VOICE in build or uncomment below.
// #define FORREST_BLE_VOICE 1

void bleVoiceSetup();
void bleVoiceLoop();
void bleVoiceEnterAwake();
void bleVoiceTouchActivity();
bool bleVoiceCanSleep();
void bleVoiceTeardown();

// Record while REC held; on release finalize WAV and queue BLE transfer.
void bleVoiceHandleButton();
