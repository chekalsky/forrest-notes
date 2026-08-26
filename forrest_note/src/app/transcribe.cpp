#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "transcribe.h"
#include "https.h"
#include "notes.h"
#include "ui.h"
#include "config_store.h"
#include "speakers.h"
#include "wav_util.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include <ArduinoJson.h>
#include "SD_MMC.h"
#include "esp_heap_caps.h"

// OpenAI diarize of pending WAVs. Control flow: see ARCHITECTURE.md "Sync pipeline".

static const char B64_TAB[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64EncodedLen(size_t n) { return 4 * ((n + 2) / 3); }

static bool streamBase64File(WiFiClientSecure& client, File& f) {
  uint8_t in[3];
  while (f.available()) {
    int n = 0;
    while (n < 3 && f.available()) {
      int c = f.read();
      if (c < 0) break;
      in[n++] = (uint8_t)c;
    }
    if (n == 0) break;
    char out[4];
    out[0] = B64_TAB[in[0] >> 2];
    out[1] = B64_TAB[((in[0] & 0x03) << 4) | (n > 1 ? (in[1] >> 4) : 0)];
    out[2] = (n > 1) ? B64_TAB[((in[1] & 0x0F) << 2) | (n > 2 ? (in[2] >> 6) : 0)] : '=';
    out[3] = (n > 2) ? B64_TAB[in[2] & 0x3F] : '=';
    if (client.write((uint8_t*)out, 4) != 4) return false;
  }
  return true;
}

static String formatTs(float sec) {
  int s = (int)sec;
  if (s < 0) s = 0;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  return String(buf);
}

// Turn diarized_json into a readable speaker script. timeOffsetSec shifts chunk-local times.
static String diarizedJsonToScript(const String& json, float timeOffsetSec) {
  DynamicJsonDocument doc(json.length() + 4096);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[Diarize] json: %s\n", err.c_str());
    return "";
  }

  String out;
  JsonArray segs = doc["segments"].as<JsonArray>();
  if (segs.isNull() || segs.size() == 0) {
    // Fallback: flat text field
    String text = doc["text"] | "";
    text.trim();
    return text;
  }

  String curSpeaker = "";
  for (JsonObject seg : segs) {
    String sp = seg["speaker"] | "A";
    String tx = seg["text"] | "";
    tx.trim();
    if (!tx.length()) continue;
    float st = seg["start"] | 0.0f;
    st += timeOffsetSec;
    if (sp != curSpeaker) {
      if (out.length()) out += "\n\n";
      out += "**" + sp + "** [" + formatTs(st) + "]\n";
      curSpeaker = sp;
    } else {
      out += " ";
    }
    out += tx;
  }
  out.trim();
  return out;
}

// Forward decls — definitions below; used inside diarizeFileOnce for live UI.
static void setDiarizeProcess(const char* process);

// Diarize one WAV file (already ≤ DIARIZE_MAX_UPLOAD). Returns speaker script or "".
static String diarizeFileOnce(const String& wavPath, float timeOffsetSec) {
  String oaiKey = cfg::openaiKey();
  if (oaiKey.length() == 0) { Serial.println("[Diarize] no API key"); return ""; }

  File f = SD_MMC.open(wavPath.c_str());
  if (!f) return "";
  size_t fileSize = f.size();
  f.close();
  if (fileSize == 0 || fileSize > DIARIZE_MAX_UPLOAD) {
    Serial.printf("[Diarize] bad size %u\n", (unsigned)fileSize);
    return "";
  }

  speakersLoad();
  Speaker refs[MAX_KNOWN_SPEAKER_REFS];
  int nRefs = speakersPickForDiarize(refs, MAX_KNOWN_SPEAKER_REFS);

  // Precompute reference file sizes for Content-Length.
  size_t refRaw[MAX_KNOWN_SPEAKER_REFS] = {};
  size_t refB64[MAX_KNOWN_SPEAKER_REFS] = {};
  const char* dataUrlPrefix = "data:audio/wav;base64,";
  size_t prefixLen = strlen(dataUrlPrefix);
  for (int i = 0; i < nRefs; i++) {
    String rp = speakersWavPath(refs[i].id);
    File rf = SD_MMC.open(rp.c_str());
    if (!rf) { nRefs = i; break; }
    refRaw[i] = rf.size();
    rf.close();
    refB64[i] = prefixLen + base64EncodedLen(refRaw[i]);
  }

  String bnd = "----ForrestDiarize";
  String pre = "--" + bnd + "\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
               "gpt-4o-transcribe-diarize\r\n"
               "--" + bnd + "\r\n"
               "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
               "diarized_json\r\n"
               "--" + bnd + "\r\n"
               "Content-Disposition: form-data; name=\"chunking_strategy\"\r\n\r\n"
               "auto\r\n";

  size_t midFixed = 0;
  for (int i = 0; i < nRefs; i++) {
    String namePart = "--" + bnd + "\r\n"
      "Content-Disposition: form-data; name=\"known_speaker_names[]\"\r\n\r\n"
      + String(refs[i].name) + "\r\n";
    String refHead = "--" + bnd + "\r\n"
      "Content-Disposition: form-data; name=\"known_speaker_references[]\"\r\n\r\n";
    midFixed += namePart.length() + refHead.length() + refB64[i] + 2; // \r\n after b64
  }

  String fileHead = "--" + bnd + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"note.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String post = "\r\n--" + bnd + "--\r\n";

  size_t totalLen = pre.length() + midFixed + fileHead.length() + fileSize + post.length();

  WiFiClientSecure client;
  httpsAttachCa(client);
  client.setHandshakeTimeout(30);

  if (!client.connect("api.openai.com", 443, 20000)) {
    Serial.println("[Diarize] connect failed");
    return "";
  }

  setDiarizeProcess("uploading");
  client.printf("POST /v1/audio/transcriptions HTTP/1.1\r\n"
                "Host: api.openai.com\r\n"
                "Authorization: Bearer %s\r\n"
                "Content-Type: multipart/form-data; boundary=%s\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                oaiKey.c_str(), bnd.c_str(), (unsigned)totalLen);
  client.print(pre);

  for (int i = 0; i < nRefs; i++) {
    client.print("--" + bnd + "\r\n"
                 "Content-Disposition: form-data; name=\"known_speaker_names[]\"\r\n\r\n");
    client.print(refs[i].name);
    client.print("\r\n");
    client.print("--" + bnd + "\r\n"
                 "Content-Disposition: form-data; name=\"known_speaker_references[]\"\r\n\r\n");
    client.print(dataUrlPrefix);
    File rf = SD_MMC.open(speakersWavPath(refs[i].id).c_str());
    if (!rf || !streamBase64File(client, rf)) {
      if (rf) rf.close();
      client.stop();
      Serial.println("[Diarize] ref stream failed");
      return "";
    }
    rf.close();
    client.print("\r\n");
    syncProgressPulse();
  }

  client.print(fileHead);
  File audio = SD_MMC.open(wavPath.c_str());
  if (!audio) { client.stop(); return ""; }
  uint8_t* chunk = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
  if (!chunk) { audio.close(); client.stop(); return ""; }
  while (audio.available()) {
    int n = audio.read(chunk, 4096);
    if (n <= 0) break;
    if (client.write(chunk, n) != (size_t)n) {
      heap_caps_free(chunk);
      audio.close();
      client.stop();
      Serial.println("[Diarize] upload stalled");
      return "";
    }
    syncProgressPulse();
  }
  heap_caps_free(chunk);
  audio.close();
  client.print(post);

  setDiarizeProcess("diarizing");

  // Stream response body to SD to keep RAM flat.
  char respPath[] = "/notes/_diar_resp.json";
  if (SD_MMC.exists(respPath)) SD_MMC.remove(respPath);
  File respF = SD_MMC.open(respPath, FILE_WRITE);
  if (!respF) { client.stop(); return ""; }

  uint32_t deadline = millis() + DIARIZE_HTTP_TIMEOUT_MS;
  while (!client.available() && client.connected() && millis() < deadline) {
    syncProgressPulse();
    delay(20);
  }

  bool inBody = false, chunked = false;
  int httpStatus = 0;
  String headerBuf;
  while (client.available() || (client.connected() && millis() < deadline)) {
    if (!client.available()) {
      syncProgressPulse();
      delay(10);
      continue;
    }
    if (!inBody) {
      String line = client.readStringUntil('\n');
      headerBuf += line;
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) httpStatus = line.substring(sp + 1).toInt();
      }
      String low = line; low.toLowerCase();
      if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) chunked = true;
      if (line == "\r" || line == "") inBody = true;
    } else {
      uint8_t buf[512];
      int n = client.read(buf, sizeof(buf));
      if (n > 0) respF.write(buf, n);
      syncProgressPulse();
    }
  }
  client.stop();
  respF.close();

  setDiarizeProcess("parsing");

  if (httpStatus != 200) {
    Serial.printf("[Diarize] HTTP %d\n", httpStatus);
    File errF = SD_MMC.open(respPath);
    if (errF) {
      String snip;
      while (errF.available() && snip.length() < 240) snip += (char)errF.read();
      errF.close();
      Serial.printf("[Diarize] body: %s\n", snip.c_str());
    }
    SD_MMC.remove(respPath);
    return "";
  }

  File jf = SD_MMC.open(respPath);
  if (!jf) return "";
  String raw;
  const size_t kMaxJson = 400000;
  while (jf.available() && raw.length() < kMaxJson) raw += (char)jf.read();
  jf.close();
  SD_MMC.remove(respPath);

  String json = chunked ? dechunkBody(raw) : raw;

  String script = diarizedJsonToScript(json, timeOffsetSec);
  if (!script.length()) Serial.println("[Diarize] empty script");
  return script;
}

// Context so diarizeFileOnce can update process text + pulse the pizza loader mid-request.
static int gDzNote = 0, gDzDone = 0, gDzTotal = 1, gDzStep = 0, gDzSteps = 1;
static char gDzChunk[36] = {};

static int syncPct(int notesDone, int notesTotal, int step, int steps) {
  if (notesTotal <= 0) return 0;
  if (steps < 1) steps = 1;
  if (step < 0) step = 0;
  if (step > steps) step = steps;
  int num = notesDone * steps + step;
  int den = notesTotal * steps;
  return (num * 100) / den;
}

static void showDiarizeProgress(int noteNum, int notesDone, int notesTotal,
                                int step, int steps,
                                const char* chunkLine, const char* process) {
  gDzNote = noteNum;
  gDzDone = notesDone;
  gDzTotal = notesTotal;
  gDzStep = step;
  gDzSteps = steps < 1 ? 1 : steps;
  if (chunkLine && chunkLine[0]) {
    strncpy(gDzChunk, chunkLine, sizeof(gDzChunk) - 1);
    gDzChunk[sizeof(gDzChunk) - 1] = 0;
  } else {
    gDzChunk[0] = 0;
  }
  char detail[32];
  snprintf(detail, sizeof(detail), "note #%03d", noteNum);
  showSyncProgress("diarize", syncPct(notesDone, notesTotal, step, gDzSteps),
                   detail, gDzChunk[0] ? gDzChunk : nullptr, process,
                   notesDone, notesTotal);
}

static void setDiarizeProcess(const char* process) {
  showDiarizeProgress(gDzNote, gDzDone, gDzTotal, gDzStep, gDzSteps,
                      gDzChunk[0] ? gDzChunk : nullptr, process);
}

static bool transcribeOnce(const String& wavPath, int noteNum,
                           int notesDone, int notesTotal) {
  uint32_t pcm = wavPcmBytes(wavPath.c_str());
  if (pcm == 0) { Serial.println("[Diarize] empty/missing wav"); return false; }

  const uint32_t chunkPcm = (uint32_t)((DIARIZE_CHUNK_MS / 1000.0f) * SAMPLE_RATE * 2);
  size_t wholeSize = (size_t)pcm + 44;
  String script;

  if (wholeSize <= DIARIZE_MAX_UPLOAD) {
    Serial.printf("[Diarize] single shot note_%03d (%u bytes)\n", noteNum, (unsigned)wholeSize);
    showDiarizeProgress(noteNum, notesDone, notesTotal, 0, 2, nullptr, "uploading");
    script = diarizeFileOnce(wavPath, 0.0f);
    if (script.length())
      showDiarizeProgress(noteNum, notesDone, notesTotal, 1, 2, nullptr, "saving transcript");
  } else {
    int nChunks = (int)((pcm + chunkPcm - 1) / chunkPcm);
    if (nChunks < 1) nChunks = 1;
    Serial.printf("[Diarize] splitting note_%03d (%u pcm bytes, %d chunks)\n",
                  noteNum, (unsigned)pcm, nChunks);
    uint32_t offset = 0;
    int part = 0;
    while (offset < pcm) {
      if (WiFi.status() != WL_CONNECTED) return false;
      uint32_t n = chunkPcm;
      if (offset + n > pcm) n = pcm - offset;

      char chunkLine[36];
      snprintf(chunkLine, sizeof(chunkLine), "chunk %d of %d", part + 1, nChunks);
      showDiarizeProgress(noteNum, notesDone, notesTotal, part, nChunks, chunkLine, "preparing");

      char chunkPath[64];
      snprintf(chunkPath, sizeof(chunkPath), "%s/note_%03d_c%d.wav", NOTES_DIR, noteNum, part);
      if (!extractWavPcmChunk(wavPath.c_str(), chunkPath, offset, n)) {
        Serial.printf("[Diarize] chunk extract failed @%u\n", (unsigned)offset);
        return false;
      }
      float tOff = (float)offset / (float)(SAMPLE_RATE * 2);
      Serial.printf("[Diarize] chunk %d offset=%.1fs bytes=%u\n", part, tOff, (unsigned)(n + 44));
      String piece = diarizeFileOnce(String(chunkPath), tOff);
      SD_MMC.remove(chunkPath);
      if (!piece.length()) {
        Serial.printf("[Diarize] chunk %d failed\n", part);
        return false;
      }
      if (script.length()) script += "\n\n";
      script += piece;
      offset += n;
      part++;
    }
    showDiarizeProgress(noteNum, notesDone, notesTotal, nChunks, nChunks, nullptr, "saving transcript");
  }

  if (!script.length()) return false;

  String tp = wavPath; tp.replace(".wav", ".txt");
  File tf = SD_MMC.open(tp.c_str(), FILE_WRITE);
  if (!tf) return false;
  tf.print(script);
  tf.close();

  updateIndexHasText(noteNum);
  return true;
}

bool transcribe(const String& wavPath, int noteNum, int notesDone, int notesTotal) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (attempt > 0) {
      char sub[28];
      snprintf(sub, sizeof(sub), "retry %d of 2", attempt);
      showDiarizeProgress(noteNum, notesDone, notesTotal, 0, 1, nullptr, sub);
      delay(3000);
    }
    if (transcribeOnce(wavPath, noteNum, notesDone, notesTotal)) return true;
  }
  return false;
}

bool transcribe(const String& wavPath, int noteNum) {
  return transcribe(wavPath, noteNum, 0, 1);
}

void transcribeAll() {
  if (!cfg::hasOpenAiKey()) { Serial.println("[Diarize] no API key; skipping sync"); return; }
  speakersLoad();

  int pending = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) if (!noteIndex[i].hasText) pending++;
  if (pending == 0) {
    showSyncProgress("diarize", 100, "nothing pending", nullptr, "all notes have text", 0, 0);
    delay(600);
    return;
  }

  int done = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (noteIndex[i].hasText) continue;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[Diarize] wifi lost; %d note(s) stay pending\n", pending - done);
      break;
    }
    int num = noteIndex[i].num;
    showDiarizeProgress(num, done, pending, 0, 1, nullptr, "preparing");
    char wp[64]; snprintf(wp, sizeof(wp), "%s/note_%03d.wav", NOTES_DIR, num);
    if (transcribe(String(wp), num, done, pending)) done++;
  }
  showSyncProgress("diarize", pending > 0 ? (done * 100) / pending : 100,
                   done == pending ? "diarize done" : "partial",
                   nullptr, nullptr, done, pending);
  Serial.printf("[Diarize] synced %d/%d pending\n", done, pending);
}

