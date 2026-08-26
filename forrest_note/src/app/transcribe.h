#pragma once
#include <Arduino.h>

// OpenAI diarized transcription of pending WAVs on the SD card.
// Call transcribeAll() from startSyncFlow() while Wi-Fi is connected and an
// OpenAI key is stored. Failures stay pending (!hasText) and retry next sync.
// Control flow: see ARCHITECTURE.md "Sync pipeline".

bool transcribe(const String& wavPath, int noteNum);
bool transcribe(const String& wavPath, int noteNum, int notesDone, int notesTotal);
void transcribeAll();
