# ESP32 ASCII Frame

Renders ASCII art, static or animated, centered on the screen of a Waveshare ESP32-S3-Touch-LCD-1.69 (240 x 280). Tap to go to the next piece. Shake the device to play a gesture animation.

## Layout

| Path | What it is |
|---|---|
| `ascii/*.json` | The main art. One file per piece, shown in file-name order; tap advances. |
| `gestures/shake/*.json` | Animations played when the device is shaken (hearts). With several files, each shake plays the next one. |
| `src/config.h` | Every setting: margin, brightness, contrast, gamma, invert, tint, speed, character ramp, shake sensitivity. |
| `src/main.cpp` | Renderer, tap handling, shake detection. |
| `src/LGFX_Config.h` | Display and touch pin map. |
| `tools/embed_art.py` | Build step that turns the JSON folders into `src/art_data.h` (generated, git-ignored). |

## Adding art

Drop a JSON file in the right folder and rebuild (`pio run -t upload`). Format (`ascii-animation/v1`):

```json
{
  "format": "ascii-animation/v1",
  "ascii_size": {"cols": 100, "rows": 58},
  "frames": [
    {"duration_ms": 200, "ascii": "line one\nline two"}
  ]
}
```

- One frame is a static image; more frames loop as an animation, each held for its `duration_ms`.
- `ascii_size` is the character grid the renderer lays out from. Text outside it is cropped and missing text is black. If it is missing, the size is measured from the frames.
- `@` is the brightest character and space is black. Change the mapping in `RAMP` in `config.h`.
- Up to 32 frames per piece.

## Shake

The QMI8658 motion sensor shares the touch controller's I2C bus. A shake is `SHAKE_JOLTS` jolts above `SHAKE_G` within `SHAKE_WINDOW` ms; tune these in `config.h`. The animation plays `SHAKE_LOOPS` times, then the previous piece returns. If the sensor is not found, serial prints `shake: IMU not found` and the gesture is disabled.

## Build and flash

PlatformIO, `espressif32` platform, Arduino framework, LovyanGFX.

```
pio run -t upload
```

If the board boot-loops with a partition table error, run `pio run -t erase` and upload again. The `esp32-s3-devkitc-1` board definition assumes 8 MB flash and no PSRAM, so [platformio.ini](platformio.ini) overrides the flash size, partitions and memory type for this 16 MB / 8 MB PSRAM board.
