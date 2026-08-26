#pragma once

// Battery ADC + idle-screen ring. Callers: app_fsm (low-batt warn) and ui (idle).

void  batteryInit();
float readBatteryVoltage();
int   batteryPercentFromVoltage(float v);
int   readBatteryPercent();
bool  isBatteryCharging();
void  drawThickArcDot(int cx, int cy, int r, int deg, int thickness, uint8_t color);
void  drawBatteryRing(int percent);
