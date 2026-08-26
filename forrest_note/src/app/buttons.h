#pragma once
#include "../../types.h"

// Button sampling. readButtonEvent() is the nav FSM (tap vs hold).
// handleIdleRec() is Idle-only: hold ≥ REC_HOLD_MS starts a short note;
// 3× tap starts a meeting. Meeting *stop* is a separate triple-tap inside record().
// Callers: app_fsm (nav), Idle REC. Control flow: see ARCHITECTURE.md "UI state machine".

bool        isDown(int pin);
ButtonEvent readButtonEvent(int pin);
bool        handleIdleRec();
