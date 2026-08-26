#pragma once

// Local HTTP portal: note transfer, tags, speaker library, provisioning, OTA.
// Callers: startTransferMode() (STA or SoftAP) and enterUltraSleep() to stop.
// Handlers are private to portal.cpp. Control flow: see ARCHITECTURE.md "Portal".

void setupTransferServer();
void stopTransferMode();
