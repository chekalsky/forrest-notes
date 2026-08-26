#pragma once
#include <Arduino.h>
#include "SD_MMC.h"

// Write a standard 16 kHz mono 16-bit PCM WAV header at offset 0.
bool writePcmWavHeader(File& f, uint32_t dataBytes);

// Bytes of PCM payload (file size minus 44), or 0 if unreadable/too small.
uint32_t wavPcmBytes(const char* path);

// Copy [pcmOffset, pcmOffset+pcmBytes) from src into a new standalone WAV at dst.
bool extractWavPcmChunk(const char* srcPath, const char* dstPath,
                        uint32_t pcmOffset, uint32_t pcmBytes);

// Truncate an existing WAV to at most maxPcmBytes of PCM (for speaker refs).
bool truncateWavPcm(const char* path, uint32_t maxPcmBytes);
