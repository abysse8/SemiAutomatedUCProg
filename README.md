# SemiAutomatedUCProg

ESP32-S3 firmware for presenting PMB update payloads to a UC as a read-only USB mass storage device.

## Modes

- `OFF`: no update disk is exposed.
- `M0`: exposes only files from `/M0` on the SD card.
- `M1`: exposes only files from `/M1` on the SD card.

The firmware defaults to `OFF` if no mode has been saved.

## Serial control

Open the native USB serial port at `115200` baud and type:

```text
m0
m1
off
status
reboot
```

Mode changes are saved and take effect after reboot.

## Button control

Hold the BOOT button while resetting/powering the ESP32 to cycle:

```text
OFF -> M0 -> M1 -> OFF
```

Release the button after the status LED changes.

## SD card layout

The SD card should contain:

```text
/M0/manifest.json
/M0/*.tar.gz
/M1/manifest.json
/M1/*.tar.gz
```

Only regular files in the selected folder are exposed at the virtual USB disk root.
Subfolders such as `/M1/old` are intentionally hidden.

## Important

This requires an ESP32-S2 or ESP32-S3 native USB port. A classic ESP32 USB-UART port cannot act as USB mass storage.
