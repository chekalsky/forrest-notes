#include "ble_voice_queue.h"

#ifdef FORREST_BLE_VOICE

#include <SD_MMC.h>
#include "../../config.h"

#define BLE_QUEUE_DIR "/ble_queue"
#define BLE_QUEUE_INDEX BLE_QUEUE_DIR "/queue.txt"

bool bleQueueEnsureDir() {
  if (!SD_MMC.exists(BLE_QUEUE_DIR)) {
    if (!SD_MMC.mkdir(BLE_QUEUE_DIR)) return false;
  }
  return true;
}

bool bleQueueAllocPath(char* out, size_t outLen) {
  if (!bleQueueEnsureDir()) return false;
  int n = snprintf(out, outLen, "%s/rec_%lu.wav", BLE_QUEUE_DIR, (unsigned long)millis());
  return n > 0 && (size_t)n < outLen;
}

static bool readFirstBasename(char* out, size_t outLen) {
  File f = SD_MMC.open(BLE_QUEUE_INDEX);
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  if (line.length() == 0) return false;
  strncpy(out, line.c_str(), outLen - 1);
  out[outLen - 1] = '\0';
  return true;
}

bool bleQueueAdd(const char* basename) {
  if (!basename || !basename[0]) return false;
  if (!bleQueueEnsureDir()) return false;

  File f = SD_MMC.open(BLE_QUEUE_INDEX, FILE_APPEND);
  if (!f) return false;
  f.println(basename);
  f.close();
  Serial.printf("[BLE] queued %s (total %d)\n", basename, bleQueueCount());
  return true;
}

bool bleQueuePeekPath(char* out, size_t outLen) {
  char base[48];
  if (!readFirstBasename(base, sizeof(base))) return false;
  int n = snprintf(out, outLen, "%s/%s", BLE_QUEUE_DIR, base);
  return n > 0 && (size_t)n < outLen;
}

bool bleQueueRemoveFirst() {
  char first[48];
  if (!readFirstBasename(first, sizeof(first))) return false;

  char path[64];
  snprintf(path, sizeof(path), "%s/%s", BLE_QUEUE_DIR, first);
  if (SD_MMC.exists(path)) SD_MMC.remove(path);

  File f = SD_MMC.open(BLE_QUEUE_INDEX);
  if (!f) return false;

  String rest;
  bool skipped = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (!skipped && line == first) {
      skipped = true;
      continue;
    }
    rest += line;
    rest += '\n';
  }
  f.close();

  if (rest.length() == 0) {
    SD_MMC.remove(BLE_QUEUE_INDEX);
  } else {
    File w = SD_MMC.open(BLE_QUEUE_INDEX, FILE_WRITE);
    if (!w) return false;
    w.print(rest);
    w.close();
  }
  Serial.printf("[BLE] dequeued %s (%d left)\n", first, bleQueueCount());
  return true;
}

int bleQueueCount() {
  if (!SD_MMC.exists(BLE_QUEUE_INDEX)) return 0;
  File f = SD_MMC.open(BLE_QUEUE_INDEX);
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) n++;
  }
  f.close();
  return n;
}

#endif
