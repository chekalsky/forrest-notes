#pragma once

// Deep-sleep: paint the sleep screen, cut radios/rails, wake on REC or PWR.
// resetActivity() is called from the button FSM. Sleep is not an AppState.
// Control flow: see ARCHITECTURE.md "UI state machine".

void resetActivity();
void enterUltraSleep();
