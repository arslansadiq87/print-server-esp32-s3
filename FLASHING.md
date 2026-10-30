# Flash the prebuilt release

Download firmware from the repository's [Releases page](https://github.com/arslansadiq87/print-server-esp32-s3/releases). Use only with an **ESP32-S3 N16R8 (16 MB flash)** and the wiring described in README. Other boards/printers have not been validated.

## Included files

| File | Purpose / flash address |
| --- | --- |
| `tsc-esp32-s3-n16r8-merged.bin` | Complete bootloader/partition/application image; flash at **0x0** |
| `firmware.bin` | Application only; **0x10000**, with matching bootloader/partitions already installed |
| `bootloader.bin` | Bootloader at **0x0** |
| `partitions.bin` | Partition table at **0x8000** |
| `SHA256SUMS.txt` | Checksums of the firmware files |

The firmware ZIP contains all these files and this guide. The merged image is the recommended first-install option. Do not flash the application-only file at 0x0. This is a PlatformIO **release-mode** build, not a debug build. Compilation is verified; physical printing must be verified on your hardware.

## Install with esptool

Install Python and esptool if needed:

```sh
python -m pip install esptool
```

The build tooling used esptool 4.11.0. The commands below use its underscore command spelling; newer versions may display a deprecation message recommending `write-flash`/`erase-flash` instead.

Connect through the board's UART programming port, close serial monitors, and replace `COM7` with the actual port (or Linux/macOS serial path):

```sh
python -m esptool --chip esp32s3 --port COM7 --baud 460800 write_flash 0x0 tsc-esp32-s3-n16r8-merged.bin
```

Use the documented BOOT/RESET sequence if automatic bootloader entry fails. Lower the baud rate if needed. Restart the board after flashing and open the UART monitor at 115200. The native USB pins GPIO 19/20 are reserved for the printer host connection.

Alternatively, a flashing tool that supports ESP32-S3 binary images can write the merged image at 0x0. Do not guess offsets or use images for a different ESP32 variant.

## First boot and capabilities

The release is compiled with **empty Wi-Fi credentials and an empty HTTP API key**. On a fresh device, read the setup AP name/password from UART, join it, and open `http://192.168.4.1/wifi`. Save your 2.4 GHz network, then use the station DHCP address from the log.

RAW TCP port 9100 and Windows RAW printing are available. The web status/setup pages are available. **HTTP label/test printing returns 401 with this public build.** To enable it, build the source with your own random `X_API_KEY` in the ignored credential header; never publish that personal binary. There is no shared public API key.

Flashing normally preserves saved NVS Wi-Fi settings. To deliberately reset an old installation, run the following **before** flashing the merged image. It destroys all existing flash contents and settings:

```sh
python -m esptool --chip esp32s3 --port COM7 erase_flash
```

## Verify downloads

In PowerShell:

```powershell
Get-FileHash .\tsc-esp32-s3-n16r8-merged.bin -Algorithm SHA256
```

Compare with the matching entry in `SHA256SUMS.txt`. On systems with `sha256sum`, run `sha256sum -c SHA256SUMS.txt` from the extracted firmware directory.

Use only on a trusted LAN: RAW has no authentication and network traffic is unencrypted. Read README, HELP, and SECURITY for wiring, printing, and limitations. A successful flash or USB transfer does not confirm physical label printing.
