# Labplus mPython Pro (掌控板 3.0)

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| Chip | ESP32-S3, dual-core LX7 @ 240 MHz |
| Flash | 16 MB quad NOR (GD25Q128ES1G), QIO 80 MHz |
| PSRAM | 8 MB quad SPI (ESP-PSRAM64), 80 MHz |
| Display | ST7789 1.47" 320x172 IPS over SPI |
| Audio codec | ES8388 (I2S + I2C) |
| Speaker amp | NS4150 class-D |
| Microphone | Dual onboard analog mics (through ES8388) |
| LED | 3x WS2812 (RGB) |
| Sensors | QMI8658C 6-axis, MMC5603NJ magnetometer, LTR-308ALS light |
| Buttons | A (GPIO0), B (GPIO46), 6 capacitive touch keys |
| Console | USB Serial/JTAG |

## GPIO Mapping

| Function | GPIO | Notes |
|----------|------|-------|
| LCD SCK | 36 | |
| LCD MOSI (SDA) | 37 | |
| LCD CS | 34 | |
| LCD DC (RS) | 35 | |
| LCD backlight | 33 | LEDC PWM |
| I2C SCL | 43 | Shared with onboard sensors and ES8388; pad P19 |
| I2C SDA | 44 | Shared with onboard sensors and ES8388; pad P20 |
| I2S MCLK | 39 | |
| I2S BCLK | 41 | |
| I2S WS (LRCK) | 42 | |
| I2S DOUT | 38 | ESP32-S3 -> ES8388 DIN |
| I2S DIN | 40 | ES8388 DOUT -> ESP32-S3 |
| WS2812 data | 8 | 3 LEDs, RMT |
| Button A | 0 | Pad P5, active low |
| Button B | 46 | Pad P11, active low |
| Touch keys P/Y/T/H/O/N | 9, 10, 11, 12, 13, 14 | Not modelled as a device |
| Buzzer | 21 | Pad P12, not modelled as a device |
| Sound sensor | 6 | Pad P10, analog |

## Build & Flash

Requires ESP-IDF v5.5.x and the ESP Board Manager helper package.

```bash
pip install esp-bmgr-assist

cd application/edge_agent

# 1. Confirm the board is discovered
idf.py bmgr -c ./boards -l

# 2. Select the board. ESP Board Manager picks the chip itself, so do NOT run
#    `idf.py set-target` (it would wipe sdkconfig and undo the board selection).
idf.py bmgr -c ./boards -b mpython_pro

# 3. Build
idf.py build

# 4. Erase first when the board still runs vendor firmware (MicroPython or the
#    xiaozhi image). Those ship a completely different partition layout, so
#    leftovers would collide with the ESP-Claw table.
idf.py -p <PORT> erase-flash

# 5. Flash and monitor
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with the USB Serial/JTAG port: `COMx` on Windows,
`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux.

### Download mode

The board has no USB-UART bridge; the console runs on the ESP32-S3 native USB
Serial/JTAG peripheral, which esptool can normally reset into download mode by
itself. If the port does not appear or flashing cannot sync, enter download mode
manually: button A is wired to the GPIO0 boot strapping pin, so

1. hold **A**,
2. press and release **Reset**,
3. release **A**.

Exit `idf.py monitor` with `Ctrl+]`.

## Pin Source Notes

Pin assignments come from the official 掌控板3.0 hardware documentation
([1.3 引脚排布及功能](https://mpython-esp32s3-doc.readthedocs.io/zh-cn/latest/1_hardware/1_3_IO.html))
and are cross-checked against the vendor MicroPython firmware
([labplus-cn/mpython_esp32s3](https://github.com/labplus-cn/mpython_esp32s3),
`port/boards/mpython_pro/`).

Two upstream inconsistencies are worth recording, because they affect this board
definition:

1. **PSRAM must be quad, not octal.** An octal PSRAM part on ESP32-S3 consumes
   GPIO33..37, which this board uses for the LCD and backlight. That would leave
   the SoC with no free pins for the panel at all, so the 8 MB PSRAM part is quad
   (`ESP-PSRAM64`), matching the vendor's per-board non-octal `sdkconfig.spiram`.
   `sdkconfig.defaults.board` therefore sets `CONFIG_SPIRAM_MODE_QUAD=y`.
2. **The vendor `mpconfigboard.h` I2C pins are stale.** It declares
   `MICROPY_HW_I2C0_SCL=35` / `SDA=34`, which collide with the documented LCD
   CS/DC pins (34/35), and the same file's `bsp_audio_board.h` is byte-identical
   to the sibling `labplus_Ledong_v2` board (its I2C block still carries a
   `labplus_classroom_kit` comment and its `bsp_i2c_init()` body is commented
   out). The documented external/internal I2C bus on GPIO43/44 is used here
   instead, which matches `labplus_Ledong_v2`.

The I2S pin group (MCLK=39, BCLK=41, WS=42, DOUT=38, DIN=40) is confirmed by both
sources and is not in doubt.

## Not Wired Up

The following onboard hardware is intentionally not declared as a board device
because esp-claw's board manager has no matching device type yet:

- The six capacitive touch keys (GPIO9..14).
- The piezo buzzer (GPIO21).
- The QMI8658C / MMC5603NJ / LTR-308ALS sensors on the shared I2C bus. The bus
  itself is declared, so they can be driven from Lua or a capability.

## Display Orientation

`swap_xy: true` plus `mirror_x: true` presents the natively portrait 172x320
glass as the documented 320x172 landscape. If the image comes up mirrored or
rotated on real hardware, flip `mirror_x` / `mirror_y` in `board_devices.yaml`;
no other LCD setting should need to change.

## Files

| File | Description |
|------|-------------|
| `board_info.yaml` | Board identity (chip, manufacturer) |
| `board_peripherals.yaml` | I2C, SPI, I2S, LEDC, RMT and GPIO pin configuration |
| `board_devices.yaml` | LCD, brightness, audio codec, LED strip and button devices |
| `sdkconfig.defaults.board` | Board-level sdkconfig defaults |
| `setup_device.c` | ST7789 panel factory entry point for the SPI display path |
