#pragma once
#include <Arduino.h>
#include "WiFiClientSecure.h"

// Shared TLS / HTTP bits used by transcribe, portal (OTA), and obsidian.
// Callers: attach the Mozilla CA bundle before connect; dechunk OpenAI JSON bodies.
// Control flow: see ARCHITECTURE.md "Sync pipeline".

void httpsAttachCa(WiFiClientSecure& client);

// Decode an HTTP/1.1 chunked-transfer body: repeated "<hexsize>\r\n<data>\r\n"
// until a zero-size chunk. OpenAI's chat (and sometimes diarize) replies chunked;
// without this the raw body keeps its chunk markers and fails to JSON-parse.
String dechunkBody(const String& in);
