# Credits & third-party licenses

This project is a fork. It stands on the work of others, and this file records
exactly whose and under what terms. Every bundled component keeps its original
`LICENSE` file in its own directory — this is a map, not a replacement.

## Upstream projects (the foundation)

Both by **Just Call Me Koko** ([justcallmekoko](https://github.com/justcallmekoko)),
both MIT-licensed:

| Project | Path | Based on | License | Copyright |
|---|---|---|---|---|
| [ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder) | `ESP32Marauder/` | v1.13.0 | MIT | (c) 2020 Just Call Me Koko |
| [ESP32DualBandWardriver](https://github.com/justcallmekoko/ESP32DualBandWardriver) | `ESP32DualBandWardriver/` | v2.2.0 | MIT | (c) 2025 Just Call Me Koko |

The bundled sources are pinned to those upstream versions — the exact code this
fork was built and tested against. Newer upstream releases exist; tracking them
is a separate, tested step.

The **Wardrive Core** aggregator mode is a substantial derivative of the
ESP32DualBandWardriver node logic and reuses its record/wire format on purpose.
Our changes live on top of the pinned upstream sources. This repository is **not**
affiliated with or endorsed by Just Call Me Koko — see the README for the full
disambiguation.

## Vendored libraries (`libs/`)

Pinned copies of build dependencies, each under its own license. No GPL and no
AGPL code is present anywhere in this tree; the strongest copyleft here is LGPL,
used only as linked libraries with their source and license included, which
imposes no license obligation on the fork's own MIT code.

| Library | License |
|---|---|
| ArduinoJson | MIT |
| Adafruit_BusIO | MIT |
| Adafruit_MAX1704X | BSD-3-Clause |
| Adafruit_NeoPixel | LGPL-3.0 |
| Adafruit_TCA8418 | MIT |
| lv_arduino (LVGL) | MIT |
| LinkedList | MIT |
| TFT_eSPI | MIT (Bodmer, ILI9341-derived) |
| XPT2046_Touchscreen | MIT |
| JPEGDecoder | permissive (picojpeg-derived) |
| NimBLE-Arduino | Apache-2.0 |
| ESPAsyncWebServer | LGPL-3.0 |
| AsyncTCP | LGPL-3.0 |
| MicroNMEA | LGPL-2.1 |
| ESP32Ping | LGPL-2.1 |
| EspSoftwareSerial | LGPL-2.1 |

If you spot an attribution error, please open an issue — getting credit right
matters.
