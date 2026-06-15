# Wiring Notes

Default SD SPI pins are board-dependent. `platformio.ini` currently only defines:

```ini
-D UC_SD_CS=10
```

The firmware uses Arduino's default SPI pins for the selected ESP32-S3 board plus chip select GPIO 10.

If your SD module is wired differently, update `UC_SD_CS` and, if needed, add explicit SPI pin setup in `src/main.cpp`.

The BOOT button is expected on GPIO 0.

The status LED defaults to GPIO 48, common on some ESP32-S3 boards with an RGB LED. If your board has no LED there, the firmware still works.
