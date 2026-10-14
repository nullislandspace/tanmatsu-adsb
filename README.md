# ADS-B Radar Display (Tanmatsu app)

Live ADS-B aircraft tracker ported from [cavac/adsb](https://github.com/cavac/adsb) (ESP32-P4
touchscreen board) to Tanmatsu. Aircraft positions are polled from
[api.adsb.lol](https://api.adsb.lol) and enriched with aircraft type, operator and route data,
then drawn on four swipeable views.

There is no SDR in this app - it is a network client, not a receiver.

## Views

- **Map** - top-down map with aircraft icons, altitude-coloured trails and pre-rendered map
  backgrounds at several zoom levels
- **Radar** - rotating sweep with phosphor-style blips that paint callsign, route, altitude and
  speed as the sweep passes each aircraft
- **Arrivals** - split-flap style board with animated character flips
- **Stats** - heap/PSRAM/temperature/FPS, network latency and RSSI, session totals

All views share the same filter set (COM / MIL / EMG / HELI / FAST / SLOW / ODD), range settings
(150 / 100 / 50 / 20 / 5 / 1 nm) and an optional auto-cycle between views.

## Controls

Tanmatsu has no touch screen, so everything runs through the navigator keys. The app has two
lanes: the view lane (default) and the widget lane (active while the settings panel is open).

| Key | View lane | Settings open |
|-----|-----------|---------------|
| Left / Right | previous / next view | move between widgets |
| Up / Down | cycle aircraft | move between widgets |
| Tab | next view | next widget |
| Select | open the detail card for the selected aircraft | activate the focused widget |
| Backspace / Esc | close the detail card, then exit to the launcher | close the settings panel |
| F1 | exit to the launcher | exit to the launcher |
| F2 | toggle the settings panel | close the settings panel |
| F3 | cycle range | cycle range |
| F4 | cycle filter | cycle filter |
| F5 | toggle auto-cycle | toggle auto-cycle |

Keys repeat when held, so scrolling the arrivals board is continuous.

## Fonts

Text is rendered with the Hershey vector font shared with
[SynthEngine3D](https://github.com/cavac/tanmatsu-synthminer-grace) instead of LVGL's built-in
bitmap fonts. Each glyph is rasterized once into an A8 mask and cached per font size, so the
letters stay sharp at any size and repeated draws cost about the same as a bitmap font. The
implementation lives in `main/hershey/`.

## Building

```sh
make build              # build for tanmatsu (esp32p4)
make install            # flash to a connected device
make size               # memory report
```

`make build` picks up ESP-IDF v6.0.2 from `IDF_PATH`/`IDF_TOOLS_PATH`, from `make prepare`, or
from `eim`. See the [Tanmatsu app template](https://docs.tanmatsu.cloud) for the toolchain setup.

## Porting notes

Differences from the original `adsb` project:

- ESP-IDF instead of PlatformIO, and the Tanmatsu app template build system instead of a bare
  `idf.py` project
- Display is 800x480 logical (480x800 panel rotated 270 degrees) instead of 1024x600
- Input is the badge-bsp navigator/keyboard queue instead of a GT11 capacitive touch panel
- Wi-Fi comes from the Tanmatsu coprocessor through `wifi-manager` / `esp-wifi-remote` instead of
  SDIO esp-hosted and Ethernet
- Hershey vector font instead of LVGL's Montserrat bitmap fonts
- No Ethernet, no touchscreen, no audio

## License

MIT, same as the original project. See `LICENSE`.
