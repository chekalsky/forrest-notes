#pragma once

// User-facing blocking flows: record, meeting, sync, transfer/portal.
// Called from the idle REC gestures, the menu/settings FSM, and boot-from-sleep.
// Sync is a subroutine (state stays STATE_MENU). Control flow: see ARCHITECTURE.md.

void startRecordFlow();
void startMeetingRecordFlow();
void startSyncFlow();
void startTransferMode();
