#pragma once
#include <Arduino.h>

// E-paper screens, icons, and coalesced redraw. One module on purpose (screens
// share layout helpers). Callers: flows, app_fsm, transcribe/obsidian progress.
// Control flow: see ARCHITECTURE.md "UI state machine".

// Icons
void iconMicWhite(int cx, int cy);
void iconRecordBig(int cx, int cy);
void iconCheck(int cx, int cy, bool filled);
void iconError(int cx, int cy);
void iconThinking(int cx, int cy);
void iconTag(int cx, int cy);
void iconSync(int cx, int cy);
void iconWifi(int cx, int cy);
void iconNoteLines(int cx, int cy);

// Layout helpers
void drawHeader(const char* title, const char* rightInfo = nullptr);
void drawHints(const char* recLabel, const char* pwrLabel);
void drawBadge(int cx, int cy, const char* text, bool filled);
void drawPageDots(int cur, int total);
void drawChevronRight(int x, int cy, uint8_t c);
void drawTinyHint(const char* left, const char* right);
void drawKicker(const char* txt, int y);
void drawSoftFrame();
void drawProductWordmark(int cx, int y, uint8_t color);
void drawModernPill(int x, int y, int w, int h, const char* label, bool active);
void drawDotSelector(int cur, int total, int y);
void drawCheckSmall(int cx, int cy, uint8_t color);
void drawMinimalDocIcon(int cx, int cy, uint8_t color);
void drawMinimalTagIcon(int cx, int cy, uint8_t color);
void drawMinimalCloudIcon(int cx, int cy, uint8_t color);
void drawMenuTile(int x, int y, int w, int h, const char* label, int icon, bool active);
void drawNoteCard(int y, int idx, bool active);
void drawListMenuCard(int y, const char* title, const char* meta, bool active);
bool activeTickerNeedsScroll(int cursor);
void drawTickerText(int x, int y, int maxW, const String& rawText, bool active, uint8_t color);

// Screens
void showIdle();
void showBatteryLow(int pct);
void showRecording();
void showMeetingRecording();
void showRecordingLive(uint32_t elapsedMs, int level, bool meeting = false);
void showSaved(int num);
void showTagSelect(int cursor);
void showMenu(int cursor);
void showTagBrowser(int cursor);
void showNoteList(int cursor);
void showNoteDetail(int cursor);
void showDeleteConfirm(int noteNum);
void showTranscribing(int done, int total);
void showWifiConnecting(int attempt, int maxA);
void showDone();
void showError(const char* msg);
void showUltraSleepScreen();
void showPlaybackOverlay();
void showTransferConnecting();
void showTransferMode(const char* ip);
void showSettings(int cursor);
void showDeviceInfo();
void showResetConfirm();
void showResetDone();
void showDeleteAllConfirm(int count, int cursor);
void showDeleteAllDone(bool alsoVault);
void showObsidianSync(int done, int total);

// Sync status: phase, %, note line, optional chunk line, process name, note counts.
// Note counts display as 1-based ("1 of 4 notes" while working on the first).
// During long waits call syncProgressPulse() — refreshes at most every 5s with a pizza loader.
void showSyncProgress(const char* phase, int percent,
                      const char* noteDetail, const char* chunkDetail,
                      const char* process, int done = -1, int total = -1);
void syncProgressPulse();
void syncProgressEnd();

void redrawCurrentScreen();   // repaint the current state's screen
void serviceDisplay();        // run each loop: paints pending redraws when panel is free
