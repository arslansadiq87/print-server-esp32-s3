# Security and credentials

## Deployment boundary

This firmware targets a trusted LAN. HTTP on port 80 is plaintext. RAW on port 9100 is unauthenticated and unencrypted. The API key protects HTTP print submission only, not RAW, status, or job lookup. Do not forward these ports from the internet.

Wi-Fi configuration requires the active setup AP interface. Its password derives from the MAC address and appears in UART logs; it is not a strong independent secret. Current source does not configure encrypted NVS, flash encryption, or secure boot.

## Credential lifecycle

- Keep `main/wifi_credentials.h` local; start from the blank example.
- Generate a unique API key under 128 ASCII characters. An empty key disables HTTP printing.
- Saved NVS Wi-Fi values override compiled defaults. Upload alone normally preserves NVS.
- Rebuild/upload with a new key to change HTTP authentication. This does not clear saved Wi-Fi.
- Flash erase clears device state, but uploading firmware with embedded secrets reintroduces them.
- Firmware, ELF/object files, serial logs, screenshots, and backups can disclose secrets. Never publish personal builds.
- Rotate exposed credentials at their source. Removing repository text cannot revoke them or erase downloaded copies.

## Repository handling

Ignore rules exclude the private header, build output, generated configuration, editor settings, logs, environment files, and common private-key extensions. Shared `sdkconfig.defaults` is explicitly included.

Ignore rules do not untrack committed files or sanitize history. This workspace initially had no `.git` directory, so previous GitHub history could not be inspected. Review an existing remote separately. Rotate exposed secrets before coordinating any necessary historical cleanup with collaborators.

The source packager uses an allowlist, checks the public template is blank, and detects a few common credential patterns. It is a basic guard, not a complete secret scanner. Review the archive and enable repository secret scanning where available.

## Reporting

Do not put working credentials in public issues. Use private repository security reporting if enabled, or an available private contact for the owner. No contact address or support guarantee has been invented for this project.
