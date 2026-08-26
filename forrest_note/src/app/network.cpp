#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "network.h"
#include "notes.h"
#include "rtc.h"
#include "ui.h"
#include "config_store.h"
#include "speakers.h"
#include "wav_util.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include <WebServer.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "../../secrets.h"

// IDF built-in Mozilla CA root bundle (libmbedtls.a). Auto-maintained with the
// esp32 core, so server certs validate without shipping/rotating a pinned PEM.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

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
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
  client.setHandshakeTimeout(30);

  if (!client.connect("api.openai.com", 443, 20000)) {
    Serial.println("[Diarize] connect failed");
    return "";
  }

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
  }
  heap_caps_free(chunk);
  audio.close();
  client.print(post);

  // Stream response body to SD to keep RAM flat.
  char respPath[] = "/notes/_diar_resp.json";
  if (SD_MMC.exists(respPath)) SD_MMC.remove(respPath);
  File respF = SD_MMC.open(respPath, FILE_WRITE);
  if (!respF) { client.stop(); return ""; }

  uint32_t deadline = millis() + DIARIZE_HTTP_TIMEOUT_MS;
  while (!client.available() && client.connected() && millis() < deadline) delay(20);

  bool inBody = false, chunked = false;
  int httpStatus = 0;
  String headerBuf;
  while (client.available() || (client.connected() && millis() < deadline)) {
    if (!client.available()) { delay(10); continue; }
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
    }
  }
  client.stop();
  respF.close();

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

  String json = raw;
  if (chunked) {
    // Inline dechunk (same algorithm as obsidian.cpp).
    String out; int i = 0, n = raw.length();
    while (i < n) {
      int eol = raw.indexOf('\n', i);
      if (eol < 0) break;
      String sizeLine = raw.substring(i, eol);
      int semi = sizeLine.indexOf(';');
      if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
      sizeLine.trim();
      long sz = strtol(sizeLine.c_str(), nullptr, 16);
      i = eol + 1;
      if (sz <= 0) break;
      if (i + sz > n) sz = n - i;
      out += raw.substring(i, i + (int)sz);
      i += (int)sz;
      while (i < n && (raw[i] == '\r' || raw[i] == '\n')) i++;
    }
    json = out;
  }

  String script = diarizedJsonToScript(json, timeOffsetSec);
  if (!script.length()) Serial.println("[Diarize] empty script");
  return script;
}

static bool transcribeOnce(const String& wavPath, int noteNum) {
  uint32_t pcm = wavPcmBytes(wavPath.c_str());
  if (pcm == 0) { Serial.println("[Diarize] empty/missing wav"); return false; }

  const uint32_t chunkPcm = (uint32_t)((DIARIZE_CHUNK_MS / 1000.0f) * SAMPLE_RATE * 2);
  size_t wholeSize = (size_t)pcm + 44;
  String script;

  if (wholeSize <= DIARIZE_MAX_UPLOAD) {
    Serial.printf("[Diarize] single shot note_%03d (%u bytes)\n", noteNum, (unsigned)wholeSize);
    script = diarizeFileOnce(wavPath, 0.0f);
  } else {
    Serial.printf("[Diarize] splitting note_%03d (%u pcm bytes)\n", noteNum, (unsigned)pcm);
    uint32_t offset = 0;
    int part = 0;
    while (offset < pcm) {
      if (WiFi.status() != WL_CONNECTED) return false;
      uint32_t n = chunkPcm;
      if (offset + n > pcm) n = pcm - offset;

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

bool transcribe(const String& wavPath, int noteNum) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (transcribeOnce(wavPath, noteNum)) return true;
    if (attempt < 2) { Serial.printf("[Diarize] retry %d/2\n", attempt + 1); delay(3000); }
  }
  return false;
}

void transcribeAll() {
  if (!cfg::hasOpenAiKey()) { Serial.println("[Diarize] no API key; skipping sync"); return; }
  speakersLoad();

  int pending = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) if (!noteIndex[i].hasText) pending++;
  int done = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (noteIndex[i].hasText) continue;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[Diarize] wifi lost; %d note(s) stay pending\n", pending - done);
      break;
    }
    showTranscribing(done, pending);
    char wp[64]; snprintf(wp, sizeof(wp), "%s/note_%03d.wav", NOTES_DIR, noteIndex[i].num);
    if (transcribe(String(wp), noteIndex[i].num)) done++;
  }
  Serial.printf("[Diarize] synced %d/%d pending\n", done, pending);
}

// ─── Portal helpers ────────────────────────────────────────────────────────

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;"); out.replace("<", "&lt;");
  out.replace(">", "&gt;"); out.replace("\"", "&quot;");
  return out;
}

String readSmallFile(const char* path, size_t maxLen) {
  File f = SD_MMC.open(path);
  if (!f) return "";
  String out;
  while (f.available() && out.length() < maxLen) out += (char)f.read();
  f.close();
  return out;
}

String urlDecodeSimple(String s) {
  s.replace("+", " ");
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == '%' && i + 2 < (int)s.length()) {
      String hex = s.substring(i + 1, i + 3);
      out += (char)strtol(hex.c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

String portalCss() {
  return String(
    "<style>"
    ":root{font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',sans-serif;color:#111;background:#f3f0e9;}"
    "body{margin:0;padding:24px;background:#f3f0e9;}"
    ".wrap{max-width:780px;margin:0 auto;}"
    ".top{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;margin-bottom:24px;}"
    "h1{font-size:44px;letter-spacing:-.06em;line-height:.9;margin:0;font-weight:800;}"
    ".sub{font-size:13px;text-transform:uppercase;letter-spacing:.12em;color:#6a665f;margin-top:10px;}"
    ".pill{display:inline-flex;border:1px solid #111;border-radius:999px;padding:8px 12px;font-size:13px;background:#fffaf1;}"
    ".grid{display:grid;grid-template-columns:1fr;gap:14px;}"
    ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;box-shadow:4px 4px 0 #111;}"
    ".row{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;}"
    ".num{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:#6a665f;margin-bottom:8px;}"
    ".date{font-size:13px;color:#6a665f;margin:-4px 0 12px;}"
    ".title{font-size:24px;line-height:1.05;letter-spacing:-.04em;font-weight:750;margin:0 0 12px;}"
    ".tag{border:1px solid #111;border-radius:999px;padding:5px 9px;font-size:12px;white-space:nowrap;background:#111;color:#fff;}"
    ".text{font-size:15px;line-height:1.45;color:#222;margin:0 0 14px;white-space:pre-wrap;}"
    ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;}"
    "a.btn{color:#111;text-decoration:none;border:1px solid #111;border-radius:999px;padding:8px 12px;background:#f3f0e9;font-size:13px;}"
    "a.btn.primary{background:#111;color:#fff;}"
    ".empty{border:1.5px dashed #111;border-radius:24px;padding:34px;text-align:center;color:#6a665f;}"
    "audio{width:100%;margin-top:8px;}"
    "@media(max-width:520px){body{padding:16px}h1{font-size:36px}.card{border-radius:20px}.title{font-size:21px}}"
    "</style>"
  );
}

// ─── Portal handlers ───────────────────────────────────────────────────────

void handlePortalRoot() {
  loadIndex();

  Serial.println("[HTTP] GET /");
  String filter = "All";
  if (transferServer.hasArg("tag")) filter = transferServer.arg("tag");

  // Stream the page in bounded chunks so RAM use stays flat regardless of how
  // many notes exist (a single accumulated String would grow unboundedly).
  transferServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  transferServer.send(200, "text/html", "");

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Forrest Portal</title>" + portalCss() + "</head><body><div class='wrap'>";

  html += "<div class='top'><div><h1>forrest<br>portal</h1>"
          "<div class='sub'>local note transfer · <a href=\"/tags\" style=\"color:inherit\">tags</a> · <a href=\"/speakers\" style=\"color:inherit\">speakers</a> · <a href=\"/provision\" style=\"color:inherit\">setup</a> · <a href=\"/ota\" style=\"color:inherit\">update</a></div></div>"
          "<div class='pill'>" + String((int)noteIndex.size()) + " notes</div></div>";

  html += "<div class='actions' style='margin-bottom:18px'>";
  html += "<a class='btn " + String(filter == "All" ? "primary" : "") + "' href='/'>All</a>";
  for (int t = 0; t < tagCount; t++) {
    String tag = String(tags[t]);
    html += "<a class='btn " + String(filter == tag ? "primary" : "") + "' href='/?tag=" + tag + "'>" + htmlEscape(tag) + "</a>";
  }
  html += "</div>";

  html += "<div class='actions' style='margin-bottom:24px'>";
  html += "<a class='btn primary' href='/export.txt'>Download all TXT</a>";
  if (filter != "All")
    html += "<a class='btn' href='/export.txt?tag=" + filter + "'>Download " + htmlEscape(filter) + " TXT</a>";
  html += "</div>";

  int visibleCount = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    if (filter == "All" || filter == String(noteIndex[i].tag)) visibleCount++;

  if (visibleCount <= 0) {
    html += "<div class='empty'>No notes for this filter.</div>";
  } else {
    html += "<div class='grid'>";
    for (int v = 0; v < (int)noteIndex.size(); v++) {
      int i = (int)noteIndex.size() - 1 - v;
      if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
      int num = noteIndex[i].num;

      char txtPath[64], wavPath[64];
      snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
      snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, num);

      String transcript = readSmallFile(txtPath, 1200);
      if (transcript.length() == 0)
        transcript = noteIndex[i].hasText ? "(empty transcript)" : "Not transcribed yet.";

      String title = transcript; title.replace("\n", " "); title.trim();
      if (title.length() > 58) title = title.substring(0, 58) + "...";
      if (title.length() == 0 || title == "Not transcribed yet.")
        title = String("Voice note ") + String(num);

      html += "<div class='card'>";
      html += "<div class='row'><div><div class='num'>#" + String(num) + "</div>";
      html += "<h2 class='title'>" + htmlEscape(title) + "</h2>";
      String createdUtc = noteCreatedUtc(num);
      if (createdUtc.length() > 0)
        html += "<div class='date' data-utc='" + createdUtc + "'>" + createdUtc + "</div>";
      else
        html += "<div class='date'>time not set</div>";
      html += "</div>";
      html += "<div class='tag'>" + htmlEscape(String(noteIndex[i].tag)) + "</div></div>";
      html += "<p class='text'>" + htmlEscape(transcript) + "</p>";
      if (SD_MMC.exists(wavPath))
        html += "<audio controls src='/audio?num=" + String(num) + "'></audio>";
      html += "<div class='actions'>";
      html += "<a class='btn primary' href='/txt?num=" + String(num) + "'>Download TXT</a>";
      if (SD_MMC.exists(wavPath))
        html += "<a class='btn' href='/wav?num=" + String(num) + "'>Download WAV</a>";
      html += "<a class='btn' style='margin-left:auto;color:#c0392b;border-color:#c0392b' "
              "href='/note/delete?num=" + String(num) + "' "
              "onclick=\"return confirm('Delete note #" + String(num) + "? This cannot be undone.')\">Delete</a>";
      html += "</div></div>";
      if (html.length() > 2048) { transferServer.sendContent(html); html = ""; }
    }
    html += "</div>";
  }

  html += "<script>"
          "document.querySelectorAll('[data-utc]').forEach(function(el){"
          "var d=new Date(el.dataset.utc);"
          "if(!isNaN(d)){el.textContent=d.toLocaleString([],{year:'numeric',month:'short',day:'2-digit',hour:'2-digit',minute:'2-digit'});}"
          "});"
          "</script>";
  html += "</div></body></html>";
  transferServer.sendContent(html);
  transferServer.sendContent("");   // terminate chunked response
}

void handlePortalJson() {
  loadIndex();
  String json = "[";
  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (v > 0) json += ",";
    json += "{";
    json += "\"num\":" + String(noteIndex[i].num) + ",";
    json += "\"tag\":\"" + String(noteIndex[i].tag) + "\",";
    json += "\"hasText\":" + String(noteIndex[i].hasText ? "true" : "false");
    json += "}";
  }
  json += "]";
  transferServer.send(200, "application/json", json);
}

void handleExportTxt() {
  loadIndex();
  String filter = "All";
  if (transferServer.hasArg("tag")) filter = transferServer.arg("tag");

  String filename = "forrest_notes_export";
  if (filter != "All") filename += "_" + filter;
  filename += ".txt";

  // Stream chunked so the full export never has to fit in RAM at once (the old
  // path capped at 55 KB and truncated). No cap now — all notes are exported.
  transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  transferServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  transferServer.send(200, "text/plain", "");

  String chunk = "Forrest Note Export\nFilter: " + filter + "\n------------------------------\n\n";

  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (!(filter == "All" || filter == String(noteIndex[i].tag))) continue;
    int num = noteIndex[i].num;
    char txtPath[64]; snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
    String transcript = readSmallFile(txtPath, 4000);
    if (transcript.length() == 0)
      transcript = noteIndex[i].hasText ? "(empty transcript)" : "Not transcribed yet.";
    chunk += "#";
    if (num < 100) chunk += "0";
    if (num < 10)  chunk += "0";
    chunk += String(num) + " · " + String(noteIndex[i].tag) + "\n";
    String createdUtc = noteCreatedUtc(num);
    if (createdUtc.length() > 0) chunk += createdUtc + "\n";
    chunk += "\n" + transcript + "\n\n------------------------------\n\n";
    if (chunk.length() > 2048) { transferServer.sendContent(chunk); chunk = ""; }
  }

  transferServer.sendContent(chunk);
  transferServer.sendContent("");   // terminate chunked response
}

void sendFileByNum(const char* ext, const char* mime, bool attachment) {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, ext);
  File f = SD_MMC.open(path);
  if (!f) { transferServer.send(404, "text/plain", "File not found"); return; }
  if (attachment) {
    String filename = String("note_") + String(num) + "." + String(ext);
    transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  }
  transferServer.streamFile(f, mime);
  f.close();
}

void handleTagAdd() {
  if (!transferServer.hasArg("name")) {
    transferServer.sendHeader("Location", "/tags?msg=missing");
    transferServer.send(303); return;
  }
  String name = urlDecodeSimple(transferServer.arg("name"));
  bool ok = addCustomTag(name.c_str());
  transferServer.sendHeader("Location", ok ? "/tags?msg=added" : "/tags?msg=exists");
  transferServer.send(303);
}

void handleTagDelete() {
  if (!transferServer.hasArg("name")) {
    transferServer.sendHeader("Location", "/tags?msg=missing");
    transferServer.send(303); return;
  }
  String name = urlDecodeSimple(transferServer.arg("name"));
  bool hadNotes = tagHasNotes(name.c_str());
  bool ok = deleteTag(name.c_str());
  if (ok && hadNotes) transferServer.sendHeader("Location", "/tags?msg=moved");
  else                transferServer.sendHeader("Location", ok ? "/tags?msg=deleted" : "/tags?msg=protected");
  transferServer.send(303);
}

void handleTagsPage() {
  loadTags();
  loadIndex();
  activeFilter = -1;

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Forrest Tags</title>"
                "<style>"
                "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:0;padding:24px;background:#f3f0e9;color:#111}"
                ".wrap{max-width:720px;margin:0 auto}"
                "h1{font-size:42px;line-height:.9;letter-spacing:-.05em;margin:0 0 22px;font-weight:800}"
                ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;margin:14px 0;box-shadow:4px 4px 0 #111}"
                ".row{display:flex;justify-content:space-between;align-items:center;gap:12px;border-top:1px solid #ddd;padding:12px 0}"
                ".row:first-child{border-top:0}"
                ".tag{font-size:20px;font-weight:700}"
                ".meta{font-size:13px;color:#666;margin-top:4px}"
                "input{font:inherit;padding:12px;border:1.5px solid #111;border-radius:999px;background:#fff;width:100%;box-sizing:border-box}"
                "button,.btn{font:inherit;border:1.5px solid #111;border-radius:999px;padding:10px 14px;background:#111;color:#fff;text-decoration:none;white-space:nowrap}"
                ".danger{background:#fffaf1;color:#111}"
                ".msg{border:1.5px solid #111;border-radius:18px;padding:12px 14px;background:#fff;margin:12px 0}"
                ".hint{font-size:13px;color:#666;line-height:1.4}"
                "form.add{display:flex;gap:10px}"
                "</style></head><body><div class='wrap'>";

  html += "<h1>forrest<br>tags</h1>";
  html += "<a class='btn' href='/'>Back to notes</a>";

  if (transferServer.hasArg("msg")) {
    String msg = transferServer.arg("msg");
    html += "<div class='msg'>";
    if (msg == "added") html += "Tag added.";
    else if (msg == "exists")    html += "Tag already exists or cannot be added.";
    else if (msg == "deleted")   html += "Tag deleted.";
    else if (msg == "moved")     html += "Tag deleted. Existing notes were moved to Untagged.";
    else if (msg == "protected") html += "This tag cannot be deleted.";
    else html += "Please enter a tag name.";
    html += "</div>";
  }

  html += "<div class='card'><form class='add' action='/tag/add' method='get'>"
          "<input name='name' maxlength='31' placeholder='New tag name'>"
          "<button type='submit'>Add</button></form>"
          "<p class='hint'>Tags appear on the device after recording. Keep them short for the e-paper UI.</p></div>";

  html += "<div class='card'>";
  for (int i = 0; i < tagCount; i++) {
    int cnt = 0;
    for (int n = 0; n < (int)noteIndex.size(); n++)
      if (strcmp(noteIndex[n].tag, tags[i]) == 0) cnt++;
    html += "<div class='row'><div><div class='tag'>" + htmlEscape(String(tags[i])) + "</div>";
    html += "<div class='meta'>" + String(cnt) + (cnt == 1 ? " note" : " notes");
    if (cnt > 0) html += " · deleting moves them to Untagged";
    html += "</div></div>";
    if (strcasecmp(tags[i], "Untagged") != 0) {
      html += "<a class='btn danger' href='/tag/delete?name=" + htmlEscape(String(tags[i])) + "' "
              "onclick=\"return confirm('Delete this tag? Notes will not be deleted. Existing notes will move to Untagged.');\">Delete</a>";
    }
    html += "</div>";
  }
  html += "</div></div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleNoteDelete() {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  deleteNote(num);
  transferServer.sendHeader("Location", "/");
  transferServer.send(303);
}

// ─── Speakers library portal ───────────────────────────────────────────────

static File gSpeakerUploadFile;
static char gSpeakerUploadPath[64];
static bool gSpeakerUploadOk = false;

void handleSpeakersPage() {
  speakersLoad();
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Forrest Speakers</title>" + portalCss() +
                "<style>"
                ".row{display:flex;justify-content:space-between;align-items:center;gap:12px;border-top:1px solid #ddd;padding:12px 0}"
                ".row:first-child{border-top:0}"
                "input{font:inherit;padding:12px;border:1.5px solid #111;border-radius:999px;background:#fff;width:100%;box-sizing:border-box}"
                "button,.btn{font:inherit;border:1.5px solid #111;border-radius:999px;padding:10px 14px;background:#111;color:#fff;text-decoration:none;white-space:nowrap}"
                ".danger{background:#fffaf1;color:#111}"
                ".msg{border:1.5px solid #111;border-radius:18px;padding:12px 14px;background:#fff;margin:12px 0}"
                ".hint{font-size:13px;color:#666;line-height:1.4}"
                "form.add{display:flex;flex-direction:column;gap:10px}"
                "audio{width:100%;margin-top:8px}"
                "</style></head><body><div class='wrap'>";
  html += "<h1>forrest<br>speakers</h1>";
  html += "<a class='btn' href='/'>Back to notes</a>";

  if (transferServer.hasArg("msg")) {
    String msg = transferServer.arg("msg");
    html += "<div class='msg'>";
    if (msg == "added") html += "Speaker saved. Up to 4 refs are sent with each diarize request.";
    else if (msg == "renamed") html += "Speaker renamed.";
    else if (msg == "deleted") html += "Speaker deleted.";
    else if (msg == "full") html += "Speaker library is full.";
    else if (msg == "bad") html += "Need a name and a short WAV (2–5 seconds, mono preferred).";
    else html += htmlEscape(msg);
    html += "</div>";
  }

  html += "<div class='card'><p class='hint'>Known speakers keep labels stable across long meetings "
          "(API allows <b>4</b> references per request — first "
          + String(MAX_KNOWN_SPEAKER_REFS) + " in this list are used). "
          "Upload a clear 2–5&nbsp;s clip of one person speaking alone.</p>"
          "<form class='add' action='/speaker/add' method='post' enctype='multipart/form-data'>"
          "<input name='name' maxlength='31' placeholder='Display name' required>"
          "<input type='file' name='wav' accept='audio/wav,audio/*' required>"
          "<button type='submit'>Add speaker</button></form></div>";

  html += "<div class='card'>";
  if (speakersCount() == 0) {
    html += "<p class='hint'>No speakers yet.</p>";
  } else {
    for (int i = 0; i < speakersCount(); i++) {
      const Speaker* sp = speakersAt(i);
      if (!sp) continue;
      html += "<div class='row'><div><div style='font-size:20px;font-weight:700'>"
              + htmlEscape(String(sp->name)) + "</div>";
      html += "<div class='hint'>id " + String(sp->id);
      if (i < MAX_KNOWN_SPEAKER_REFS) html += " · used for diarize";
      else html += " · stored (not in top 4)";
      html += "</div>";
      String wp = speakersWavPath(sp->id);
      if (SD_MMC.exists(wp.c_str()))
        html += "<audio controls src='/speaker/audio?id=" + String(sp->id) + "'></audio>";
      html += "</div><div style='display:flex;gap:8px;flex-wrap:wrap'>";
      html += "<form action='/speaker/rename' method='get' style='display:flex;gap:6px'>"
              "<input type='hidden' name='id' value='" + String(sp->id) + "'>"
              "<input name='name' maxlength='31' placeholder='Rename' style='width:110px'>"
              "<button type='submit'>Save</button></form>";
      html += "<a class='btn danger' href='/speaker/delete?id=" + String(sp->id) + "' "
              "onclick=\"return confirm('Delete speaker " + htmlEscape(String(sp->name)) + "?')\">Delete</a>";
      html += "</div></div>";
    }
  }
  html += "</div></div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleSpeakerUpload() {
  HTTPUpload& up = transferServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    gSpeakerUploadOk = false;
    speakersEnsureDir();
    snprintf(gSpeakerUploadPath, sizeof(gSpeakerUploadPath), "%s/_upload.wav", SPEAKERS_DIR);
    if (SD_MMC.exists(gSpeakerUploadPath)) SD_MMC.remove(gSpeakerUploadPath);
    gSpeakerUploadFile = SD_MMC.open(gSpeakerUploadPath, FILE_WRITE);
    Serial.printf("[Speakers] upload start %s\n", up.filename.c_str());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (gSpeakerUploadFile) gSpeakerUploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (gSpeakerUploadFile) {
      gSpeakerUploadFile.close();
      gSpeakerUploadOk = true;
      Serial.printf("[Speakers] upload end %u bytes\n", (unsigned)up.totalSize);
    }
  }
}

void handleSpeakerAddDone() {
  String name = transferServer.hasArg("name") ? transferServer.arg("name") : "";
  name.trim();
  String loc = "/speakers?msg=bad";
  if (gSpeakerUploadOk && name.length() > 0) {
    int id = speakersAdd(name.c_str(), gSpeakerUploadPath);
    if (id > 0) loc = "/speakers?msg=added";
    else if (speakersCount() >= MAX_SPEAKERS) loc = "/speakers?msg=full";
  }
  if (SD_MMC.exists(gSpeakerUploadPath)) SD_MMC.remove(gSpeakerUploadPath);
  gSpeakerUploadOk = false;
  transferServer.sendHeader("Location", loc);
  transferServer.send(303);
}

void handleSpeakerRename() {
  int id = transferServer.hasArg("id") ? transferServer.arg("id").toInt() : 0;
  String name = transferServer.hasArg("name") ? transferServer.arg("name") : "";
  if (id > 0 && speakersRename(id, name.c_str()))
    transferServer.sendHeader("Location", "/speakers?msg=renamed");
  else
    transferServer.sendHeader("Location", "/speakers?msg=bad");
  transferServer.send(303);
}

void handleSpeakerDelete() {
  int id = transferServer.hasArg("id") ? transferServer.arg("id").toInt() : 0;
  if (id > 0) speakersDelete(id);
  transferServer.sendHeader("Location", "/speakers?msg=deleted");
  transferServer.send(303);
}

void handleSpeakerAudio() {
  int id = transferServer.hasArg("id") ? transferServer.arg("id").toInt() : 0;
  String path = speakersWavPath(id);
  File f = SD_MMC.open(path.c_str());
  if (!f) { transferServer.send(404, "text/plain", "missing"); return; }
  transferServer.streamFile(f, "audio/wav");
  f.close();
}

void handleProvisionPage() {
  Serial.println("[HTTP] GET /provision");
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Forrest Setup</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='top'><div><h1>forrest<br>setup</h1>"
          "<div class='sub'>device provisioning</div></div></div>";
  html += "<div class='card'>";
  html += "<p class='hint'>Wi-Fi: " + String(cfg::hasWifi() ? "configured" : "not set") +
          " &middot; OpenAI key: " + String(cfg::hasOpenAiKey() ? "configured" : "not set") +
          " &middot; GitHub: " + String(cfg::hasGithub() ? "on" : (cfg::githubRepo().length() ? "set, off" : "not set")) + "</p>";
  html += "<form action='/provision/save' method='post'>";
  html += "<p><input name='ssid' placeholder='Wi-Fi network (SSID)'></p>";
  html += "<p><input name='pass' type='password' placeholder='Wi-Fi password'></p>";
  html += "<p><input name='openai' type='password' placeholder='OpenAI API key (sk-...)'></p>";
  html += "<hr><p class='hint'><b>Obsidian / GitHub vault</b></p>";
  html += "<p><input name='gh_repo' placeholder='GitHub repo (owner/name)' value='" + htmlEscape(cfg::githubRepo()) + "'></p>";
  html += "<p><input name='gh_branch' placeholder='Branch (default main)' value='" + htmlEscape(cfg::githubBranch()) + "'></p>";
  html += "<p><input name='gh_dir' placeholder='Vault folder (default VoiceNotes)' value='" + htmlEscape(cfg::githubDir()) + "'></p>";
  html += "<p><input name='gh_token' type='password' placeholder='GitHub token (github_pat_...)'></p>";
  html += "<p><label><input type='checkbox' name='gh_on' value='1'" + String(cfg::githubEnabled() ? " checked" : "") + "> Enable GitHub sync</label></p>";
  html += "<p><label><input type='checkbox' name='gh_ai' value='1'" + String(cfg::githubAiEnrich() ? " checked" : "") + "> AI titles + topic links</label></p>";
  html += "<p class='hint'>Leave a text field blank to keep its current value.</p>";
  html += "<button type='submit'>Save</button></form></div>";
  html += "<a class='btn' href='/'>Back to notes</a>";
  html += "</div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleProvisionSave() {
  String ssid = transferServer.hasArg("ssid")   ? transferServer.arg("ssid")   : "";
  String pass = transferServer.hasArg("pass")   ? transferServer.arg("pass")   : "";
  String key  = transferServer.hasArg("openai") ? transferServer.arg("openai") : "";
  ssid.trim(); key.trim();
  bool changed = false;
  if (ssid.length() > 0) { cfg::setWifi(ssid, pass); changed = true; }
  if (key.length()  > 0) { cfg::setOpenAiKey(key);   changed = true; }

  // GitHub vault fields
  if (transferServer.hasArg("gh_repo")) {
    String r = transferServer.arg("gh_repo"); r.trim();
    if (r.length() > 0) { cfg::setGithubRepo(r); changed = true; }
  }
  if (transferServer.hasArg("gh_branch")) {
    String b = transferServer.arg("gh_branch"); b.trim();
    if (b.length() > 0) { cfg::setGithubBranch(b); changed = true; }
  }
  if (transferServer.hasArg("gh_dir")) {
    String d = transferServer.arg("gh_dir"); d.trim();
    if (d.length() > 0) { cfg::setGithubDir(d); changed = true; }
  }
  if (transferServer.hasArg("gh_token")) {
    String t = transferServer.arg("gh_token"); t.trim();
    if (t.length() > 0) { cfg::setGithubToken(t); changed = true; }
  }
  // checkboxes only POST when checked → presence = on, absence = off
  cfg::setGithubEnabled(transferServer.hasArg("gh_on"));
  cfg::setGithubAiEnrich(transferServer.hasArg("gh_ai"));
  changed = true;

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Forrest Setup</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='card'><h1>" + String(changed ? "saved" : "no change") + "</h1>";
  html += "<p class='hint'>" + String(changed
            ? "Settings stored to the device. Re-open Transfer or Sync to use them."
            : "Nothing was submitted.") + "</p>";
  html += "<a class='btn' href='/provision'>Back to setup</a></div></div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleOtaPage() {
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Forrest OTA</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='top'><div><h1>forrest<br>update</h1>"
          "<div class='sub'>firmware " FW_VERSION "</div></div></div>";
  html += "<div class='card'>";
  html += "<p class='hint'>Paste an HTTPS URL to a compiled firmware .bin. "
          "The device verifies the server certificate, flashes the inactive OTA slot, "
          "and reboots into it (rolling back automatically if it fails to boot).</p>";
  html += "<form action='/ota/run' method='post'>"
          "<p><input name='url' placeholder='https://host/forrest-note.bin'></p>"
          "<button type='submit'>Update firmware</button></form></div>";
  html += "<a class='btn' href='/'>Back to notes</a>";
  html += "</div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleOtaRun() {
  if (!transferServer.hasArg("url") || transferServer.arg("url").length() == 0) {
    transferServer.send(400, "text/plain", "Missing url");
    return;
  }
  String url = transferServer.arg("url");
  transferServer.send(200, "text/html",
    "<!doctype html><meta charset='utf-8'><h1>Updating&hellip;</h1>"
    "<p>Flashing firmware. The device reboots automatically if the update succeeds. "
    "If it fails it stays on the current version &mdash; reopen Transfer and retry.</p>");
  delay(250);

  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(client, url, FW_VERSION);
  if (r == HTTP_UPDATE_FAILED)
    Serial.printf("[OTA] failed (%d): %s\n",
                  httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
  else if (r == HTTP_UPDATE_NO_UPDATES)
    Serial.println("[OTA] no update available");
}

void setupTransferServer() {
  transferServer.on("/", HTTP_GET, handlePortalRoot);
  transferServer.on("/provision", HTTP_GET, handleProvisionPage);
  transferServer.on("/provision/save", HTTP_POST, handleProvisionSave);
  transferServer.on("/ota", HTTP_GET, handleOtaPage);
  transferServer.on("/ota/run", HTTP_POST, handleOtaRun);
  transferServer.on("/tags", HTTP_GET, handleTagsPage);
  transferServer.on("/tag/add", HTTP_GET, handleTagAdd);
  transferServer.on("/tag/delete", HTTP_GET, handleTagDelete);
  transferServer.on("/note/delete", HTTP_GET, handleNoteDelete);
  transferServer.on("/speakers", HTTP_GET, handleSpeakersPage);
  transferServer.on("/speaker/add", HTTP_POST, handleSpeakerAddDone, handleSpeakerUpload);
  transferServer.on("/speaker/rename", HTTP_GET, handleSpeakerRename);
  transferServer.on("/speaker/delete", HTTP_GET, handleSpeakerDelete);
  transferServer.on("/speaker/audio", HTTP_GET, handleSpeakerAudio);
  transferServer.on("/api/notes", HTTP_GET, handlePortalJson);
  transferServer.on("/export.txt", HTTP_GET, handleExportTxt);
  transferServer.on("/txt",   HTTP_GET, [](){ sendFileByNum("txt", "text/plain", true); });
  transferServer.on("/wav",   HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  true); });
  transferServer.on("/audio", HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  false); });
  transferServer.onNotFound([](){
    Serial.printf("[HTTP] miss: %s\n", transferServer.uri().c_str());
    if (captivePortalActive) {
      // Captive portal: bounce any unknown URL (incl. the OS connectivity probe)
      // to the setup page so it opens automatically.
      transferServer.sendHeader("Location", "http://" + transferUrl + "/provision", true);
      transferServer.send(302, "text/plain", "");
    } else {
      transferServer.send(404, "text/plain", "Not found");
    }
  });
}

void stopTransferMode() {
  if (transferServerActive) {
    transferServer.stop();
    transferServerActive = false;
  }
  if (captivePortalActive) {
    dnsServer.stop();
    captivePortalActive = false;
  }
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  transferUrl = "";
}
