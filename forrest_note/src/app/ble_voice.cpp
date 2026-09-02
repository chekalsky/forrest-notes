#include "ble_voice.h"

#ifdef FORREST_BLE_VOICE

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <SD_MMC.h>
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "../../config.h"
#include "forrest_voice_protocol.h"
#include "wav_util.h"
#include "ble_voice_ui.h"
#include "ble_voice_queue.h"
#include "battery.h"

extern "C" {
#include "../../src/audio/audio_bsp.h"
}

static BLECharacteristic* gStatusChar = nullptr;
static BLECharacteristic* gMetaChar = nullptr;
static BLECharacteristic* gAudioChar = nullptr;
static bool gDeviceConnected = false;
static bool gDrainQueue = false;
static uint16_t gRecordingId = 1;

static bool gBtnWasDown = false;

static volatile bool gAckReceived = false;
static volatile uint16_t gAckSeq = 0;

static uint8_t gCurrentState = FV_STATE_IDLE;
static uint8_t gPingSeq = 0;
static uint32_t gLastPingMs = 0;
static uint8_t gRemoteBda[6] = {0};
static bool gHasRemoteBda = false;

// ── BLE helpers ───────────────────────────────────────────────────────────

static void notifyStatus(uint8_t state) {
  if (!gStatusChar) return;
  gCurrentState = state;
  int batt = readBatteryPercent();
  uint8_t payload[4] = {
      state,
      (uint8_t)(batt < 0 ? 255 : (batt > 100 ? 100 : batt)),
      (uint8_t)bleQueueCount(),
      gPingSeq++,
  };
  gStatusChar->setValue(payload, sizeof(payload));
  gStatusChar->notify();
}

static uint16_t bleChunkPayloadMax() {
  uint16_t mtu = BLEDevice::getMTU();
  if (mtu < 23) mtu = 247;
  uint16_t n = mtu - 3 - 8;  // ATT overhead + 8-byte header
  return n > BLE_CHUNK_MAX ? BLE_CHUNK_MAX : n;
}

static void requestIdleBleConnection() {
  if (!gHasRemoteBda) return;
  esp_ble_conn_update_params_t params = {};
  memcpy(params.bda, gRemoteBda, sizeof(params.bda));
  params.min_int = 0x0050;  // 100 ms
  params.max_int = 0x00A0;  // 200 ms
  params.latency = 4;       // peripheral may skip connection events
  params.timeout = 400;
  esp_ble_gap_update_conn_params(&params);
}

static void requestTransferBleConnection() {
  if (!gHasRemoteBda) return;
  esp_ble_conn_update_params_t params = {};
  memcpy(params.bda, gRemoteBda, sizeof(params.bda));
  params.min_int = 0x000C;  // 15 ms
  params.max_int = 0x0020;  // 40 ms
  params.latency = 0;
  params.timeout = 400;
  esp_ble_gap_update_conn_params(&params);
}

static bool waitForAck(uint16_t needSeq, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (gAckReceived && gAckSeq >= needSeq) {
      gAckReceived = false;
      return true;
    }
    delay(2);
  }
  Serial.printf("[BLE] ACK timeout need=%u last=%u\n", needSeq, gAckSeq);
  return false;
}

static bool wavPcmTooShort(uint32_t pcmBytes) {
  return pcmBytes < BLE_MIN_REC_PCM_BYTES;
}

static bool wavFileTooShort(uint32_t fileBytes) {
  return fileBytes <= 44 || wavPcmTooShort(fileBytes - 44);
}

static bool sendWavFile(const char* path) {
  if (!gDeviceConnected || !gMetaChar || !gAudioChar) return false;

  File f = SD_MMC.open(path);
  if (!f) {
    Serial.println("[BLE] open failed");
    notifyStatus(FV_STATE_ERROR);
    bleVoiceUiSetState(BLE_UI_ERROR, "File error");
    return false;
  }
  uint32_t totalBytes = (uint32_t)f.size();
  if (totalBytes <= 44) {
    f.close();
    notifyStatus(FV_STATE_ERROR);
    bleVoiceUiSetState(BLE_UI_ERROR, "Empty recording");
    return false;
  }

  bleVoiceUiSetState(BLE_UI_SENDING);
  notifyStatus(FV_STATE_TRANSFERRING);

  requestTransferBleConnection();
  delay(BLE_XFER_CONN_SETTLE_MS);

  gAckReceived = false;
  gAckSeq = 0;

  uint8_t meta[16] = {0};
  meta[0] = FV_MSG_RECORDING_META;
  meta[1] = 0;
  meta[2] = gRecordingId & 0xFF;
  meta[3] = (gRecordingId >> 8) & 0xFF;
  memcpy(meta + 4, &totalBytes, 4);
  uint16_t sr = SAMPLE_RATE;
  memcpy(meta + 8, &sr, 2);
  meta[10] = 1;
  meta[11] = 16;
  gMetaChar->setValue(meta, sizeof(meta));
  gMetaChar->notify();
  delay(50);

  const uint16_t payloadMax = bleChunkPayloadMax();
  uint8_t* readBuf = (uint8_t*)malloc(payloadMax);
  uint8_t* packet = (uint8_t*)malloc(8 + payloadMax);
  if (!readBuf || !packet) {
    if (readBuf) free(readBuf);
    if (packet) free(packet);
    f.close();
    bleVoiceUiSetState(BLE_UI_ERROR, "No memory");
    return false;
  }

  uint32_t sent = 0;
  uint16_t seq = 0;
  int lastUiPct = -1;
  uint32_t t0 = millis();
  bool ok = true;

  while (sent < totalBytes && ok) {
    uint32_t remain = totalBytes - sent;
    uint16_t payloadLen = remain > payloadMax ? payloadMax : (uint16_t)remain;
    int got = f.read(readBuf, payloadLen);
    if (got <= 0) break;
    payloadLen = (uint16_t)got;

    packet[0] = FV_MSG_AUDIO_CHUNK;
    packet[1] = (sent + payloadLen >= totalBytes) ? FV_FLAG_LAST_CHUNK : 0;
    packet[2] = gRecordingId & 0xFF;
    packet[3] = (gRecordingId >> 8) & 0xFF;
    packet[4] = seq & 0xFF;
    packet[5] = (seq >> 8) & 0xFF;
    packet[6] = payloadLen & 0xFF;
    packet[7] = (payloadLen >> 8) & 0xFF;
    memcpy(packet + 8, readBuf, payloadLen);

    gAudioChar->setValue(packet, 8 + payloadLen);
    gAudioChar->notify();

    sent += payloadLen;

    int pct = (int)((sent * 100UL) / totalBytes);
    if (pct >= lastUiPct + 10) {
      bleVoiceUiSetSendProgress(pct);
      lastUiPct = pct;
    }

    if (seq % FV_ACK_EVERY == (FV_ACK_EVERY - 1) || sent >= totalBytes) {
      if (!waitForAck(seq, 5000)) {
        Serial.printf("[BLE] ACK timeout at seq %u\n", seq);
        ok = false;
        break;
      }
    }

    seq++;
    delay(BLE_CHUNK_DELAY_MS);
  }

  free(readBuf);
  free(packet);
  f.close();

  requestIdleBleConnection();

  if (!ok || sent < totalBytes) {
    notifyStatus(FV_STATE_ERROR);
    bleVoiceUiSetState(BLE_UI_ERROR, "Transfer fail");
    delay(1500);
    bleVoiceUiSetState(BLE_UI_READY);
    notifyStatus(FV_STATE_IDLE);
    return false;
  }

  gRecordingId++;
  notifyStatus(FV_STATE_SUCCESS);
  uint32_t ms = millis() - t0;
  Serial.printf("[BLE] sent %s (%u bytes, %u chunks, %u ms, ~%u KB/s)\n",
                path, sent, seq, ms, ms ? (unsigned)(sent / ms) : 0);
  bleVoiceUiShowResult("Sent");
  delay(1500);
  bleVoiceUiSetState(BLE_UI_READY);
  notifyStatus(FV_STATE_IDLE);
  return true;
}

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
    gDeviceConnected = true;
    Serial.println("[BLE] connected");
    bleVoiceUiSetPhoneConnected(true);
    if (param) {
      memcpy(gRemoteBda, param->connect.remote_bda, sizeof(gRemoteBda));
      gHasRemoteBda = true;
      requestIdleBleConnection();
    }
    gLastPingMs = millis();
    notifyStatus(FV_STATE_IDLE);
    if (bleQueueCount() > 0) {
      gDrainQueue = true;
      Serial.printf("[BLE] %d queued — will drain\n", bleQueueCount());
    }
  }

  void onDisconnect(BLEServer* pServer) override {
    gDeviceConnected = false;
    gHasRemoteBda = false;
    Serial.println("[BLE] disconnected");
    bleVoiceUiSetPhoneConnected(false);
    BLEDevice::startAdvertising();
  }
};

class BleResultCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String v = pCharacteristic->getValue();
    if (v.length() == 0) return;
    bleVoiceUiShowResult(v.c_str());
  }
};

class BleControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String v = pCharacteristic->getValue();
    if (v.length() == 0) return;
    uint8_t cmd = v[0];
    if (cmd == FV_CMD_ACK_CHUNK && v.length() >= 5) {
      gAckSeq = (uint8_t)v[3] | ((uint16_t)(uint8_t)v[4] << 8);
      gAckReceived = true;
      Serial.printf("[BLE] ACK_CHUNK seq=%u\n", gAckSeq);
    } else if (cmd == FV_CMD_RETRY_PENDING) {
      gDrainQueue = true;
    }
  }
};

static void setupBle() {
  BLEDevice::init("Forrest-Voice");
  BLEDevice::setMTU(517);
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());

  BLEService* service = server->createService(FV_SVC_UUID);

  BLECharacteristic* devInfo = service->createCharacteristic(
      FV_CHAR_DEVICE_INFO, BLECharacteristic::PROPERTY_READ);
  devInfo->setValue("Forrest Voice");

  gStatusChar = service->createCharacteristic(
      FV_CHAR_DEVICE_STATUS,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  gStatusChar->addDescriptor(new BLE2902());

  BLECharacteristic* control = service->createCharacteristic(
      FV_CHAR_CONTROL,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  control->setCallbacks(new BleControlCallbacks());

  gMetaChar = service->createCharacteristic(
      FV_CHAR_RECORDING_META, BLECharacteristic::PROPERTY_NOTIFY);
  gMetaChar->addDescriptor(new BLE2902());

  gAudioChar = service->createCharacteristic(
      FV_CHAR_AUDIO_DATA, BLECharacteristic::PROPERTY_NOTIFY);
  gAudioChar->addDescriptor(new BLE2902());

  BLECharacteristic* resultChar = service->createCharacteristic(
      FV_CHAR_RESULT_TEXT,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  resultChar->setCallbacks(new BleResultCallbacks());

  BLECharacteristic* proto = service->createCharacteristic(
      FV_CHAR_PROTOCOL_VER, BLECharacteristic::PROPERTY_READ);
  uint16_t ver = FV_PROTOCOL_VERSION;
  proto->setValue((uint8_t*)&ver, 2);

  service->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(FV_SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] advertising Forrest-Voice");
}

// ── Hold-to-record (simplified from record.cpp) ─────────────────────────────

struct RecCtx {
  RingbufHandle_t ring;
  volatile bool running;
  volatile bool finished;
};

static void recProducerTask(void* arg) {
  RecCtx* ctx = (RecCtx*)arg;
  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF, MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF / 2, MALLOC_CAP_8BIT);
  const int monoSamples = REC_BUF / 4;

  if (sbuf && mbuf) {
    while (ctx->running) {
      audio_playback_read((void*)sbuf, REC_BUF);
      for (int i = 0; i < monoSamples; i++) mbuf[i] = sbuf[i * 2];
      xRingbufferSend(ctx->ring, mbuf, monoSamples * 2, pdMS_TO_TICKS(1000));
    }
  }
  if (sbuf) heap_caps_free(sbuf);
  if (mbuf) heap_caps_free(mbuf);
  ctx->finished = true;
  vTaskDelete(NULL);
}

static bool recButtonStillHeld() {
  // Ignore brief HIGH glitches while the button is physically held.
  static uint8_t upSamples = 0;
  if (digitalRead(BTN_REC) == LOW) {
    upSamples = 0;
    return true;
  }
  upSamples++;
  return upSamples < 5;
}

static bool recordHoldToFile(const char* path) {
  notifyStatus(FV_STATE_RECORDING);
  bleVoiceUiSetState(BLE_UI_LISTENING);

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    bleVoiceUiSetState(BLE_UI_ERROR, "SD write fail");
    delay(1200);
    bleVoiceUiSetState(BLE_UI_READY);
    return false;
  }

  writePcmWavHeader(f, 0);
  f.flush();
  f.seek(44);

  RecCtx ctx;
  ctx.ring = xRingbufferCreateWithCaps(REC_RING_LEN, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
  if (!ctx.ring) {
    f.close();
    bleVoiceUiSetState(BLE_UI_ERROR, "No memory");
    delay(1200);
    bleVoiceUiSetState(BLE_UI_READY);
    return false;
  }
  ctx.running = true;
  ctx.finished = false;

  TaskHandle_t producer = NULL;
  if (xTaskCreatePinnedToCore(recProducerTask, "recprod", 4096, &ctx, 6, &producer, 0) != pdPASS) {
    vRingbufferDeleteWithCaps(ctx.ring);
    f.close();
    bleVoiceUiSetState(BLE_UI_ERROR, "Rec start fail");
    delay(1200);
    bleVoiceUiSetState(BLE_UI_READY);
    return false;
  }

  uint32_t totalMono = 0;
  uint32_t t0 = millis();
  uint32_t lastFlush = millis();
  uint32_t lastUi = millis();
  int recPeak = 0;
  bool writeOk = true;

  auto drain = [&](TickType_t wait) -> bool {
    size_t got = 0;
    void* item = xRingbufferReceive(ctx.ring, &got, wait);
    if (!item) return false;
    int16_t* sp = (int16_t*)item;
    int ns = (int)(got / 2);
    for (int i = 0; i < ns; i++) {
      int a = abs(sp[i]);
      if (a > recPeak) recPeak = a;
    }
    size_t written = f.write((uint8_t*)item, got);
    vRingbufferReturnItem(ctx.ring, item);
    totalMono += written;
    if (written != got) writeOk = false;
    return true;
  };

  while (writeOk && (millis() - t0 < MAX_REC_MS)) {
    drain(pdMS_TO_TICKS(40));

    uint32_t elapsed = millis() - t0;
    // Match record.cpp: keep going while held, or until min hold time elapses.
    if (elapsed >= BLE_MIN_REC_MS && !recButtonStillHeld()) break;

    if (millis() - lastFlush >= REC_FLUSH_MS) {
      lastFlush = millis();
      f.flush();
      writePcmWavHeader(f, totalMono);
      f.flush();
      f.seek(44 + totalMono);
    }
    if (millis() - lastUi >= 200) {
      lastUi = millis();
      int lvl = (int)((long)recPeak * 152L * 3L / 32767L);
      if (lvl > 152) lvl = 152;
      bleVoiceUiUpdateListening(millis() - t0, lvl);
      recPeak = 0;
    }
  }

  ctx.running = false;
  while (!ctx.finished) drain(pdMS_TO_TICKS(50));
  while (drain(0)) {}

  vRingbufferDeleteWithCaps(ctx.ring);
  writePcmWavHeader(f, totalMono);
  f.close();

  if (wavPcmTooShort(totalMono)) {
    SD_MMC.remove(path);
    Serial.printf("[BLE] discarded (< %lums)\n", BLE_MIN_REC_MS);
    bleVoiceUiSetState(BLE_UI_ERROR, "Too short");
    delay(1200);
    bleVoiceUiSetState(BLE_UI_READY);
    return false;
  }

  Serial.printf("[BLE] recorded %lu PCM bytes\n", (unsigned long)totalMono);
  return writeOk;
}

static void queueAfterRecord(const char* path) {
  const char* base = strrchr(path, '/');
  base = base ? base + 1 : path;
  bleQueueAdd(base);
  int n = bleQueueCount();
  bleVoiceUiSetQueueCount(n);

  if (gDeviceConnected) {
    gDrainQueue = true;
  } else {
    char msg[32];
    snprintf(msg, sizeof(msg), "Saved · %d queued", n);
    bleVoiceUiSetState(BLE_UI_READY, msg);
    delay(1500);
    bleVoiceUiSetState(BLE_UI_READY);
    notifyStatus(FV_STATE_WAITING);
  }
}

static void tickKeepalive() {
  if (!gDeviceConnected) return;
  if (gCurrentState == FV_STATE_RECORDING || gCurrentState == FV_STATE_TRANSFERRING ||
      gCurrentState == FV_STATE_FINALIZING) {
    return;
  }

  uint32_t interval = bleQueueCount() > 0 ? BLE_PING_QUEUE_MS : BLE_PING_IDLE_MS;
  uint32_t now = millis();
  if (now - gLastPingMs < interval) return;

  gLastPingMs = now;
  uint8_t state = bleQueueCount() > 0 ? FV_STATE_WAITING : FV_STATE_IDLE;
  notifyStatus(state);
  Serial.printf("[BLE] keepalive state=%u queue=%d seq=%u\n",
                state, bleQueueCount(), gPingSeq - 1);
}

static void tryDrainQueue() {
  if (!gDrainQueue || !gDeviceConnected) return;

  char path[64];
  if (!bleQueuePeekPath(path, sizeof(path))) {
    gDrainQueue = false;
    return;
  }

  File peek = SD_MMC.open(path);
  if (peek) {
    uint32_t fileBytes = (uint32_t)peek.size();
    peek.close();
    if (wavFileTooShort(fileBytes)) {
      Serial.printf("[BLE] dropping queued file (< %lums)\n", BLE_MIN_REC_MS);
      bleQueueRemoveFirst();
      bleVoiceUiSetQueueCount(bleQueueCount());
      return;
    }
  }

  if (sendWavFile(path)) {
    bleQueueRemoveFirst();
    bleVoiceUiSetQueueCount(bleQueueCount());
    if (bleQueueCount() > 0) {
      gDrainQueue = true;
    } else {
      gDrainQueue = false;
    }
  } else {
    gDrainQueue = false;
    notifyStatus(FV_STATE_WAITING);
  }
}

// ── Public API ──────────────────────────────────────────────────────────────

void bleVoiceSetup() {
  setupBle();
  bleVoiceUiInit();
  bleQueueEnsureDir();
  bleVoiceUiSetQueueCount(bleQueueCount());
  notifyStatus(FV_STATE_IDLE);
}

void bleVoiceHandleButton() {
  bool down = (digitalRead(BTN_REC) == LOW);

  // Hold-to-record: block until release (debounced inside recordHoldToFile).
  if (!gBtnWasDown && down) {
    gDrainQueue = false;
    char recPath[64];
    if (!bleQueueAllocPath(recPath, sizeof(recPath))) {
      bleVoiceUiSetState(BLE_UI_ERROR, "Queue full");
      delay(1200);
      bleVoiceUiSetState(BLE_UI_READY);
    } else {
      Serial.println("[BLE] record start");
      if (recordHoldToFile(recPath)) {
        Serial.println("[BLE] record stop");
        notifyStatus(FV_STATE_FINALIZING);
        queueAfterRecord(recPath);
      } else {
        SD_MMC.remove(recPath);
        notifyStatus(FV_STATE_ERROR);
      }
    }
  }

  gBtnWasDown = down;
}

void bleVoiceLoop() {
  bleVoiceUiTick();
  bleVoiceHandleButton();
  tickKeepalive();
  tryDrainQueue();
  delay(10);
}

#endif  // FORREST_BLE_VOICE
