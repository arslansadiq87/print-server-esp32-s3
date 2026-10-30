#pragma once

// Copy to wifi_credentials.h (excluded by .gitignore).
// Leave Wi-Fi empty to provision through the setup access point.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
// Empty disables HTTP printing. Set a unique random key before building.
// Generate one with: python -c "import secrets; print(secrets.token_hex(32))"
#define X_API_KEY ""
