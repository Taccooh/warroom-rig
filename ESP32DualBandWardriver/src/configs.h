#ifndef configs_h
#define configs_h

// ===========================================================================
// Board selection. Define exactly one externally (-DNODEMCU32_NODE,
// -DC5_ZERO_NODE) or fall back to the default JCMK C5 host board if nothing
// is set.
// ===========================================================================
#if !defined(JCMK_HOST_BOARD) && !defined(NODEMCU32_NODE) && !defined(C5_ZERO_NODE) \
    && !defined(XIAO_C5_NODE)
  #define JCMK_HOST_BOARD
#endif

// ---------------------------------------------------------------------------
// Default board: JCMK C5 Host (original Koko design, ESP32-C5-DevKitC-1)
// ---------------------------------------------------------------------------
#ifdef JCMK_HOST_BOARD

// Pins used:
/*
1 BTN
2 SPI
4 BAT I2C
5 BAT I2C
6 SPI
7 SPI
8 BTN
9 BTN
10 SD CS
13 GPS UART
14 GPS UART
23 TFT
24 TFT
27 TFT
28 ACT LED
*/

//// Firmware info stuff
#define FIRMWARE_VERSION "v2.2.0"
#define DEVICE_NAME      "JCMK C5 Wardriver"

//// Role stuff
#define SOLO
// #define CORE
// #define NODE

//// LED stuff
#define LED_PIN 28

//// Display stuff
#define TFT_CS   23
#define TFT_DC   24
#define TFT_RST  -1
#define TOUCH_CS -1
#define TFT_MOSI 7
#define TFT_SCLK 6
#define TFT_BL   27

//// Buttons
#define U_BTN 9
#define D_BTN 8
#define C_BTN 1

#define C_PULL false
#define U_PULL false
#define D_PULL false

//// Battery
#define HAS_BATTERY
#define I2C_SCL 4
#define I2C_SDA 5

//// GPS
#define GPS_SERIAL_INDEX 1
#define TX_TO_GPS 13
#define RX_TO_GPS 14

//// SD / shared SPI
#define SPI_SCK  6
#define SPI_MISO 2
#define SPI_MOSI 7
#define SD_CS    10

//// Device capabilities
#define HAS_PSRAM
#define HAS_GPS
#define HAS_SD

#endif // JCMK_HOST_BOARD

// ---------------------------------------------------------------------------
// Waveshare ESP32-C5-Zero — headless dual-band NODE.
// Activated with -DC5_ZERO_NODE. Same C5 SoC as the JCMK host board, so full
// 2.4 + 5 GHz scanning, but a bare Zero-form module: no display, SD, GPS or
// battery wired up — the Marauder HUB provides those. Only scans WiFi and
// ships results via ESP-NOW.
//
// CRITICAL vs the JCMK board: on the ESP32-C5 the USB-Serial-JTAG is on
// GPIO13/GPIO14, and on the Zero the USB-C port routes straight to them. The
// JCMK config puts the GPS UART on 13/14 — that would kill the Zero's USB
// console. GPS is therefore moved to UART pins 16/17 (nothing attached, finds
// no fix → harmless). Every dummy peripheral pin is kept off 13/14.
//
// 4 MB flash, NO PSRAM (build: FlashSize=4M,PartitionScheme=min_spiffs,
// PSRAM=disabled). HAS_PSRAM is intentionally left undefined; its utils.cpp
// helper is guarded, same as the NodeMCU-32 port.
// ---------------------------------------------------------------------------
#ifdef C5_ZERO_NODE

#define FIRMWARE_VERSION "v2.2.0-c5zero"
#define DEVICE_NAME      "Waveshare C5 Zero Node"

//// Role: NODE only. Headless ESP-NOW probe for a Marauder HUB.
#define NODE

//// LED — onboard WS2812 RGB is not driven by plain digitalWrite; park the
//// LED pin on a harmless pad. Status is via Serial / the HUB node table.
#define LED_PIN 28

//// Display — not connected. Pins kept off USB (13/14).
#define TFT_CS   23
#define TFT_DC   24
#define TFT_RST  -1
#define TOUCH_CS -1
#define TFT_MOSI 7
#define TFT_SCLK 6
#define TFT_BL   -1   // freed from GPIO27 → WS2812 status LED owns 27

//// Buttons — not connected; default not-pressed.
#define U_BTN 9
#define D_BTN 8
#define C_BTN 1

#define C_PULL false
#define U_PULL false
#define D_PULL false

//// Battery — not wired; default I2C pins for compile sanity.
#define HAS_BATTERY
#define I2C_SCL 4
#define I2C_SDA 5

//// GPS — not wired. Moved OFF 13/14 (USB-Serial-JTAG on C5) to UART 16/17.
#define GPS_SERIAL_INDEX 1
#define TX_TO_GPS 17
#define RX_TO_GPS 16

//// SD / shared SPI — not wired; kept off USB pins.
#define SPI_SCK  6
#define SPI_MISO 2
#define SPI_MOSI 7
#define SD_CS    10

//// Device capabilities — NO PSRAM on the Zero.
#define HAS_GPS
#define HAS_SD

//// Antenna — the Waveshare C5-Zero carries a GPIO-controlled RF switch
//// (internal ceramic vs external IPEX) on GPIO26. Firmware must ACTIVELY
//// select the external path: the power-on default is the internal antenna,
//// which leaves a connected external antenna out of the RF chain → near-dead
//// RX (finds ~1 AP where the HUB finds a dozen). GPIO26 is otherwise unused
//// in this build.
#define ANT_SWITCH_PIN      26
#define ANT_EXTERNAL_LEVEL  HIGH   // TODO verify polarity (schematic truth table / empirical)

#endif // C5_ZERO_NODE

// ---------------------------------------------------------------------------
// Seeed XIAO ESP32-C5 — headless NODE-only port.
// Activated with -DXIAO_C5_NODE. Same C5 SoC as the Waveshare C5-Zero and the
// same job: scan WiFi, ship results over ESP-NOW, let the HUB own display, SD
// and GPS. It is a separate target rather than a rename because two board
// differences are load-bearing:
//
//   * PSRAM. The XIAO carries 8 MB of it, the Zero has none. Espressif
//     documents GPIO16 ~ GPIO22 as "usually used for SPI flash and PSRAM, not
//     recommended for other uses". The Zero build parks its (unwired) GPS UART
//     on 16/17 — free on that module, sitting on the PSRAM lines here. Opening
//     that UART reset the chip on every boot: a flash that verified fine, then
//     a device re-enumerating on USB forever. The UART moves to free pads here.
//     (Compiling GPS out instead does not work: GpsInterface.cpp is behind
//     HAS_GPS, but WiFiOps.cpp calls into it unguarded and the link fails. The
//     pins were the bug, not the feature.)
//   * Strapping. GPIO2, 7, 25, 27 and 28 are strapping pins on the C5. The Zero
//     build parks the LED on 28 and the unwired SD/SPI on 2 and 7, and gets
//     away with it. Every dummy pin below avoids the straps instead — a pin the
//     firmware holds is a pin that decides the boot mode at the next reset.
//
// No antenna switch: the XIAO has no RF switch to select, so ANT_SWITCH_PIN is
// deliberately absent. Driving a pin for a part that is not on the board buys
// nothing and risks whatever IS on it.
//
// Pins below avoid 13/14 (USB-Serial-JTAG), 16-22 (flash/PSRAM) and the straps.
// Build: FlashSize=4M, PartitionScheme=min_spiffs, CDCOnBoot=cdc. 4 MB is the
// proven Zero layout and runs fine on the bigger part; it is deliberately not
// two changes at once.
// ---------------------------------------------------------------------------
#ifdef XIAO_C5_NODE

// THE FLASH FLAGS ARE NOT OPTIONAL HERE. Build this target with
//   FlashMode=dio,FlashFreq=40
// on top of the usual CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=4M.
//
// The core defaults the C5 to QIO at 80 MHz. The Waveshare Zero tolerates that;
// this module does not. The failure is worth spelling out because it looks like
// anything except a flash-timing problem: the image writes and verifies
// byte-for-byte, esptool reads the partition table back perfectly intact, and
// the second-stage bootloader still says "No bootable app partitions in the
// partition table" and reboots forever. That asymmetry is the tell. esptool
// reads through the ROM's conservative settings; the bootloader reconfigures
// the flash controller from its own image header first, and every read it makes
// after that comes back as garbage. Before settling into the loop it may also
// boot once or twice and then die with CPU_LOCKUP, taking the app partition
// with it -- which reads like a bug in whatever code ran last, and is not.
#define FIRMWARE_VERSION "v2.2.0-xiaoc5"
#define DEVICE_NAME      "XIAO C5 Node"

//// Role: NODE only. Headless ESP-NOW probe for a Marauder HUB.
#define NODE

//// LED — onboard RGB is a WS2812 and not driven by digitalWrite; park it on a
//// free pad, off the straps. Status is via Serial / the HUB node table.
#define LED_PIN 24

//// Display — not connected. Pins kept off USB, flash/PSRAM and straps.
#define TFT_CS   23
#define TFT_DC   12
#define TFT_RST  -1
#define TOUCH_CS -1
#define TFT_MOSI 11
#define TFT_SCLK 6
#define TFT_BL   -1

//// Buttons — not connected; must read "not pressed" at boot, or the wardriver
//// opens its admin window on a headless node.
#define U_BTN 9
#define D_BTN 8
#define C_BTN 1

#define C_PULL false
#define U_PULL false
#define D_PULL false

//// Battery — not wired; default I2C pins for compile sanity.
#define HAS_BATTERY
#define I2C_SCL 4
#define I2C_SDA 5

//// GPS — not wired; the HUB supplies the fix. The UART still opens, so its
//// pins must be real and harmless: OFF 16-22 (flash/PSRAM, the bug this target
//// exists for), off 13/14 (USB) and off the straps. GPIO26 is neither a strap
//// nor a flash pin, and the XIAO has no antenna switch sitting on it.
#define GPS_SERIAL_INDEX 1
#define TX_TO_GPS 15
#define RX_TO_GPS 26

//// SD / shared SPI — not wired; defines exist so the bus can talk into the
//// void without touching a strap.
#define SPI_SCK  6
#define SPI_MISO 3
#define SPI_MOSI 11
#define SD_CS    10

//// Device capabilities — same set as the Zero. HAS_PSRAM stays undefined even
//// though the module has 8 MB: nothing here needs it, and enabling it only
//// adds an early-boot init that hangs when the line mode does not match.
#define HAS_GPS
#define HAS_SD

#endif // XIAO_C5_NODE

// ---------------------------------------------------------------------------
// NodeMCU-32 (ESP32-WROOM-32) — headless NODE-only port.
// Activated with -DNODEMCU32_NODE. No display, no SD, no GPS, no battery —
// the Marauder HUB provides all of that. The board only needs to scan WiFi
// (2.4 GHz) and ship results via ESP-NOW.
//
// Why these pin choices: GPIOs 6-11 on ESP32-WROOM-32 are wired to the
// internal flash and must not be touched. GPIOs 1/3 are the USB serial.
// GPIOs 34-39 are input-only with no internal pull resistors.
// Buttons are placed on pulldown-capable GPIOs so they read "not pressed"
// at boot (the wardriver opens an admin window if c_btn is held during
// startup — we never want that on a headless node).
// ---------------------------------------------------------------------------
#ifdef NODEMCU32_NODE

#define FIRMWARE_VERSION "v2.2.0-nodemcu32"
#define DEVICE_NAME      "NodeMCU-32 Wardriver Node"

//// Role: NODE only. This is a headless ESP-NOW probe for a Marauder HUB.
#define NODE

//// LED — onboard LED on NodeMCU-32 is GPIO 2
#define LED_PIN 2

//// Display — not connected. Pins picked so display.begin() can talk SPI
//// into the void without colliding with reserved pins.
#define TFT_CS   15
#define TFT_DC   4
#define TFT_RST  -1
#define TOUCH_CS -1
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_BL   26

//// Buttons — pulldown-capable GPIOs; default LOW = not pressed
#define U_BTN 32
#define D_BTN 33
#define C_BTN 25

#define C_PULL false
#define U_PULL false
#define D_PULL false

//// I2C — not connected, default ESP32 pins for compile sanity
#define I2C_SCL 22
#define I2C_SDA 21

//// GPS — not connected, UART2 default pins for compile sanity
#define GPS_SERIAL_INDEX 2
#define TX_TO_GPS 17
#define RX_TO_GPS 16

//// Shared SPI — VSPI defaults (TFT/SD share this bus; both disabled)
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23
#define SD_CS    5

//// Device capabilities
// HAS_PSRAM intentionally undefined (WROOM-32 has no PSRAM, and its helper
// in utils.cpp is properly guarded in both header and .cpp).
//
// HAS_BATTERY/HAS_GPS/HAS_SD ARE defined even though no peripheral is
// wired up. Reason: these classes' headers declare methods that the rest
// of the code calls unconditionally, but in {GpsInterface,SDInterface,
// BatteryInterface}.cpp the method *bodies* are wrapped in #ifdef HAS_xxx.
// Undefining them removes the bodies and the linker fails. With them
// defined, the I2C/UART/SD probes simply find no hardware at runtime,
// the *_supported / gps_enabled flags stay false, and downstream code
// already gates on those flags (`gps.getFixStatus()`, `sd_obj.supported`).
#define HAS_BATTERY
#define HAS_GPS
#define HAS_SD

// Classic ESP32 has no 5 GHz radio. Calling esp_wifi_set_channel(36..177)
// on this hardware spams `wifi:unsupported channel` ERRORs ~26x per scan
// cycle and confuses the WiFi stack enough that ESP-NOW heartbeat sends
// occasionally fail. Force the node-side scan loop and the default
// assigned slice to stay in the 2.4-GHz portion of scan_channels[].
#define WARDRIVE_2_4_ONLY

#endif // NODEMCU32_NODE

// ===========================================================================
// Common across all boards
// ===========================================================================

//// Role sanity check
#if !defined(SOLO) && !defined(CORE) && !defined(NODE)
  #error "Define exactly one role: SOLO, CORE, or NODE"
#endif
#if defined(SOLO) && defined(CORE) && defined(NODE)
  #error "Define exactly one role: SOLO, CORE, or NODE"
#elif defined(SOLO) && defined(CORE)
  #error "Define exactly one role: SOLO, CORE, or NODE"
#elif defined(CORE) && defined(NODE)
  #error "Define exactly one role: SOLO, CORE, or NODE"
#elif defined(SOLO) && defined(NODE)
  #error "Define exactly one role: SOLO, CORE, or NODE"
#endif

#define SOLO_MODE 1
#define NODE_MODE 2
#define CORE_MODE 3

#define ENOW_KEY_MAX_LEN 32
#define ENOW_TEXT_MAX    200

//// BLE stuff
#define BLE_SCAN_DURATION   1 * 500 // 0.5 second

//// Display dimensions / SPI speed (shared)
#define ON  HIGH
#define OFF LOW

#define TFT_HEIGHT 80
#define TFT_WIDTH  160

#define TFT_SPI_SPEED 27000000

//// UI Stuff
#define UI_UPDATE_TIME 1 * 1000 // 1 second

#define WEB_PAGE_TIMEOUT 60 * 1000 // 60 seconds
#define TIMER_UPDATE 1 * 1000 // 1 second
#define STATION_CONNECT_TIMEOUT 5 * 1000 // 5 seconds
#define WIFI_CONFIG "/settings.json"
#define LOG_FILE_NAME "wardrive"
#define SETTING_SANITY "wu"

#define SMALL_CHAR_HEIGHT 8

//// Buffer stuff
#define BUF_SIZE 2 * 1024
#define SNAP_LEN 2324

#define UPDATE_KEY "UpdateFile"

////WiFi stuff
#define mac_history_len 200
#define CHANNEL_TIMER 80

#endif
