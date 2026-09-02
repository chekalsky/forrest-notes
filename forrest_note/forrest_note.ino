#include "Arduino.h"
#include "SD_MMC.h"
#include <WebServer.h>
#include <DNSServer.h>
#include <vector>
#include "driver/i2c_master.h"
#include "esp_sleep.h"
#include "esp_system.h"

extern "C" {
#include "config.h"
#include "src/i2c_bsp/i2c_bsp.h"
#include "src/audio/audio_bsp.h"
}

#include "src/power/board_power_bsp.h"
#include "src/display/epaper_driver_bsp.h"

#include "types.h"
#include "globals.h"
#include "src/app/ui.h"
#include "src/app/notes.h"
#include "src/app/sleep.h"
#include "src/app/config_store.h"
#include "src/app/speakers.h"
#include "src/app/flows.h"
#include "src/app/serial_cfg.h"
#include "src/app/app_fsm.h"

#ifdef FORREST_BLE_VOICE
#include "src/app/ble_voice.h"
#endif

// Sketch entry. Hardware bring-up and global storage live here; user flows and
// the button FSM live in src/app/. Control flow: see ARCHITECTURE.md.
// All pin, timing, path and threshold constants live in config.h.

// ─── Content arrays ───────────────────────────────────────────────────────
const char* DEFAULT_TAGS[]    = { "undefined", "Note", "Work", "Idea", "Buy", "Private", "Meeting" };
const char* MENU_ITEMS[]     = { "Notes", "Tags", "Sync", "Settings" };
const char* SETTINGS_ITEMS[] = { "Sounds", "Transfer", "Device", "Erase All", "Reset" };

// ─── Global variable definitions ─────────────────────────────────────────
board_power_bsp_t      board(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
epaper_driver_display* display = nullptr;

std::vector<NoteEntry> noteIndex;

AppState state          = STATE_IDLE;
int      listCursor     = 0;
int      tagCursor      = 2;
int      menuCursor     = 0;
int      settingsCursor = 0;
int      activeFilter   = -1;
int      lastRecNum     = -1;

uint32_t lastActivityMs      = 0;
bool     wokeFromUltraSleep  = false;
bool     wakeToMenuRequested = false;
bool     wakeToRecRequested  = false;

uint32_t tickerLastMs = 0;
int      tickerOffset = 0;
int      tickerCursor = -1;

WebServer transferServer(80);
bool      transferServerActive = false;
String    transferUrl          = "";
DNSServer dnsServer;
bool      captivePortalActive  = false;

bool timeReady    = false;
bool audioPlaying = false;
bool stopPlayback = false;

int detailScrollPage = 0;
int detailTotalLines = 0;

uint32_t lastBatCheckMs    = 0;
bool     batLowWarned      = false;
bool     batWarnActive     = false;
uint32_t batWarnShowUntilMs = 0;

char tags[20][32];
int  tagCount = 0;

void keepBatteryPowerOn() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Forrest Note " FIRMWARE_VERSION " ===");

#ifndef FORREST_BLE_VOICE
  cfg::begin();   // load Wi-Fi / API secrets from NVS (seeded from secrets.h once)
#endif

  pinMode(BTN_REC, INPUT_PULLUP);
  pinMode(BTN_PWR, INPUT_PULLUP);

  board.VBAT_POWER_ON();

  esp_reset_reason_t resetReason = esp_reset_reason();
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  uint64_t ext1Pins = 0;
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    ext1Pins = esp_sleep_get_ext1_wakeup_status();
  }
  wokeFromUltraSleep = (resetReason == ESP_RST_DEEPSLEEP)
      || (wakeCause == ESP_SLEEP_WAKEUP_EXT1)
      || (ext1Pins != 0);
  delay(50);

  wakeToMenuRequested = (wokeFromUltraSleep && digitalRead(BTN_PWR) == LOW);
  wakeToRecRequested  = (wokeFromUltraSleep && digitalRead(BTN_REC) == LOW);

  resetActivity();
  keepBatteryPowerOn();
  delay(20);

  board.POWEER_EPD_ON();
#ifndef FORREST_BLE_VOICE
  board.POWEER_Audio_ON();
#endif
  delay(200);

  custom_lcd_spi_t dispCfg = {};
  dispCfg.cs       = EPD_CS_PIN;
  dispCfg.dc       = EPD_DC_PIN;
  dispCfg.rst      = EPD_RST_PIN;
  dispCfg.busy     = EPD_BUSY_PIN;
  dispCfg.mosi     = EPD_MOSI_PIN;
  dispCfg.scl      = EPD_SCK_PIN;
  dispCfg.spi_host = EPD_SPI_NUM;
  dispCfg.buffer_len = (200*200)/8;

  display = new epaper_driver_display(200, 200, dispCfg);
  display->EPD_Init();
  display->EPD_Clear();
  display->EPD_DisplayPartBaseImage();
  display->EPD_Init_Partial();

#ifdef FORREST_BLE_VOICE
  bool bleWakeRequested = wokeFromUltraSleep
      || digitalRead(BTN_REC) == LOW
      || digitalRead(BTN_PWR) == LOW;

  if (!bleWakeRequested) {
    Serial.println("[BLE] cold boot — entering deep sleep");
    enterBleVoiceSleep();
  }
#endif

  i2c_master_Init();
  delay(50);

#ifndef FORREST_BLE_VOICE
  audio_bsp_init();
  audio_play_init();
#endif

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    showError("SD ERR");
    while (true) delay(1000);
  }

#ifdef FORREST_BLE_VOICE
  Serial.println("=== BLE Voice mode (hold REC, release to send) ===");
  bleVoiceEnterAwake();
#else
  if (!SD_MMC.exists(NOTES_DIR)) SD_MMC.mkdir(NOTES_DIR);
  speakersEnsureDir();
  speakersLoad();
  loadTags();
  loadIndex();
  Serial.printf("[SD] %d notes, %d speakers\n", (int)noteIndex.size(), speakersCount());

  if (wakeToMenuRequested) {
    menuCursor = 0;
    state = STATE_MENU;
    showMenu(menuCursor);
  } else if (wakeToRecRequested) {
    startRecordFlow();
  } else {
    showIdle();
  }
#endif
}

void loop() {
#ifdef FORREST_BLE_VOICE
  bleVoiceLoop();
#else
  handleSerialConfig();
  serviceDisplay();
  appHandleLoop();
  delay(LOOP_DELAY_MS);
#endif
}
