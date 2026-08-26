#pragma once

// Per-loop app FSM: idle timeout, ticker, portal pump, battery warn, button dispatch.
// Called from loop() after serial config and coalesced display service.
// Control flow: see ARCHITECTURE.md "UI state machine".

void appHandleLoop();
