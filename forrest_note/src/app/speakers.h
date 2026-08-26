#pragma once
#include <Arduino.h>

struct Speaker {
  int  id;
  char name[32];
};

void speakersEnsureDir();
void speakersLoad();
int  speakersCount();
const Speaker* speakersAt(int index);          // 0..count-1
const Speaker* speakersFindId(int id);
String speakersWavPath(int id);

// Copy srcWav into the library (truncated to SPEAKER_REF_MAX_MS). Returns new id or -1.
int  speakersAdd(const char* name, const char* srcWavPath);
bool speakersRename(int id, const char* name);
bool speakersDelete(int id);

// Fill out[0..maxOut) with up to MAX_KNOWN_SPEAKER_REFS speakers (first N). Returns count.
int  speakersPickForDiarize(Speaker* out, int maxOut);
