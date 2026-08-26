#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "flows.h"
#include "ui.h"
#include "record.h"
#include "transcribe.h"
#include "obsidian.h"
#include "portal.h"
#include "rtc.h"
#include "notes.h"
#include "config_store.h"
#include "../../sounds.h"
#include "WiFi.h"

void startRecordFlow() {
  state = STATE_RECORDING;
  showRecording();

  palaSoundSetEnabled(false);
  bool recOk = record(true);
  palaSoundSetEnabled(true);

  if (!recOk) {
    showError("REC FAIL");
    delay(1600);
    state = STATE_IDLE;
    showIdle();
    return;
  }

  soundSaved();

  state = STATE_SAVED;
  showSaved(lastRecNum);
  delay(900);

  tagCursor = 0;
  for (int i = 0; i < tagCount; i++) {
    if (strcasecmp(tags[i], DEFAULT_NOTE_TAG) == 0) { tagCursor = i; break; }
  }
  state = STATE_TAG_SELECT;
  showTagSelect(tagCursor);
}

void startMeetingRecordFlow() {
  state = STATE_RECORDING;
  showMeetingRecording();
  soundSelect();

  palaSoundSetEnabled(false);
  bool recOk = record(false);
  palaSoundSetEnabled(true);

  if (!recOk) {
    showError("REC FAIL");
    delay(1600);
    state = STATE_IDLE;
    showIdle();
    return;
  }

  soundSaved();

  state = STATE_SAVED;
  showSaved(lastRecNum);
  delay(900);

  // Prefer Meeting for the confirm picker; fall back to undefined.
  tagCursor = 0;
  for (int i = 0; i < tagCount; i++) {
    if (strcasecmp(tags[i], DEFAULT_NOTE_TAG) == 0) tagCursor = i;
  }
  for (int i = 0; i < tagCount; i++) {
    if (strcasecmp(tags[i], "meeting") == 0) { tagCursor = i; break; }
  }
  state = STATE_TAG_SELECT;
  showTagSelect(tagCursor);
}

void startSyncFlow() {
  // Blocking subroutine: state stays STATE_MENU (or IDLE on error). See ARCHITECTURE.md.
  if (!cfg::hasWifi()) {
    showError("NO WIFI CFG");
    delay(1800);
    showIdle();
    return;
  }

  const int MAX_TRIES = 20;
  showWifiConnecting(0, MAX_TRIES);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);        // creds live in our NVS; don't wear the WiFi NVS
  WiFi.setAutoReconnect(true);   // SDK recovers (with backoff) if the link drops mid-sync
  String ssid = cfg::wifiSsid(), pass = cfg::wifiPass();
  WiFi.begin(ssid.c_str(), pass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < MAX_TRIES) {
    delay(500); tries++;
    showWifiConnecting(tries, MAX_TRIES);
  }

  if (WiFi.status() == WL_CONNECTED) {
    showSyncProgress("time", 100, "NTP clock", nullptr, "syncing", -1, -1);
    syncTimeFromNTP(6000);
    transcribeAll();
    loadIndex();
    obsidianSyncAll();        // push freshly-transcribed notes to the Obsidian vault
    WiFi.disconnect(true);
    syncProgressEnd();
    showDone();
    soundSuccess();
    delay(1600);
  } else {
    syncProgressEnd();
    showError("NO WIFI");
    delay(1800);
  }

  if (wakeToMenuRequested) {
    menuCursor = 0;
    state = STATE_MENU;
    showMenu(menuCursor);
  } else {
    showIdle();
  }
}

void startTransferMode() {
  state = STATE_TRANSFER;
  showTransferConnecting();

  // First-time setup: no Wi-Fi credentials yet -> host a SoftAP so the user can
  // open the portal and provision Wi-Fi + OpenAI key (saved to NVS).
  if (!cfg::hasWifi()) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    bool apOk = WiFi.softAP("ForrestNote-Setup");
    delay(200);                                   // let the AP + DHCP server come up
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[Transfer] SoftAP '%s' start=%d ip=%s\n",
                  "ForrestNote-Setup", apOk, apIP.toString().c_str());

    // Captive portal: resolve every DNS query to us so any URL (and the OS's
    // own connectivity probe) lands on the device and pops the setup page.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIP);
    captivePortalActive = true;

    setupTransferServer();
    transferServer.begin();
    transferServerActive = true;
    transferUrl = apIP.toString();
    Serial.println("[Transfer] HTTP server started on :80");
    showTransferMode(transferUrl.c_str());
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  String ssid = cfg::wifiSsid(), pass = cfg::wifiPass();
  WiFi.begin(ssid.c_str(), pass.c_str());

  const int MAX_TRIES = 24;
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < MAX_TRIES) {
    delay(500); tries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    showError("NO WIFI");
    delay(1600);
    state = STATE_SETTINGS;
    showSettings(settingsCursor);
    return;
  }

  syncTimeFromNTP(8000);
  setupTransferServer();
  transferServer.begin();
  transferServerActive = true;

  IPAddress ip = WiFi.localIP();
  transferUrl = ip.toString();
  showTransferMode(transferUrl.c_str());
}
