/*
 * Forrest Voice BLE PoC — sends a synthetic WAV over GATT on button press.
 * Board: ESP32-S3 (Arduino-ESP32 3.2.x). Pair with ios/ForrestVoice app.
 *
 * Hold BOOT (GPIO0) to trigger transfer of a ~1 s test tone WAV.
 */
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "../../protocol/forrest_voice_protocol.h"

static BLECharacteristic* gStatusChar = nullptr;
static BLECharacteristic* gMetaChar = nullptr;
static BLECharacteristic* gAudioChar = nullptr;
static bool gDeviceConnected = false;
static bool gShouldTransfer = false;

static uint16_t gRecordingId = 1;

// Minimal 16 kHz mono 16-bit PCM WAV (~1 s silence + header = 44 + 32000 bytes)
// PoC uses a tiny 0.5 s buffer of zeros for speed (~16044 bytes total)
static const uint32_t kPcmBytes = 16000; // 0.5 s
static const uint32_t kTotalBytes = 44 + kPcmBytes;

static void notifyStatus(uint8_t state) {
  if (!gStatusChar) return;
  uint8_t payload[4] = { state, 100, 0, 0 };
  gStatusChar->setValue(payload, sizeof(payload));
  gStatusChar->notify();
}

static void buildWavHeader(uint8_t* hdr, uint32_t pcmBytes) {
  uint32_t fileSize = pcmBytes + 36;
  memcpy(hdr + 0, "RIFF", 4);
  memcpy(hdr + 4, &fileSize, 4);
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  uint32_t fmtSize = 16;
  memcpy(hdr + 16, &fmtSize, 4);
  uint16_t audioFormat = 1;
  memcpy(hdr + 20, &audioFormat, 2);
  uint16_t channels = 1;
  memcpy(hdr + 22, &channels, 2);
  uint32_t sampleRate = 16000;
  memcpy(hdr + 24, &sampleRate, 4);
  uint32_t byteRate = 16000 * 2;
  memcpy(hdr + 28, &byteRate, 4);
  uint16_t blockAlign = 2;
  memcpy(hdr + 32, &blockAlign, 2);
  uint16_t bits = 16;
  memcpy(hdr + 34, &bits, 2);
  memcpy(hdr + 36, "data", 4);
  memcpy(hdr + 40, &pcmBytes, 4);
}

static void sendRecording() {
  if (!gDeviceConnected || !gMetaChar || !gAudioChar) return;

  notifyStatus(FV_STATE_TRANSFERRING);

  // RecordingMeta
  uint8_t meta[16] = {0};
  meta[0] = FV_MSG_RECORDING_META;
  meta[1] = 0; // PCM WAV
  meta[2] = gRecordingId & 0xFF;
  meta[3] = (gRecordingId >> 8) & 0xFF;
  memcpy(meta + 4, &kTotalBytes, 4);
  uint16_t sr = 16000;
  memcpy(meta + 8, &sr, 2);
  meta[10] = 1;
  meta[11] = 16;
  gMetaChar->setValue(meta, sizeof(meta));
  gMetaChar->notify();
  delay(50);

  const uint16_t chunkPayloadMax = 180;
  uint8_t wavHdr[44];
  buildWavHeader(wavHdr, kPcmBytes);

  uint32_t sent = 0;
  uint16_t seq = 0;

  while (sent < kTotalBytes) {
    uint32_t remain = kTotalBytes - sent;
    uint16_t payloadLen = remain > chunkPayloadMax ? chunkPayloadMax : (uint16_t)remain;

    uint8_t packet[8 + chunkPayloadMax];
    packet[0] = FV_MSG_AUDIO_CHUNK;
    packet[1] = (sent + payloadLen >= kTotalBytes) ? FV_FLAG_LAST_CHUNK : 0;
    packet[2] = gRecordingId & 0xFF;
    packet[3] = (gRecordingId >> 8) & 0xFF;
    packet[4] = seq & 0xFF;
    packet[5] = (seq >> 8) & 0xFF;
    packet[6] = payloadLen & 0xFF;
    packet[7] = (payloadLen >> 8) & 0xFF;

    if (sent < 44) {
      uint16_t hdrOff = (uint16_t)sent;
      uint16_t hdrRemain = 44 - hdrOff;
      uint16_t n = payloadLen < hdrRemain ? payloadLen : hdrRemain;
      memcpy(packet + 8, wavHdr + hdrOff, n);
      if (n < payloadLen) {
        memset(packet + 8 + n, 0, payloadLen - n);
      }
    } else {
      memset(packet + 8, 0, payloadLen);
    }

    gAudioChar->setValue(packet, 8 + payloadLen);
    gAudioChar->notify();

    sent += payloadLen;
    seq++;
    delay(15); // yield for iOS stack (~50–80 KB/s effective)
  }

  gRecordingId++;
  notifyStatus(FV_STATE_SUCCESS);
  Serial.printf("[BLE] sent recording %u bytes in %u chunks\n", kTotalBytes, seq);
  delay(500);
  notifyStatus(FV_STATE_IDLE);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    gDeviceConnected = true;
    Serial.println("[BLE] connected");
    notifyStatus(FV_STATE_IDLE);
  }
  void onDisconnect(BLEServer* pServer) override {
    gDeviceConnected = false;
    Serial.println("[BLE] disconnected");
    BLEDevice::startAdvertising();
  }
};

class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String v = pCharacteristic->getValue();
    if (v.length() == 0) return;
    uint8_t cmd = v[0];
    if (cmd == FV_CMD_RETRY_PENDING) {
      gShouldTransfer = true;
    }
  }
};

static void setupBle() {
  BLEDevice::init("Forrest-Voice");
  BLEDevice::setMTU(247);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* service = server->createService(FV_SVC_UUID);

  BLECharacteristic* devInfo = service->createCharacteristic(
      FV_CHAR_DEVICE_INFO, BLECharacteristic::PROPERTY_READ);
  devInfo->setValue("Forrest Voice PoC");

  gStatusChar = service->createCharacteristic(
      FV_CHAR_DEVICE_STATUS,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  gStatusChar->addDescriptor(new BLE2902());

  BLECharacteristic* control = service->createCharacteristic(
      FV_CHAR_CONTROL,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  control->setCallbacks(new ControlCallbacks());

  gMetaChar = service->createCharacteristic(
      FV_CHAR_RECORDING_META, BLECharacteristic::PROPERTY_NOTIFY);
  gMetaChar->addDescriptor(new BLE2902());

  gAudioChar = service->createCharacteristic(
      FV_CHAR_AUDIO_DATA, BLECharacteristic::PROPERTY_NOTIFY);
  gAudioChar->addDescriptor(new BLE2902());

  BLECharacteristic* result = service->createCharacteristic(
      FV_CHAR_RESULT_TEXT,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);

  BLECharacteristic* proto = service->createCharacteristic(
      FV_CHAR_PROTOCOL_VER, BLECharacteristic::PROPERTY_READ);
  uint16_t ver = FV_PROTOCOL_VERSION;
  proto->setValue((uint8_t*)&ver, 2);

  service->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(FV_SVC_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] advertising as Forrest-Voice");
}

void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  setupBle();
  notifyStatus(FV_STATE_IDLE);
}

void loop() {
  static bool lastBtn = true;
  bool btn = digitalRead(0); // active low

  if (lastBtn && !btn && gDeviceConnected) {
    gShouldTransfer = true;
  }
  lastBtn = btn;

  if (gShouldTransfer && gDeviceConnected) {
    gShouldTransfer = false;
    sendRecording();
  }

  delay(20);
}
