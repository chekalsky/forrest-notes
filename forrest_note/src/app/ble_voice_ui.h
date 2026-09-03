#pragma once

#include <Arduino.h>

#ifdef FORREST_BLE_VOICE

enum BleVoiceUiState : uint8_t {
  BLE_UI_READY,
  BLE_UI_LISTENING,
  BLE_UI_SENDING,
  BLE_UI_DONE,
  BLE_UI_ERROR,
};

void bleVoiceUiInit();
void bleVoiceUiSetPhoneConnected(bool connected);
void bleVoiceUiSetState(BleVoiceUiState state, const char* detail = nullptr);
void bleVoiceUiUpdateListening(uint32_t elapsedMs);
void bleVoiceUiSetSendProgress(uint32_t sentBytes, uint32_t totalBytes, uint32_t elapsedMs);
void bleVoiceUiFinishSending(uint32_t totalBytes, uint32_t elapsedMs);
void bleVoiceUiSetQueueCount(int count);
void bleVoiceUiShowResult(const char* text);

// Call from main loop — refreshes header when charge state changes on Ready screen.
void bleVoiceUiTick();

#endif
