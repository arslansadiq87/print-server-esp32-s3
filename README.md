# ESP32-S3 Wi-Fi Print Server for TSC TTP-244 Pro

Turn an ESP32-S3 N16R8 into a local Wi-Fi-to-USB bridge for a TSC TTP-244 Pro. Print short text labels from a browser or send printer-ready bytes through RAW TCP port 9100, including from a Windows printer driver.

The firmware uses ESP-IDF through PlatformIO and targets USB **VID:PID 1203:0172**. Other printers require code changes and hardware validation. Successful USB delivery does not confirm physical printing: inspect the actual label.

**Prebuilt firmware:** download from [GitHub Releases](https://github.com/arslansadiq87/print-server-esp32-s3/releases) and follow [FLASHING.md](FLASHING.md). The credential-free public build supports Wi-Fi setup and RAW/Windows printing. HTTP printing requires your own build with a private API key.

## Contents

- [Features](#features)
- [Hardware and wiring](#hardware-and-wiring)
- [Software requirements](#software-requirements)
- [Credentials](#credentials)
- [Build and upload](#build-and-upload)
- [Wi-Fi setup](#wi-fi-setup)
- [Browser printing](#browser-printing)
- [HTTP API](#http-api)
- [RAW printing](#raw-printing)
- [Windows installation](#windows-installation)
- [Customization](#customization)
- [Architecture and files](#architecture-and-files)
- [Troubleshooting](#troubleshooting)
- [Security and publication](#security-and-publication)

## Features

| Feature | Behavior |
| --- | --- |
| Wi-Fi | 2.4 GHz station, DHCP, setup AP fallback |
| Browser | Status, text label, test label, Wi-Fi setup |
| HTTP | Port 80; API key required for print submission |
| RAW | TCP 9100; unchanged bytes; one active client; no authentication |
| USB | Native USB Host; interface 0, alternate 0; discovered Bulk OUT endpoint |
| Default media | 50 x 25 mm, 3 mm gap |
| HTTP text | 1-30 printable ASCII characters; quotes/backslashes rejected |
| Buffering | Four shared queue items; 512-byte RAW chunks |
| HTTP history | Eight reusable in-memory job slots |
| RAW job boundary | Clean TCP EOF or 30 idle seconds |
| Persistence | Wi-Fi in NVS; job history/counters reset at reboot |

There is no TLS, SNMP, persistent print spool, physical printing acknowledgment, general document converter, or implemented OTA update flow. RAW data must already use a printer-supported format. The 8 MB PSRAM is intentionally disabled because the project configuration records a prior startup fault when enabling it.

## Hardware and wiring

You need an ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM), TSC TTP-244 Pro, suitable media, USB host wiring/breakout, regulated VBUS power, and a UART programming/console connection.

| ESP32-S3 / supply | USB connection |
| --- | --- |
| GPIO 20 | D+ |
| GPIO 19 | D- |
| Regulated 5 V supply | VBUS |
| Common ground | GND |

Keep data wires short and check the board schematic. GPIOs are not a 5 V source. The printer needs its normal power supply; the USB connection still needs correct host VBUS. Avoid backfeeding power between USB connectors and external supplies.

Native USB Serial/JTAG and USB OTG host share a PHY. Use a separate USB-to-UART connection or appropriate UART adapter while GPIO 19/20 serve the printer. Console baud rate is **115200**. Connector routing varies by board; a USB label alone does not establish host wiring.

The known printer has Bulk OUT endpoint `0x01` and 64-byte maximum packets. Firmware discovers the endpoint from descriptors and accepts the configured VID/PID only.

## Software requirements

- PlatformIO Core or VS Code with PlatformIO. Use a terminal where `pio` works.
- Python 3 for key generation and the RAW helper; no third-party Python packages required.
- Your board's USB-to-UART driver and a working serial port.
- Internet access on first build to download tools/framework.
- The manufacturer's TSC TTP-244 Pro driver for Windows printing.

`platformio.ini` pins **espressif32 6.13.0**, framework **espidf**, and custom board `esp32-s3-n16r8`. Use this build path. A separate ESP-IDF installation is not required and can have a different configuration/version.

## Credentials

Create the ignored local header before building.

PowerShell:

```powershell
Copy-Item main/wifi_credentials.example.h main/wifi_credentials.h
python -c "import secrets; print(secrets.token_hex(32))"
```

Linux/macOS:

```sh
cp main/wifi_credentials.example.h main/wifi_credentials.h
python3 -c 'import secrets; print(secrets.token_hex(32))'
```

Edit `main/wifi_credentials.h` locally:

```c
#pragma once
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define X_API_KEY "PASTE_GENERATED_KEY_HERE"
```

Paste the generated value, not the example text. The command generates 64 random hexadecimal characters. Keys must be fewer than 128 ASCII characters. **An empty key disables HTTP printing; RAW printing remains available.**

Leave Wi-Fi empty to provision through the setup AP. Alternatively, enter your 2.4 GHz network before compiling. These are C string literals, so escape embedded quotes/backslashes. Existing NVS settings override compiled Wi-Fi defaults; changing the header alone does not replace saved settings.

Never publish the local header or personal build artifacts. Credentials can appear in `.bin`, `.elf`, and object files. The browser asks for the API key at runtime and does not store it in browser storage.

## Build and upload

From the repository root after creating the local header:

```sh
pio run -e esp32-s3-n16r8
pio device list
pio run -e esp32-s3-n16r8 --target upload
pio device monitor --baud 115200
```

To select a port explicitly, replace `COM7` with the actual port:

```sh
pio run -e esp32-s3-n16r8 --target upload --upload-port COM7
pio device monitor --port COM7 --baud 115200
```

Linux/macOS use their actual serial device path. Close other serial monitors before upload. Use the board's documented BOOT/RESET sequence if needed. Upload speed is 921600; lower `upload_speed` in `platformio.ini` for an unreliable serial link.

Clean rebuild:

```sh
pio run -e esp32-s3-n16r8 --target clean
pio run -e esp32-s3-n16r8
```

A normal build does not flash hardware. `.pio/` and `sdkconfig.esp32-s3-n16r8` are generated local artifacts. **Commit `sdkconfig.defaults`**, the shared configuration. Existing generated SDK settings can retain old values when defaults change.

## Wi-Fi setup

1. Power both devices, connect USB, and open the UART monitor.
2. If saved/compiled credentials work, the log prints the station DHCP address and HTTP URL.
3. With no usable credentials, or after roughly 20 seconds without station connectivity, the device starts a WPA2 AP named `TSC-Setup-` plus a device suffix.
4. Read the SSID/password from the serial log. Join that AP and stay connected even without internet.
5. Open **http://192.168.4.1/wifi** manually.
6. Enter your 2.4 GHz SSID/password and save. SSIDs accept 1-32 bytes without controls. Passwords are empty for open networks or 8-64 bytes; actual association depends on valid access-point credentials.
7. Settings are saved to NVS. Successful station association stops the AP. Rejoin your normal network and open the station address printed in the log.

Configuration writes require the active setup AP interface; requests through the station address are rejected. The home Wi-Fi password is not logged or returned in status. The setup password is logged locally and derived from the MAC address; it is not a strong independent secret.

After later station failure the device retries and starts the AP after the timeout. To change networks, make the old network unavailable and use that AP. Reserve the DHCP lease in your router for a stable address.

### Full reset

Ordinary flashing normally preserves NVS. These commands intentionally erase **all flash**, including saved settings and firmware, then upload again. Use only when resetting the connected board:

```sh
pio run -e esp32-s3-n16r8 --target erase
pio run -e esp32-s3-n16r8 --target upload
```

Empty compiled Wi-Fi defaults before rebuilding if you want AP provisioning after reset. Editing repository files does not erase credentials on an already-flashed device.

## Browser printing

Open `http://DEVICE_IP/` on your trusted network. Throughout this guide, replace `DEVICE_IP` with the ESP32's current DHCP address.

Enter the configured API key, type a short label, and select **Print label**. **Print test label** submits `ESP32-S3 USB TEST`. The page displays Wi-Fi, USB readiness, RAW state, and counters. Inspect actual output even when a job reports success.

Default HTTP TSPL uses font `2`, position `32,32`, speed `2`, density `8`, direction `1`, and one copy. Match label size/gap to your media.

## HTTP API

Base URL: `http://DEVICE_IP`. HTTP is unencrypted. Only the two print endpoints require `X-API-Key`; pages, status, and HTTP job lookups do not.

| Method/path | Required input | Result |
| --- | --- | --- |
| `GET /` | None | Printing page |
| `GET /wifi` | None | Setup page |
| `POST /wifi/config` | Active setup AP interface; JSON `ssid`, `password` strings | Save and reconnect |
| `GET /api/status` | None | State/cumulative RAW counters |
| `POST /api/print` | API key; JSON `text` string | `202`, job ID |
| `POST /api/print-test` | API key; JSON object `{}` | `202`, job ID |
| `GET /api/jobs/<id>` | Numeric ID | Job status or `404` |

### PowerShell example

Set your address and enter the key at the prompt. Avoid saving real keys in scripts/transcripts.

```powershell
$printerUrl = 'http://DEVICE_IP'
$secureKey = Read-Host 'Printer API key' -AsSecureString
$credential = [System.Net.NetworkCredential]::new('', $secureKey)
$headers = @{ 'X-API-Key' = $credential.Password }
Invoke-RestMethod "$printerUrl/api/status"
$body = @{ text = 'Hello printer' } | ConvertTo-Json -Compress
$job = Invoke-RestMethod "$printerUrl/api/print" -Method Post -Headers $headers -ContentType 'application/json' -Body $body
Invoke-RestMethod "$printerUrl/api/jobs/$($job.job_id)"
Invoke-RestMethod "$printerUrl/api/print-test" -Method Post -Headers $headers -ContentType 'application/json' -Body '{}'
Remove-Variable secureKey, credential, headers
```

Example acceptance response: `{"job_id":1,"status":"queued"}`. Poll until `usb_transferred` or `failed`; intermediate states are `queued` and `sending`. Slots can be reused and history disappears at reboot. USB success confirms bytes delivered, not physical printing.

### Input and errors

Both print POSTs require `Content-Type: application/json`, an object body, and at most 512 bytes. The test endpoint still requires `{}`. Text is 1-30 printable basic ASCII bytes excluding quotes/backslashes; controls, newlines, and non-ASCII are rejected to keep them out of TSPL commands.

| Status | Typical cause |
| --- | --- |
| `400` | Invalid JSON/text/job ID or incomplete request |
| `401` | Missing/wrong API key or empty compiled key |
| `403` | Wi-Fi write outside active setup AP interface |
| `404` | Unknown/reused job ID |
| `408` | Request-body timeout |
| `413` | Body over 512 bytes |
| `415` | Unsupported/missing content type |
| `429` | Queue or job table full |
| `503` | Printer unavailable or RAW owns print source |
| `500` | Internal failure; inspect serial logs |

Errors normally contain an `error` string. Retry `429`/`503` after recovery. Retrying an uncertain submission can print twice; there is no idempotency token.

### Status fields

| Fields | Meaning |
| --- | --- |
| `wifi_connected`, `wifi_setup_mode` | Station/setup state |
| `wifi_ip`, `wifi_ap_ip`, `wifi_ap_ssid` | Addresses/setup name; no password |
| `printer_connected`, `printer_ready_for_transfer` | USB presence/readiness |
| `printer_vid`, `printer_pid`, `printer_model` | Configured target identity |
| `queue_length` | Outstanding HTTP jobs plus pending RAW stream, not raw chunk count |
| `raw_tcp_listening`, `raw_tcp_client_connected`, `raw_tcp_stream_pending`, `raw_tcp_port` | RAW service state |
| `raw_tcp_received_bytes`, `raw_tcp_queued_bytes`, `raw_tcp_usb_bytes` | RAW socket/queue/USB totals |
| `raw_tcp_dropped_bytes` | Bytes discarded by failure handling |
| `raw_tcp_jobs_completed`, `raw_tcp_jobs_failed` | RAW stream totals |
| `raw_tcp_last_error` | Most recently recorded RAW error |

Counters are cumulative since boot and exclude HTTP labels. Counts differ while queues drain. For an isolated successful RAW job, received/queued/USB byte increments should converge and dropped bytes should not increase. Client disconnection does not mean USB processing is complete.

## RAW printing

```sh
python scripts/raw9100_test.py DEVICE_IP
python scripts/raw9100_test.py DEVICE_IP --large
python scripts/raw9100_test.py DEVICE_IP --file path/to/printer-data.bin
python scripts/raw9100_test.py --help
```

The default sends one TSPL label. `--large` streams 1 MiB of blank CRLF lines then one label to exercise backpressure. `--file` forwards an existing file unchanged in 4096-byte reads. These options are mutually exclusive. `--port` changes the helper destination; firmware remains at 9100 unless rebuilt with another `RAW_TCP_PORT`.

The helper shuts down its write side after sending. Clean EOF or 30 idle seconds ends a firmware stream; TCP packets are not job boundaries. One active client holds source ownership. Extra clients are closed, HTTP printing can return `503`, and slow USB applies backpressure to socket reads.

The helper's `Sent` output confirms socket sending only. Check status counters, logs, and physical output. No application-level acknowledgment is returned. Interrupted streams can leave partial output. PDF/image/plain text files are not automatically converted into printer commands.

## Windows installation

1. Install the manufacturer's TSC TTP-244 Pro driver.
2. Open printer settings and choose manual installation by IP address/hostname; exact labels vary by Windows release.
3. Enter the ESP32 station IP as a TCP/IP device. Use a port name such as `TSC_ESP32_RAW`.
4. In custom port settings select **RAW**, port **9100**.
5. Disable **SNMP Status Enabled**. Avoid bidirectional features that require printer replies.
6. Select the TSC driver and correct stock size, gap, orientation, and media settings.
7. Print one small label and inspect it.

Reserve the DHCP address in your router. This path does not use the HTTP API key. Keep the device on a trusted LAN without internet port forwarding.

## Customization

| Setting | Location | Notes |
| --- | --- | --- |
| HTTP media | `LABEL_WIDTH_MM`, `LABEL_HEIGHT_MM`, `LABEL_GAP_MM` in `main/main.c` | Update helper TSPL separately |
| Font/position/speed/density | `build_tspl_payload()` | Stay within `MAX_PRINT_PAYLOAD` |
| VID/PID | `inspect_device()` and status fields | Update filter/display together; test descriptors |
| Interface/alternate | `PRINTER_INTERFACE_NUMBER`, `PRINTER_ALT_SETTING` | Match target descriptors |
| RAW port/timeout | `RAW_TCP_PORT`, `RAW_IDLE_TIMEOUT_MS` | Update clients |
| Queue/text/body limits | Constants near top of `main/main.c` | Review RAM, UI, validation, payload sizing together |
| Hardware/upload | Board JSON and `platformio.ini` | Match actual board |
| SDK defaults | `sdkconfig.defaults` | Review existing generated config on rebuild |

HTTP media constants do not modify RAW jobs. Windows/RAW data supplies its own layout. PSRAM changes need independent validation.

## Architecture and files

HTTP handlers build bounded TSPL jobs. RAW enqueues chunks and stream markers. Both feed one queue; a USB client task owns interface claims and Bulk OUT transfers. A separate USB library task handles events. Mutexes coordinate state/source ownership so bytes cannot interleave.

```text
HTTP --> TSPL jobs ---+
                     +--> shared queue --> USB owner --> printer
RAW --> byte chunks -+
```

| Path | Purpose |
| --- | --- |
| `main/main.c` | Firmware and embedded pages |
| `main/CMakeLists.txt` | Component registration |
| `main/wifi_credentials.example.h` | Blank public template |
| `main/wifi_credentials.h` | Ignored local secrets |
| `platformio.ini` | Pinned build environment |
| `boards/esp32-s3-n16r8.json` | N16R8 board definition |
| `sdkconfig.defaults` | Shared SDK configuration |
| `partitions.csv` | NVS, PHY, 4 MB factory app, 1 MB FAT storage |
| `CMakeLists.txt` | Project entry; historical name `usb_file_list` |
| `dependencies.lock` | Existing component lock metadata |
| `scripts/raw9100_test.py` | Standard-library RAW sender |
| `scripts/package_source.py` | Allowlisted source archive builder |
| `HELP.md` | Diagnostic workflow and issue template |
| `SECURITY.md` | Credentials/network boundaries |
| `PUBLISHING.md` | GitHub publication instructions |

The existing lock includes historical `usb_host_msc` metadata. Current code uses the ESP-IDF USB Host API directly and does not mount USB disks. `managed_components/` is dependency content, not source to upload. The FAT partition is unused by printing; there is no OTA partition pair.

## Troubleshooting

| Symptom | Checks |
| --- | --- |
| Missing credential header | Copy the template before building |
| `pio` unavailable | Use PlatformIO terminal or fix PATH |
| Build failure | Check network downloads and first compilation error |
| Upload timeout | UART/cable, close monitors, BOOT/RESET, lower upload speed |
| No console | UART at 115200; native USB serves host mode |
| Setup unreachable | Join setup AP, stay connected without internet, open HTTP address manually |
| Old Wi-Fi persists | NVS overrides header; configure through AP or intentionally erase flash |
| Station fails | Check 2.4 GHz SSID/password, signal, AP settings, logs |
| USB absent | Check power/VBUS/ground, D+/D-, cable, VID/PID |
| USB not ready | Check interface/alternate and Bulk OUT descriptors |
| `401` | Nonempty local key, rebuild/upload, exact matching input |
| `429`/`503` | Wait for queue/RAW or restore USB readiness |
| Windows offline | Stable IP, RAW 9100, SNMP disabled, correct driver |
| Blank/misaligned label | Match stock/gap/language; calibrate using printer instructions |
| RAW timeout | Check network and USB progress; backpressure can delay sends |
| USB success but no paper | Check printer/media/language; transfer is not print confirmation |

See [HELP.md](HELP.md) for the diagnostic procedure and issue report template.

## Security and publication

Use only on a trusted LAN. HTTP/RAW are unencrypted and RAW has no authentication. The setup password derives from device identity. The HTTP key protects submission only. See [SECURITY.md](SECURITY.md).

Follow [PUBLISHING.md](PUBLISHING.md) to create a clean source archive. Keep personal credentials, firmware builds, editor settings, and logs out of GitHub. Rotate any previously exposed secret; removing a file does not remove old commits/downloads.

Contributions: see [CONTRIBUTING.md](CONTRIBUTING.md). No original-source license has been chosen in this folder. The owner should add their chosen license before promising open-source reuse rights. Public visibility alone is not a license. Dependencies retain their own licenses. This is not an official TSC or Espressif product.
