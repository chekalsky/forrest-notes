#pragma once

// Mic capture to /notes/note_NNN.wav. Dual-core producer/consumer with a PSRAM
// ring so SD stalls don't drop samples. holdMode=true stops on REC release;
// holdMode=false (meeting) stops on 3× REC tap or MAX_MEETING_MS.
// Callers: startRecordFlow / startMeetingRecordFlow. See ARCHITECTURE.md "Record pipeline".

// holdMode=true  → classic press-and-hold note (stops on release)
// holdMode=false → meeting: keeps going until 3× REC tap (or MAX_MEETING_MS)
bool record(bool holdMode = true);
bool playWavFile(const char* path);
