#pragma once

// Battery ADC + idle-screen ring. Callers: app_fsm (low-batt warn) and ui (idle).

void  batteryInit();
float readBatteryVoltage(int samples = 16);
int   batteryPercentFromVoltage(float v);
int   readBatteryPercent(int samples = 16);
bool  isBatteryChargingAt(float v);
bool  isBatteryCharging();
void  drawThickArcDot(int cx, int cy, int r, int deg, int thickness, uint8_t color);
void  drawBatteryRing(int percent);
