#pragma once

// USB serial provisioning: SSID=/PASS=/KEY=/GH* line commands into NVS via cfg::.
// Called once per loop from the sketch. Same config sink as the SoftAP/STA portal.

void handleSerialConfig();
