#pragma once

#include <stddef.h>

#ifdef FORREST_BLE_VOICE

// FIFO queue of WAV files on SD, drained over BLE when a phone connects.

bool bleQueueEnsureDir();

// Allocate a new recording path (e.g. /ble_queue/rec_12345.wav).
bool bleQueueAllocPath(char* out, size_t outLen);

// Register a completed recording (basename only, file must already exist).
bool bleQueueAdd(const char* basename);

// Full path of the oldest queued file, or false if empty.
bool bleQueuePeekPath(char* out, size_t outLen);

// Remove the oldest entry and delete its file.
bool bleQueueRemoveFirst();

int bleQueueCount();

#endif
