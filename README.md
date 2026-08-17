# ESP32 Atari Emulator + PS2 keyboard + SD Launcher

ESP32 project containing an Atari emulator and a small SD-card launcher. The launcher can browse Atari images on an SD card, copy selected games to SPIFFS, remove stored games, format SPIFFS, and then restart into the Atari application.

## Repository layout

- `atari/` - Atari emulator project and bundled SPIFFS game data.
- `launcher/` - SD/SPIFFS launcher project.
- `atari/data/atari800/` - Atari images included in the SPIFFS filesystem image.
- `atari/bin/` - prebuilt flashing files included in the original project archive.

## Hardware / pins

The current launcher sources use:

- ESP32 (`esp32dev`)
- PS/2 keyboard DATA: GPIO 32
- PS/2 keyboard CLOCK: GPIO 33
- SD SCK: GPIO 18
- SD MISO: GPIO 19
- SD MOSI: GPIO 23
- SD CS: GPIO 21
- Composite video: ESP32 DAC video path (GPIO 25 in `video_out.h`)
- Audio PWM: GPIO 27

Check the source before wiring hardware, especially if you modify the video or SD configuration.

## Software requirements

- PlatformIO
- Espressif32 / Arduino framework
- Python + esptool for manual flashing

The projects pull `PS2KeyAdvanced` from GitHub through PlatformIO.

## Build

### Launcher

```bash
cd launcher
pio run -e esp32Launcher
```

### Atari emulator

```bash
cd atari
pio run -e esp32Atari
```

## Partition layout

Both projects use the custom `partitions.csv` layout:

| Partition | Offset | Size |
|---|---:|---:|
| launcher | `0x10000` | `0x80000` |
| atari | `0x90000` | `0x100000` |
| spiffs | `0x190000` | `0x270000` |

The table above is based on the supplied partition CSV. Keep the projects on the same partition layout when building/flashing the combined firmware.

## Flashing

The supplied Windows batch files contain machine-specific paths and a hard-coded `COM6`. Edit those values for your computer before using them.

The original combined Atari flashing script expects binaries at these offsets:

```text
0x8000   partitions.bin
0x10000  launcher.bin
0x90000  atari.bin
0x190000 spiffs.bin   (optional/commented in the original script)
```

Do not blindly flash prebuilt binaries to a different ESP32/partition setup. Build for your board and verify the offsets first.

## Launcher usage

At startup the launcher waits briefly for **F12**. If F12 is not detected, it boots directly into Atari. If F12 is held/detected, the SD launcher starts.

Main controls:

- **Arrow Up / Down** - move selection
- **Page Up / Page Down** - move by a page
- **Insert** - mark/unmark a game for copying
- **Enter** - open a directory or copy marked/selected game(s)
- **Delete** - open manual SPIFFS delete screen
- **Esc** - go back; from the root, start Atari
- **F2** - SPIFFS format dialog

The launcher first looks for `/atari800` on the SD card and otherwise starts at the SD root. It handles ATR/XEX-style Atari game files according to the source implementation.

## SD card

For the simplest layout, put games in:

```text
/atari800/
```

The launcher copies selected files into the SPIFFS `/atari800/` directory. It reserves free space and can ask you to remove existing SPIFFS games when necessary.

## Notes for GitHub

- `.pio/` build output is ignored by the included `.gitignore` files.
- Review bundled ROM/game images before publishing. Make sure you have the right to redistribute every binary/image in `atari/data/atari800/` and `atari/bin/`.
- The batch files currently contain a local Windows user path. Replace it with a portable command or document your local setup before publishing.

## License

No repository-wide license was included in the supplied archives. Before publishing, add an appropriate `LICENSE` file and preserve any upstream license/copyright notices from incorporated emulator code and libraries.
