#include "Arduino.h"
#include "serial_cfg.h"
#include "config_store.h"

void handleSerialConfig() {
  static String line;
  static String pendingSsid;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      line += c;
      if (line.length() > 256) line = "";   // overflow guard
      continue;
    }
    line.trim();
    if (line.length() == 0) { line = ""; continue; }

    if (line.startsWith("SSID=")) {
      pendingSsid = line.substring(5);
      Serial.printf("[cfg] ssid buffered ('%s'); now send PASS=<password>\n", pendingSsid.c_str());
    } else if (line.startsWith("PASS=")) {
      if (pendingSsid.length() > 0) {
        cfg::setWifi(pendingSsid, line.substring(5));
        Serial.printf("[cfg] wifi saved for ssid '%s'\n", pendingSsid.c_str());
        pendingSsid = "";
      } else {
        Serial.println("[cfg] send SSID=<network> first");
      }
    } else if (line.startsWith("KEY=")) {
      cfg::setOpenAiKey(line.substring(4));
      Serial.println("[cfg] openai key saved");
    } else if (line.startsWith("GHTOKEN=")) {
      cfg::setGithubToken(line.substring(8));
      Serial.println("[cfg] github token saved");
    } else if (line.startsWith("GHREPO=")) {
      bool ok = cfg::setGithubRepo(line.substring(7));
      Serial.printf("[cfg] github repo %s\n", ok ? "saved" : "rejected (need owner/name)");
    } else if (line.startsWith("GHBRANCH=")) {
      cfg::setGithubBranch(line.substring(9));
      Serial.println("[cfg] github branch saved");
    } else if (line.startsWith("GHDIR=")) {
      cfg::setGithubDir(line.substring(6));
      Serial.println("[cfg] github dir saved");
    } else if (line == "GHON")  { cfg::setGithubEnabled(true);  Serial.println("[cfg] github sync ON");
    } else if (line == "GHOFF") { cfg::setGithubEnabled(false); Serial.println("[cfg] github sync OFF");
    } else if (line == "SHOW") {
      Serial.printf("[cfg] wifi=%s  openai_key=%s\n",
        cfg::hasWifi() ? cfg::wifiSsid().c_str() : "(none)",
        cfg::hasOpenAiKey() ? "set" : "(none)");
      Serial.printf("[cfg] github=%s branch=%s dir=%s token=%s enabled=%d ai=%d ready=%d\n",
        cfg::githubRepo().length() ? cfg::githubRepo().c_str() : "(none)",
        cfg::githubBranch().c_str(), cfg::githubDir().c_str(),
        cfg::githubToken().length() ? "set" : "(none)",
        cfg::githubEnabled(), cfg::githubAiEnrich(), cfg::hasGithub());
    } else if (line == "RESET") {
      cfg::factoryReset();
      Serial.println("[cfg] factory reset done");
    } else {
      Serial.println("[cfg] cmds: SSID= PASS= KEY= GHREPO= GHBRANCH= GHDIR= GHTOKEN= GHON GHOFF SHOW RESET");
    }
    line = "";
  }
}
