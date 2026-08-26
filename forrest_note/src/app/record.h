#pragma once

// holdMode=true  → classic press-and-hold note (stops on release)
// holdMode=false → meeting: keeps going until 3× REC tap (or MAX_MEETING_MS)
bool record(bool holdMode = true);
bool playWavFile(const char* path);
