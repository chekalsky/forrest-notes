#include "speakers.h"
#include "../../config.h"
#include "wav_util.h"
#include "SD_MMC.h"
#include <string.h>

namespace {
  Speaker gSpeakers[MAX_SPEAKERS];
  int     gCount = 0;
  int     gNextId = 1;

  void saveIndex() {
    speakersEnsureDir();
    const char* tmp = "/speakers/index.tmp";
    if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);
    File f = SD_MMC.open(tmp, FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < gCount; i++)
      f.printf("%d,%s\n", gSpeakers[i].id, gSpeakers[i].name);
    f.close();
    if (SD_MMC.exists(SPEAKERS_INDEX)) SD_MMC.remove(SPEAKERS_INDEX);
    SD_MMC.rename(tmp, SPEAKERS_INDEX);
  }
}

void speakersEnsureDir() {
  if (!SD_MMC.exists(SPEAKERS_DIR)) SD_MMC.mkdir(SPEAKERS_DIR);
}

void speakersLoad() {
  gCount = 0;
  gNextId = 1;
  speakersEnsureDir();
  File f = SD_MMC.open(SPEAKERS_INDEX);
  if (!f) return;
  while (f.available() && gCount < MAX_SPEAKERS) {
    String ln = f.readStringUntil('\n'); ln.trim();
    if (!ln.length()) continue;
    int c = ln.indexOf(',');
    if (c <= 0) continue;
    int id = ln.substring(0, c).toInt();
    String name = ln.substring(c + 1); name.trim();
    if (id <= 0 || name.length() == 0) continue;
    gSpeakers[gCount].id = id;
    strncpy(gSpeakers[gCount].name, name.c_str(), 31);
    gSpeakers[gCount].name[31] = 0;
    if (id >= gNextId) gNextId = id + 1;
    gCount++;
  }
  f.close();
}

int speakersCount() { return gCount; }

const Speaker* speakersAt(int index) {
  if (index < 0 || index >= gCount) return nullptr;
  return &gSpeakers[index];
}

const Speaker* speakersFindId(int id) {
  for (int i = 0; i < gCount; i++)
    if (gSpeakers[i].id == id) return &gSpeakers[i];
  return nullptr;
}

String speakersWavPath(int id) {
  char p[48];
  snprintf(p, sizeof(p), "%s/%d.wav", SPEAKERS_DIR, id);
  return String(p);
}

int speakersAdd(const char* name, const char* srcWavPath) {
  if (!name || !srcWavPath || gCount >= MAX_SPEAKERS) return -1;
  String n = name; n.trim();
  if (n.length() == 0 || n.length() > 31) return -1;
  if (!SD_MMC.exists(srcWavPath)) return -1;

  speakersEnsureDir();
  int id = gNextId++;
  String dst = speakersWavPath(id);
  if (SD_MMC.exists(dst.c_str())) SD_MMC.remove(dst.c_str());

  // Copy then truncate to the API's preferred 2–5 s window (keep from start).
  File src = SD_MMC.open(srcWavPath);
  if (!src) return -1;
  File out = SD_MMC.open(dst.c_str(), FILE_WRITE);
  if (!out) { src.close(); return -1; }
  uint8_t buf[2048];
  while (src.available()) {
    int got = src.read(buf, sizeof(buf));
    if (got <= 0) break;
    out.write(buf, (size_t)got);
  }
  src.close();
  out.close();

  uint32_t maxPcm = (uint32_t)((SPEAKER_REF_MAX_MS / 1000.0f) * SAMPLE_RATE * 2);
  if (!truncateWavPcm(dst.c_str(), maxPcm)) {
    SD_MMC.remove(dst.c_str());
    return -1;
  }
  uint32_t minPcm = (uint32_t)((SPEAKER_REF_MIN_MS / 1000.0f) * SAMPLE_RATE * 2);
  if (wavPcmBytes(dst.c_str()) < minPcm) {
    SD_MMC.remove(dst.c_str());
    return -1;
  }

  gSpeakers[gCount].id = id;
  strncpy(gSpeakers[gCount].name, n.c_str(), 31);
  gSpeakers[gCount].name[31] = 0;
  gCount++;
  saveIndex();
  return id;
}

bool speakersRename(int id, const char* name) {
  Speaker* sp = nullptr;
  for (int i = 0; i < gCount; i++)
    if (gSpeakers[i].id == id) { sp = &gSpeakers[i]; break; }
  if (!sp || !name) return false;
  String n = name; n.trim();
  if (n.length() == 0 || n.length() > 31) return false;
  strncpy(sp->name, n.c_str(), 31);
  sp->name[31] = 0;
  saveIndex();
  return true;
}

bool speakersDelete(int id) {
  int idx = -1;
  for (int i = 0; i < gCount; i++)
    if (gSpeakers[i].id == id) { idx = i; break; }
  if (idx < 0) return false;
  String path = speakersWavPath(id);
  if (SD_MMC.exists(path.c_str())) SD_MMC.remove(path.c_str());
  for (int i = idx; i < gCount - 1; i++) gSpeakers[i] = gSpeakers[i + 1];
  gCount--;
  saveIndex();
  return true;
}

int speakersPickForDiarize(Speaker* out, int maxOut) {
  if (!out || maxOut <= 0) return 0;
  int n = maxOut < MAX_KNOWN_SPEAKER_REFS ? maxOut : MAX_KNOWN_SPEAKER_REFS;
  if (n > gCount) n = gCount;
  for (int i = 0; i < n; i++) out[i] = gSpeakers[i];
  return n;
}
