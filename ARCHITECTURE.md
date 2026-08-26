# Architecture

Firmware for the Forrest Note device: record voice on an ESP32-S3 e-paper board, diarize/transcribe with OpenAI, enrich into Markdown, and push to a GitHub repo for Obsidian.

This document is the map of **application** code under [`forrest_note/src/app/`](forrest_note/src/app/). Vendor BSP (`esp_codec_dev/`, `codec_board/`, `display/`, `i2c_bsp/`, `audio/`, `power/`) is hardware bring-up — leave it alone unless the board changes.

---

## Module map

The Arduino sketch (`forrest_note.ino`) owns `setup()`, `loop()`, and global storage. Everything else sits behind a small header in `src/app/`.

```mermaid
flowchart TB
  subgraph entry [Sketch entry]
    ino["forrest_note.ino setup/loop"]
  end
  subgraph app [App modules]
    fsm[app_fsm]
    flows[flows]
    ui[ui]
    notes[notes]
    rec[record]
    tr[transcribe]
    portal[portal]
    obs[obsidian]
    cfg[config_store]
    https[https]
  end
  subgraph bsp [Board support]
    epd[epaper]
    audio[audio codec]
    pwr[power rails]
  end
  ino --> fsm
  ino --> flows
  ino --> cfg
  fsm --> ui
  fsm --> flows
  flows --> rec
  flows --> tr
  flows --> obs
  flows --> portal
  tr --> notes
  tr --> https
  obs --> notes
  obs --> https
  portal --> notes
  portal --> cfg
  portal --> https
  rec --> notes
  ui --> notes
```

| Module | Owns | Public interface |
|---|---|---|
| [`https`](forrest_note/src/app/https.h) | Mozilla CA bundle + HTTP chunked decode | `httpsAttachCa`, `dechunkBody` |
| [`transcribe`](forrest_note/src/app/transcribe.h) | OpenAI diarize of pending WAVs | `transcribe`, `transcribeAll` |
| [`portal`](forrest_note/src/app/portal.h) | HTTP transfer / provision / OTA / speakers | `setupTransferServer`, `stopTransferMode` |
| [`obsidian`](forrest_note/src/app/obsidian.h) | AI enrich + GitHub Contents + tombstones | `obsidianSyncAll`, `obsidianFlushDeletes` |
| [`flows`](forrest_note/src/app/flows.h) | Record / meeting / sync / transfer | `start*Flow`, `startTransferMode` |
| [`app_fsm`](forrest_note/src/app/app_fsm.h) | Sleep timeout, ticker, battery, buttons | `appHandleLoop` |
| [`notes`](forrest_note/src/app/notes.h) | SD index, tags, `.meta`, vault uid | load/save/delete/meta helpers |
| [`record`](forrest_note/src/app/record.h) | WAV capture + playback | `record`, `playWavFile` |
| [`ui`](forrest_note/src/app/ui.h) | E-paper screens + ticker drawing | `show*`, `serviceDisplay` |
| [`config_store`](forrest_note/src/app/config_store.h) | NVS secrets | `cfg::*` |
| [`serial_cfg`](forrest_note/src/app/serial_cfg.h) | USB `SSID=`/`KEY=`/`GH*` | `handleSerialConfig` |

---

## Boot / provision

Three ways to write config; all go through `cfg::*` (NVS namespace `forrest`).

```mermaid
flowchart TD
  boot[setup]
  boot --> nvs[cfg::begin NVS]
  nvs --> hw[power display I2C audio SD]
  hw --> load[speakers tags index]
  load --> wake{wake source}
  wake -->|PWR held| menu[STATE_MENU]
  wake -->|REC held| recFlow[startRecordFlow]
  wake -->|cold or neither| idle[STATE_IDLE]

  idle --> needWifi{cfg::hasWifi}
  needWifi -->|no| transfer[Settings Transfer]
  transfer --> softap[SoftAP ForrestNote-Setup]
  softap --> portal["portal /provision"]
  portal --> nvs

  needWifi -->|yes| use[Idle / Sync / STA portal]
  usb[USB serial SSID= KEY= GH*] --> nvs
```

- **SoftAP:** Settings → Transfer when no Wi-Fi is stored. Captive DNS `*` → `/provision`.
- **STA portal:** same Transfer menu when Wi-Fi is stored; browse `http://<device-ip>/provision`.
- **USB:** every `loop()` runs `handleSerialConfig()` (`SSID=` then `PASS=`, `KEY=`, `GHTOKEN=`, …).

---

## UI state machine

`AppState` lives in [`types.h`](forrest_note/types.h). **Sync is not a state** — `startSyncFlow()` blocks while `state` stays `STATE_MENU`. **Sleep is not a state** — `enterUltraSleep()` is MCU deep-sleep. `STATE_SAVED` is a brief splash; `STATE_ERROR` is never assigned.

Idle REC uses `handleIdleRec()` (hold vs 3× tap). Every other screen uses `readButtonEvent()`. Meeting *stop* reimplements 3× tap inside `record()` — do not merge those two gesture loops; they share a pattern, not a lifecycle.

```mermaid
stateDiagram-v2
  [*] --> Idle: boot
  [*] --> Menu: wake PWR held
  [*] --> Recording: wake REC held
  Idle --> Recording: hold REC or triple tap
  Idle --> Menu: PWR
  Recording --> TagSelect: WAV saved
  Recording --> Idle: rec fail
  TagSelect --> Sleep: REC confirm tag
  Menu --> NoteList: Notes
  Menu --> TagBrowser: Tags
  Menu --> Menu: Sync blocking subroutine
  Menu --> Settings: Settings
  Menu --> Idle: hold REC
  TagBrowser --> NoteList: pick tag
  NoteList --> NoteDetail: REC
  NoteDetail --> DeleteConfirm: hold PWR
  NoteDetail --> NoteList: hold REC
  Settings --> Transfer: Transfer
  Settings --> DeviceInfo: Device
  Settings --> EraseAll: Erase All
  Settings --> ResetConfirm: Reset
  Transfer --> Settings: hold REC
  Sleep --> Idle: wake neither held
```

Default nav: **PWR tap** = next/cycle, **REC tap** = select, **REC hold** = back. Exceptions: Idle REC is capture; note detail **PWR hold** = delete; tag confirm always sleeps (no path back to idle without deep-sleep).

---

## Record pipeline

```mermaid
flowchart TD
  idle[STATE_IDLE]
  idle -->|hold REC >= REC_HOLD_MS| short[startRecordFlow]
  idle -->|3 taps within REC_TRIPLE_GAP_MS| meet[startMeetingRecordFlow]
  short --> recHold["record true hold-to-talk"]
  meet --> recMeet["record false until 3x tap or 2h"]
  recHold --> dual[core0 I2S producer / core1 SD consumer]
  recMeet --> dual
  dual --> wav["/notes/note_NNN.wav + index row"]
  wav -->|too short| fail[deleteNote IDLE]
  wav -->|ok| tag[STATE_TAG_SELECT]
  tag --> sleep[saveTag then enterUltraSleep]
```

WAV header is rewritten on a crash-safe flush (`REC_FLUSH_MS`) so a power loss still leaves a playable file.

---

## Sync pipeline

Menu → Sync. Wi-Fi STA must already be configured. Vault deletes drain **first** inside `obsidianSyncAll()`.

```mermaid
sequenceDiagram
  participant UI
  participant WiFi
  participant STT as transcribe
  participant AI as obsidian enrich
  participant GH as GitHub Contents
  UI->>WiFi: connect STA
  WiFi->>UI: NTP
  UI->>STT: transcribeAll pending WAVs
  STT->>STT: diarize chunks plus speaker refs
  STT->>UI: note_NNN.txt
  UI->>AI: obsidianSyncAll
  AI->>GH: flush tombs.csv deletes first
  AI->>AI: gpt-4o-mini title summary body topics event
  AI->>GH: PUT VoiceNotes/Title.md
  AI->>GH: update tag MOCs
```

Offline-first: failed diarize stays `hasText=false`; failed push stays `obsidian=0`; failed vault delete stays in `tombs.csv`.

Quirk: a **single** `deleteNote()` also calls `obsidianFlushDeletes()` immediately if Wi-Fi happens to be up. **Erase all** only queues tombs and waits for the next sync.

---

## Portal

One `WebServer` on port 80, started by `startTransferMode()`.

```mermaid
flowchart LR
  settings[Settings Transfer]
  settings -->|no Wi-Fi| ap[SoftAP plus captive DNS]
  settings -->|has Wi-Fi| sta[STA plus NTP]
  ap --> srv[setupTransferServer]
  sta --> srv
  srv --> root["/ notes"]
  srv --> prov["/provision"]
  srv --> ota["/ota"]
  srv --> tags["/tags"]
  srv --> spk["/speakers"]
```

Hold REC to exit (`stopTransferMode()`). OTA uses the same CA bundle as OpenAI/GitHub (`httpsAttachCa`).
