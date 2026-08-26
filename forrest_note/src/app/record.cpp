#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "record.h"
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "notes.h"
#include "ui.h"
#include "wav_util.h"
#include "../../sounds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

extern "C" {
#include "../../src/audio/audio_bsp.h"
}

// Capture (producer) and SD-write (consumer) run on separate cores connected by
// a PSRAM ring buffer. The producer keeps draining the I2S DMA at line rate so a
// slow SD write only grows the ring instead of dropping samples.
struct RecCtx {
  RingbufHandle_t   ring;
  volatile bool     running;    // consumer -> producer: keep capturing
  volatile bool     finished;   // producer -> consumer: capture loop exited
};

static void recProducerTask(void* arg) {
  RecCtx* ctx = (RecCtx*)arg;
  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF,   MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF/2, MALLOC_CAP_8BIT);
  const int monoSamples = REC_BUF / 4;   // stereo int16 in -> mono int16 out

  if (sbuf && mbuf) {
    while (ctx->running) {
      audio_playback_read((void*)sbuf, REC_BUF);   // blocking read from codec DMA
      for (int i = 0; i < monoSamples; i++) mbuf[i] = sbuf[i * 2];  // left channel
      // Block briefly if the ring is full (SD catching up); never silently drop.
      xRingbufferSend(ctx->ring, mbuf, monoSamples * 2, pdMS_TO_TICKS(1000));
    }
  }

  if (sbuf) heap_caps_free(sbuf);
  if (mbuf) heap_caps_free(mbuf);
  ctx->finished = true;
  vTaskDelete(NULL);
}

// Push header sizes + FatFS buffers to the card so a power loss still leaves
// a playable (truncated) WAV. Returns the byte offset where PCM continues.
static uint32_t checkpointWav(File& f, uint32_t dataBytes) {
  uint32_t dataPos = 44UL + dataBytes;
  f.flush();
  writePcmWavHeader(f, dataBytes);
  f.flush();
  f.seek(dataPos);
  return dataPos;
}

bool record(bool holdMode) {
  int num = nextNoteNumber();
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, num);
  Serial.printf("[Rec] %s mode=%s\n", path, holdMode ? "hold" : "meeting");

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;

  // Valid header from the first byte — empty data chunk until the first checkpoint.
  writePcmWavHeader(f, 0);
  f.flush();
  f.seek(44);

  // Register in the index immediately so a crash / skipped tag confirm still
  // leaves a discoverable note (provisional tag = DEFAULT_NOTE_TAG).
  writeNoteMeta(num, DEFAULT_NOTE_TAG);
  addToIndex(num, DEFAULT_NOTE_TAG, false);
  lastRecNum = num;

  RecCtx ctx;
  ctx.ring = xRingbufferCreateWithCaps(REC_RING_LEN, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
  if (!ctx.ring) {
    f.close();
    deleteNote(num);
    lastRecNum = -1;
    return false;
  }
  ctx.running  = true;
  ctx.finished = false;

  TaskHandle_t producer = NULL;
  if (xTaskCreatePinnedToCore(recProducerTask, "recprod", 4096, &ctx, 6, &producer, 0) != pdPASS) {
    vRingbufferDeleteWithCaps(ctx.ring);
    f.close();
    deleteNote(num);
    lastRecNum = -1;
    return false;
  }

  uint32_t totalMono = 0, t0 = millis();
  int      recPeak = 0;   // peak |sample| since the last UI update
  const uint32_t maxMs = holdMode ? MAX_REC_MS : MAX_MEETING_MS;

  // Meeting stop: count three short REC taps (same gesture as start).
  int      stopTaps = 0;
  uint32_t lastTapMs = 0;
  bool     wasDown = (digitalRead(BTN_REC) == LOW);
  uint32_t pressStart = wasDown ? millis() : 0;
  uint32_t lastFlush = millis();
  bool     writeOk = true;

  auto drain = [&](TickType_t wait) -> bool {
    size_t got = 0;
    void* item = xRingbufferReceive(ctx.ring, &got, wait);
    if (!item) return false;
    int16_t* sp = (int16_t*)item;
    int ns = got / 2;
    for (int i = 0; i < ns; i++) { int a = abs(sp[i]); if (a > recPeak) recPeak = a; }
    size_t written = f.write((uint8_t*)item, got);
    vRingbufferReturnItem(ctx.ring, item);
    totalMono += written;
    if (written != got) {
      writeOk = false;
      Serial.printf("[Rec] SD write short %u/%u\n", (unsigned)written, (unsigned)got);
    }
    return true;
  };

  // Hold mode: record while held (min 500 ms). Meeting mode: until 3× tap or cap.
  // UI is e-ink gated inside showRecordingLive(); poll peak ~4 Hz so we don't
  // burn cycles redrawing a panel that only accepts ~1 Hz meaningful changes.
  uint32_t lastUi = 0;
  bool stop = false;
  while (!stop && writeOk && (millis() - t0 < maxMs)) {
    drain(pdMS_TO_TICKS(40));

    if (holdMode) {
      if (!(digitalRead(BTN_REC) == LOW || millis() - t0 < 500)) stop = true;
    } else {
      bool down = (digitalRead(BTN_REC) == LOW);
      uint32_t now = millis();
      if (down && !wasDown) {
        pressStart = now;
      } else if (!down && wasDown) {
        uint32_t held = now - pressStart;
        if (held >= BTN_DEBOUNCE_MS && held < BTN_LONG_MS) {
          if (stopTaps > 0 && now - lastTapMs > REC_TRIPLE_GAP_MS) stopTaps = 0;
          stopTaps++;
          lastTapMs = now;
          Serial.printf("[Rec] meeting stop tap %d/3\n", stopTaps);
          if (stopTaps >= 3) stop = true;
        }
      }
      wasDown = down;
    }

    uint32_t now = millis();
    if (now - lastFlush >= REC_FLUSH_MS) {
      lastFlush = now;
      checkpointWav(f, totalMono);
    }
    if (now - lastUi >= 200) {
      lastUi = now;
      int lvl = (int)((long)recPeak * 152L * 3L / 32767L);   // ×3 boost for speech
      if (lvl > 152) lvl = 152;
      showRecordingLive(now - t0, lvl, !holdMode);
      recPeak = 0;
    }
  }

  // Stop the producer and flush everything still buffered.
  ctx.running = false;
  while (!ctx.finished) drain(pdMS_TO_TICKS(50));
  while (drain(0)) { /* final drain */ }

  vRingbufferDeleteWithCaps(ctx.ring);

  checkpointWav(f, totalMono);
  f.close();

  if (totalMono <= 1000) {
    // Too short to keep — drop the provisional index row + files.
    deleteNote(num);
    lastRecNum = -1;
    Serial.println("[Rec] discarded (too short)");
    return false;
  }

  lastRecNum = num;
  Serial.printf("[Rec] done: %lu bytes%s\n",
                (unsigned long)totalMono, writeOk ? "" : " (truncated SD error)");
  return true;
}

bool playWavFile(const char* path) {
  File f = SD_MMC.open(path);
  if (!f) return false;
  if (f.size() <= 44) { f.close(); return false; }

  f.seek(44);

  const int monoBytes = 1024;
  uint8_t* monoBuf   = (uint8_t*)heap_caps_malloc(monoBytes,     MALLOC_CAP_8BIT);
  int16_t* stereoBuf = (int16_t*)heap_caps_malloc(monoBytes * 2, MALLOC_CAP_8BIT);

  if (!monoBuf || !stereoBuf) {
    if (monoBuf)   heap_caps_free(monoBuf);
    if (stereoBuf) heap_caps_free(stereoBuf);
    f.close();
    return false;
  }

  audioPlaying  = true;
  stopPlayback  = false;

  palaSoundSetEnabled(false);
  audio_playback_set_vol(85);

  while (f.available() && !stopPlayback) {
    int readBytes = f.read(monoBuf, monoBytes);
    if (readBytes <= 0) break;
    if (readBytes & 1) readBytes--;

    int samples = readBytes / 2;
    int16_t* mono = (int16_t*)monoBuf;
    for (int i = 0; i < samples; i++) {
      int16_t s = mono[i];
      stereoBuf[i * 2 + 0] = s;
      stereoBuf[i * 2 + 1] = s;
    }
    audio_playback_write((void*)stereoBuf, (uint32_t)(samples * 2 * sizeof(int16_t)));

    if (digitalRead(BTN_REC) == LOW) {
      delay(20);
      if (digitalRead(BTN_REC) == LOW) {
        while (digitalRead(BTN_REC) == LOW) delay(5);
        stopPlayback = true;
      }
    }
  }

  audio_playback_set_vol(0);
  palaSoundSetEnabled(true);

  heap_caps_free(monoBuf);
  heap_caps_free(stereoBuf);
  f.close();

  audioPlaying = false;
  stopPlayback = false;
  return true;
}
