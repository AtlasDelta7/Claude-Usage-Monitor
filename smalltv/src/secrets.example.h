#pragma once
// Copy this file to secrets.h (same folder) and fill in your own values.
// secrets.h is git-ignored, so your passwords never get committed.
// Use long random values, e.g. run  [guid]::NewGuid()  in PowerShell for each.

#define SETUP_AP_PASS "change-me-8-plus-chars"   // password for the "SmallTV-Setup" Wi-Fi hotspot
#define OTA_PASS      "change-me-ota-password"   // password for wireless firmware updates
#define PUSH_TOKEN    "change-me-push-token"     // shared secret with bridge/config.json "push_token"
