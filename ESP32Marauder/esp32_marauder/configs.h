#pragma once

#ifndef configs_h

  #define configs_h

  #define POLISH_POTATO

  //#define DEVELOPER

  //// BOARD TARGETS
  //#define MARAUDER_V7
  //#define MARAUDER_V7_1

  // CORE MODE FEATURE TOGGLE — orthogonal zu Hardware-Target.
  // Aktiviert die Marauder-CORE-Funktionalitaet (ESP-NOW Wardrive-Aggregator).
  // Sinnvoll mit MARAUDER_V7 oder MARAUDER_V7_1.
  //#define MARAUDER_CORE_MODE

  // 2.4-GHz-only NODES (z.B. NodeMCU-32/WROOM-32 ohne 5 GHz Radio). Beschraenkt
  // den Channel-Assignment-Pool im Core auf die 2.4-GHz-Eintraege (idx 0..13)
  // von scan_channels[]. Setzen wenn ALLE angeschlossenen Nodes 2.4-GHz-only
  // sind. Bei Mixed-Fleet (manche C5, manche Classic) auslassen — dann faellt
  // der 5-GHz-Slice auf Classic-Nodes still durch (esp_wifi_set_channel(36)
  // returnt ESP_ERR_INVALID_ARG, Node scannt nichts in dem Bereich).
  //#define WARDRIVE_2_4_ONLY

  // WDGWARS UPLOAD FEATURE TOGGLE — orthogonal zu Hardware-Target und Core-Mode.
  // Aktiviert den Upload von Wigle-CSV-Logs zu wdgwars.pl/api/v2/upload-csv via STA.
  // Sinnvoll wenn der Marauder GPS+SD hat und ein Heim-AP erreichbar ist.
  //#define MARAUDER_WDGWARS_UPLOAD

  // FILE SERVER AP FEATURE TOGGLE — orthogonal zu Hardware-Target / Core-Mode.
  // Spannt einen WPA2-SoftAP + HTTP-Server auf der SD-Karte auf. Praktisch
  // im Feld um Wardrive-Logs (oder beliebige andere Dateien) per Browser
  // zu ziehen ohne SD-Karte rauspopeln zu muessen.
  // Default-SSID: warroom-rig-Files-XXXX. Das WPA2-Passwort ist KEINE Konstante
  // mehr: es wird beim ersten Start pro Geraet zufaellig erzeugt und in NVS
  // abgelegt, zusammen mit einem zweiten Geheimnis fuer die HTTP-Anmeldung.
  // Beides steht im Betrieb auf dem Display. Grund: der PSK stand vorher als
  // Literal in einem oeffentlichen Repo, und dahinter lag keine Authentifizierung
  // — wer den String kannte, konnte die komplette SD-Karte lesen, inklusive
  // /wdgwars.txt mit Heim-WLAN-Passwort und API-Key. Aus der MAC ableiten waere
  // keine Loesung: die BSSID steht in jedem Beacon.
  // Ueberschreibbar via optionalem /fileserver.txt auf SD (Keys: ssid, pass,
  // user, httppass, auth).
  //#define MARAUDER_FILE_SERVER_AP

  //#define MARAUDER_CARDPUTER_ADV
  //#define MARAUDER_V8
  //#define DUAL_MINI_C5
  //// END BOARD TARGETS

  #define JSON_SETTING_SIZE 2048

  #define MARAUDER_VERSION "v1.13.0"
  // warroom-rig product version (shown on the boot splash). The Marauder
  // version above is kept for internal/upstream references.
  #define WARROOM_RIG_VERSION "v0.2"

  #define GRAPH_REFRESH   100

  #define TRACK_EVICT_SEC 90 // Seconds before marking tracked MAC as tombstone

  #define DUAL_BAND_CHANNELS 51

  #define DISPLAY_BUFFER_LIMIT 20

  #define MODE_OFF 0
  #define MODE_RAINBOW 1
  #define MODE_ATTACK 2
  #define MODE_SNIFF 3
  #define MODE_CUSTOM 4

  //// HARDWARE NAMES
#if defined(MARAUDER_CARDPUTER_ADV)
    #define HARDWARE_NAME "M5 Cardputer ADV"
#elif defined(MARAUDER_V7)
    #define HARDWARE_NAME "Marauder v7"
#elif defined(MARAUDER_V7_1)
    #define HARDWARE_NAME "Marauder v7.1"
#elif defined(MARAUDER_V8)
    #define HARDWARE_NAME "Marauder v8"
#elif defined(DUAL_MINI_C5)
    #define HARDWARE_NAME "Dual Mini C5"
#else
    #define HARDWARE_NAME "ESP32"
#endif

  //// END HARDWARE NAMES

 //// BOARD FEATURES
#if defined(DUAL_MINI_C5)
    #define MARAUDER_MINI_V3
#endif


#if defined(MARAUDER_CARDPUTER_ADV)
    //#define FLIPPER_ZERO_HAT
    #define HAS_MINI_KB
    #define HAS_BT
    // HAS_BUTTONS is what the inherited code tests before compiling any input
    // path, and the ADV does have one -- a keyboard. The pin numbers below are
    // all -1 because there are no discrete buttons; RigInput is what turns that
    // into working navigation.
    #define HAS_BUTTONS
    //#define HAS_PWR_MGMT
    #define HAS_SCREEN
    #define HAS_MINI_SCREEN
    #define HAS_SD
    #define USE_SD
    #define HAS_TEMP_SENSOR
    #define HAS_GPS
    #define HAS_BATTERY
    #define BATTERY_ADC_PIN 10
    #define HAS_NEOPIXEL_LED

    // Toolchain selectors, not hardware. This tree pins one arduino-cli, one
    // ESP32 core (3.3.4, IDF 5.x) and one vendored NimBLE (2.x) for every
    // target, so the modern API paths are the only ones that can compile.
    // Upstream builds the Cardputer against an older core and therefore leaves
    // these off; without them the ADV falls into the legacy branches and dies
    // on esp_event_send_internal, tcpip_adapter_* and the NimBLE 1.x calls.
    #define HAS_NIMBLE_2
    #define HAS_IDF_3
#endif


#ifdef MARAUDER_V7
    //#define FLIPPER_ZERO_HAT
    #define HAS_MINI_KB
    #define HAS_BATTERY
    #define HAS_BT
    #define HAS_BT_REMOTE
    #define HAS_BUTTONS
    //#define HAS_NEOPIXEL_LED
    //#define HAS_PWR_MGMT
    #define HAS_SCREEN
    #define HAS_FULL_SCREEN
    #define HAS_SD
    #define USE_SD
    #define HAS_TEMP_SENSOR
    #define HAS_GPS
    #define HAS_NIMBLE_2
    #define HAS_IDF_3
    #define HAS_C5_SD
#endif

#ifdef MARAUDER_V7_1
    //#define FLIPPER_ZERO_HAT
    #define HAS_MINI_KB
    #define HAS_BATTERY
    #define HAS_BT
    #define HAS_BT_REMOTE
    #define HAS_BUTTONS
    #define HAS_NEOPIXEL_LED
    //#define HAS_PWR_MGMT
    #define HAS_SCREEN
    #define HAS_FULL_SCREEN
    #define HAS_SD
    #define USE_SD
    #define HAS_TEMP_SENSOR
    #define HAS_GPS
    #define HAS_PSRAM

    // Toolchain selectors, not hardware -- the same two macros V7 carries a few
    // lines up, and V7.1 was the one target missing them. This tree pins one
    // arduino-cli, one ESP32 core (3.3.4, IDF 5.5) and one vendored NimBLE (2.x)
    // for every build, so only the modern API paths can compile. Without these,
    // V7.1 falls into the legacy branches that do not exist here: WiFiScan.h dies
    // on esp_event_send_internal / g_wifi_feature_caps, and esp32_marauder.ino
    // reaches for esp_spiram_init(), which IDF 5.5 renamed to esp_psram_init().
    // That is why V7.1 never actually built while V7 did. Set to match V7.
    // Verified: the target now compiles green. It has still never been run on a
    // V7.1 board, so the pin map below is inherited from V7 and unconfirmed.
    #define HAS_NIMBLE_2
    #define HAS_IDF_3
#endif
















#ifdef MARAUDER_V8
    #define HAS_TOUCH
    //#define HAS_FLIPPER_LED
    //#define FLIPPER_ZERO_HAT
    #define HAS_BATTERY
    #define HAS_BT
    //#define HAS_BUTTONS
    //#define HAS_NEOPIXEL_LED
    //#define HAS_PWR_MGMT
    #define HAS_SCREEN
    #define HAS_FULL_SCREEN
    #define HAS_GPS
    #define HAS_C5_SD
    #define HAS_SD
    #define USE_SD
    #define HAS_DUAL_BAND
    #define HAS_PSRAM
    //#define HAS_TEMP_SENSOR
    #define HAS_NIMBLE_2
    #define HAS_IDF_3
    #define HAS_ACT_LED
#endif



  //// END BOARD FEATURES

  //// POWER MANAGEMENT
#ifdef HAS_PWR_MGMT
#if defined(HAS_AXP192)
      #include "AXP192.h"
#endif

#endif
  //// END POWER MANAGEMENT

  //// BUTTON DEFINITIONS
#ifdef HAS_BUTTONS



#ifdef MARAUDER_V7
      #define L_BTN 13
      #define C_BTN 34
      #define U_BTN 36
      #define R_BTN 39
      #define D_BTN 35

      #define HAS_L
      #define HAS_R
      #define HAS_U
      #define HAS_D
      #define HAS_C

      #define L_PULL true
      #define C_PULL true
      #define U_PULL true
      #define R_PULL true
      #define D_PULL true
#endif

#ifdef MARAUDER_V7_1
      #define L_BTN 13
      #define C_BTN 34
      #define U_BTN 36
      #define R_BTN 39
      #define D_BTN 35

      #define HAS_L
      #define HAS_R
      #define HAS_U
      #define HAS_D
      #define HAS_C

      #define L_PULL true
      #define C_PULL true
      #define U_PULL true
      #define R_PULL true
      #define D_PULL true
#endif


#if defined(MARAUDER_CARDPUTER_ADV)
      #define L_BTN -1
      #define C_BTN 0
      #define U_BTN -1
      #define R_BTN -1
      #define D_BTN -1

      //#define HAS_L
      //#define HAS_R
      //#define HAS_U
      //#define HAS_D
      #define HAS_C

      #define L_PULL true
      #define C_PULL true
      #define U_PULL true
      #define R_PULL true
      #define D_PULL true
#endif








#endif
  //// END BUTTON DEFINITIONS

  //// DISPLAY DEFINITIONS
#ifdef HAS_SCREEN



#if defined(MARAUDER_CARDPUTER_ADV)
      #define CHAN_PER_PAGE 14

      #define SCREEN_CHAR_WIDTH 40
      //#define TFT_MISO -1
      #define TFT_MOSI 35
      #define TFT_SCLK 36
      #define TFT_CS 37
      #define TFT_DC 34
      #define TFT_RST 33
      #define TFT_BL 38
      // #define TOUCH_CS -1

      #define SCREEN_BUFFER

      #define MAX_SCREEN_BUFFER 9

      #define BANNER_TEXT_SIZE 1

#ifndef TFT_WIDTH
        #define TFT_WIDTH 135
#endif

#ifndef TFT_HEIGHT
        #define TFT_HEIGHT 240
#endif

      #define EXT_BUTTON_WIDTH 0

      #define SCREEN_ORIENTATION 1

      #define CHAR_WIDTH 6
      #define SCREEN_WIDTH TFT_HEIGHT // 240 in landscape
      #define SCREEN_HEIGHT TFT_WIDTH // 135 in landscape
      #define GRAPH_VERT_LIM SCREEN_HEIGHT/2 - 1
      #define HEIGHT_1 TFT_WIDTH
      #define WIDTH_1 TFT_WIDTH
      #define STANDARD_FONT_CHAR_LIMIT (TFT_WIDTH/6) // number of characters on a single line with normal font
      #define TEXT_HEIGHT (TFT_HEIGHT/10) // Height of text to be printed and scrolled
      #define BOT_FIXED_AREA 0 // Number of lines in bottom fixed area (lines counted from bottom of screen)
      #define TOP_FIXED_AREA 48 // Number of lines in top fixed area (lines counted from top of screen)
      #define YMAX TFT_HEIGHT // Bottom of screen area
      #define minimum(a,b)     (((a) < (b)) ? (a) : (b))
      //#define MENU_FONT NULL
      #define MENU_FONT &FreeMono9pt7b // Winner
      //#define MENU_FONT &FreeMonoBold9pt7b
      //#define MENU_FONT &FreeSans9pt7b
      //#define MENU_FONT &FreeSansBold9pt7b
      #define BUTTON_SCREEN_LIMIT 6
      #define BUTTON_ARRAY_LEN 100
      #define STATUS_BAR_WIDTH (SCREEN_HEIGHT/16)
      #define LVGL_TICK_PERIOD 6
    
      #define FRAME_X 100
      #define FRAME_Y 64
      #define FRAME_W 120
      #define FRAME_H 50
    
      // Red zone size
      #define REDBUTTON_X FRAME_X
      #define REDBUTTON_Y FRAME_Y
      #define REDBUTTON_W (FRAME_W/2)
      #define REDBUTTON_H FRAME_H
    
      // Green zone size
      #define GREENBUTTON_X (REDBUTTON_X + REDBUTTON_W)
      #define GREENBUTTON_Y FRAME_Y
      #define GREENBUTTON_W (FRAME_W/2)
      #define GREENBUTTON_H FRAME_H
    
      #define STATUSBAR_COLOR 0x5A44

#endif



#if defined(MARAUDER_V8)
      #define CHAN_PER_PAGE 7

      #define SCREEN_CHAR_WIDTH 40
      #define HAS_ILI9341
    
      #define BANNER_TEXT_SIZE 2

#ifndef TFT_WIDTH
        #define TFT_WIDTH 240
#endif

#ifndef TFT_HEIGHT
        #define TFT_HEIGHT 320
#endif

      //#ifndef MARAUDER_CYD_MICRO
        //#define TFT_DIY
        //#define TFT_SHIELD
      //#endif

      #define GRAPH_VERT_LIM TFT_HEIGHT/2 - 1

      #define EXT_BUTTON_WIDTH 30

      #define SCREEN_BUFFER

      #define MAX_SCREEN_BUFFER 21

      #define SCREEN_ORIENTATION 0
    
      #define CHAR_WIDTH 12
      #define SCREEN_WIDTH TFT_WIDTH
      #define SCREEN_HEIGHT TFT_HEIGHT
      #define HEIGHT_1 TFT_WIDTH
      #define WIDTH_1 TFT_HEIGHT
      #define STANDARD_FONT_CHAR_LIMIT (TFT_WIDTH/6) // number of characters on a single line with normal font
      #define TEXT_HEIGHT 16 // Height of text to be printed and scrolled
      #define BOT_FIXED_AREA 0 // Number of lines in bottom fixed area (lines counted from bottom of screen)
      #define TOP_FIXED_AREA 48 // Number of lines in top fixed area (lines counted from top of screen)
      #define YMAX 320 // Bottom of screen area
      #define minimum(a,b)     (((a) < (b)) ? (a) : (b))
      //#define MENU_FONT NULL
      #define MENU_FONT &FreeMono9pt7b // Winner
      //#define MENU_FONT &FreeMonoBold9pt7b
      //#define MENU_FONT &FreeSans9pt7b
      //#define MENU_FONT &FreeSansBold9pt7b
      #define BUTTON_SCREEN_LIMIT 12
      #define BUTTON_ARRAY_LEN BUTTON_SCREEN_LIMIT
      #define STATUS_BAR_WIDTH 16
      #define LVGL_TICK_PERIOD 6

      #define FRAME_X 100
      #define FRAME_Y 64
      #define FRAME_W 120
      #define FRAME_H 50
    
      // Red zone size
      #define REDBUTTON_X FRAME_X
      #define REDBUTTON_Y FRAME_Y
      #define REDBUTTON_W (FRAME_W/2)
      #define REDBUTTON_H FRAME_H
    
      // Green zone size
      #define GREENBUTTON_X (REDBUTTON_X + REDBUTTON_W)
      #define GREENBUTTON_Y FRAME_Y
      #define GREENBUTTON_W (FRAME_W/2)
      #define GREENBUTTON_H FRAME_H
    
      #define STATUSBAR_COLOR 0x5A44
    
      #define KIT_LED_BUILTIN 13
#endif






#ifdef MARAUDER_V7
      #define CHAN_PER_PAGE 7

      #define SCREEN_CHAR_WIDTH 40
      //#define HAS_ILI9341
    
      #define BANNER_TEXT_SIZE 2

#ifndef TFT_WIDTH
        #define TFT_WIDTH 240
#endif

#ifndef TFT_HEIGHT
        #define TFT_HEIGHT 320
#endif

      #define GRAPH_VERT_LIM TFT_HEIGHT/2 - 1

      #define TFT_DIY

      #define SCREEN_BUFFER

      #define MAX_SCREEN_BUFFER 22

      #define EXT_BUTTON_WIDTH 0

      #define SCREEN_ORIENTATION 0
    
      #define CHAR_WIDTH 12
      #define SCREEN_WIDTH TFT_WIDTH
      #define SCREEN_HEIGHT TFT_HEIGHT
      #define HEIGHT_1 TFT_WIDTH
      #define WIDTH_1 TFT_HEIGHT
      #define STANDARD_FONT_CHAR_LIMIT (TFT_WIDTH/6) // number of characters on a single line with normal font
      #define TEXT_HEIGHT 16 // Height of text to be printed and scrolled
      #define BOT_FIXED_AREA 0 // Number of lines in bottom fixed area (lines counted from bottom of screen)
      #define TOP_FIXED_AREA 48 // Number of lines in top fixed area (lines counted from top of screen)
      #define YMAX 320 // Bottom of screen area
      #define minimum(a,b)     (((a) < (b)) ? (a) : (b))
      //#define MENU_FONT NULL
      #define MENU_FONT &FreeMono9pt7b // Winner
      //#define MENU_FONT &FreeMonoBold9pt7b
      //#define MENU_FONT &FreeSans9pt7b
      //#define MENU_FONT &FreeSansBold9pt7b
      #define BUTTON_SCREEN_LIMIT 12
      #define BUTTON_ARRAY_LEN BUTTON_SCREEN_LIMIT
      #define STATUS_BAR_WIDTH 16
      #define LVGL_TICK_PERIOD 6

      #define FRAME_X 100
      #define FRAME_Y 64
      #define FRAME_W 120
      #define FRAME_H 50
    
      // Red zone size
      #define REDBUTTON_X FRAME_X
      #define REDBUTTON_Y FRAME_Y
      #define REDBUTTON_W (FRAME_W/2)
      #define REDBUTTON_H FRAME_H
    
      // Green zone size
      #define GREENBUTTON_X (REDBUTTON_X + REDBUTTON_W)
      #define GREENBUTTON_Y FRAME_Y
      #define GREENBUTTON_W (FRAME_W/2)
      #define GREENBUTTON_H FRAME_H
    
      #define STATUSBAR_COLOR 0x5A44
    
      #define KIT_LED_BUILTIN 13
#endif

#ifdef MARAUDER_V7_1
      #define CHAN_PER_PAGE 7

      #define SCREEN_CHAR_WIDTH 40
      //#define HAS_ILI9341
    
      #define BANNER_TEXT_SIZE 2

#ifndef TFT_WIDTH
        #define TFT_WIDTH 240
#endif

#ifndef TFT_HEIGHT
        #define TFT_HEIGHT 320
#endif

      #define GRAPH_VERT_LIM TFT_HEIGHT/2 - 1

      #define TFT_DIY

      #define SCREEN_BUFFER

      #define MAX_SCREEN_BUFFER 22

      #define EXT_BUTTON_WIDTH 0

      #define SCREEN_ORIENTATION 0
    
      #define CHAR_WIDTH 12
      #define SCREEN_WIDTH TFT_WIDTH
      #define SCREEN_HEIGHT TFT_HEIGHT
      #define HEIGHT_1 TFT_WIDTH
      #define WIDTH_1 TFT_HEIGHT
      #define STANDARD_FONT_CHAR_LIMIT (TFT_WIDTH/6) // number of characters on a single line with normal font
      #define TEXT_HEIGHT 16 // Height of text to be printed and scrolled
      #define BOT_FIXED_AREA 0 // Number of lines in bottom fixed area (lines counted from bottom of screen)
      #define TOP_FIXED_AREA 48 // Number of lines in top fixed area (lines counted from top of screen)
      #define YMAX 320 // Bottom of screen area
      #define minimum(a,b)     (((a) < (b)) ? (a) : (b))
      //#define MENU_FONT NULL
      #define MENU_FONT &FreeMono9pt7b // Winner
      //#define MENU_FONT &FreeMonoBold9pt7b
      //#define MENU_FONT &FreeSans9pt7b
      //#define MENU_FONT &FreeSansBold9pt7b
      #define BUTTON_SCREEN_LIMIT 12
      #define BUTTON_ARRAY_LEN BUTTON_SCREEN_LIMIT
      #define STATUS_BAR_WIDTH 16
      #define LVGL_TICK_PERIOD 6

      #define FRAME_X 100
      #define FRAME_Y 64
      #define FRAME_W 120
      #define FRAME_H 50
    
      // Red zone size
      #define REDBUTTON_X FRAME_X
      #define REDBUTTON_Y FRAME_Y
      #define REDBUTTON_W (FRAME_W/2)
      #define REDBUTTON_H FRAME_H
    
      // Green zone size
      #define GREENBUTTON_X (REDBUTTON_X + REDBUTTON_W)
      #define GREENBUTTON_Y FRAME_Y
      #define GREENBUTTON_W (FRAME_W/2)
      #define GREENBUTTON_H FRAME_H
    
      #define STATUSBAR_COLOR 0x5A44
    
      #define KIT_LED_BUILTIN 13
#endif

  



#endif
  //// END DISPLAY DEFINITIONS

  //// MENU DEFINITIONS


  // Status bar right-side icon x-positions (SCREEN_WIDTH-relative)
  // V8 (240px): SD=170 WiFi=154 Force=138 Touch=186 Bat=204
  // Pancake (320px): SD=250 WiFi=234 Force=218 Touch=266 Bat=284
  #define SB_MEM_X    (SCREEN_WIDTH / 2 - 20)    // D%/P% text: 120 on V8, 160 on Pancake
  #define SB_SD_X     (SCREEN_WIDTH - 70)
  #define SB_WIFI_X   (SCREEN_WIDTH - 86)
  #define SB_FORCE_X  (SCREEN_WIDTH - 102)
  #define SB_TOUCH_X  (SCREEN_WIDTH - 54)
  #define SB_BAT_X    (SCREEN_WIDTH - 36)

  // Packet monitor oscilloscope geometry
  // PKT_HALF  = landscape height midpoint (zero-line y)
  // PKT_AXIS_W = x-axis draw width
  // HEIGHT_1 = TFT_WIDTH (landscape height): V8=240 Pancake=320
  // WIDTH_1  = TFT_HEIGHT (landscape width):  V8=320 Pancake=480
  #define PKT_HALF    (HEIGHT_1 / 2)
  #define PKT_AXIS_W  (WIDTH_1 - 10)

#if defined(MARAUDER_V8)
    #define BANNER_TIME 100
    
    #define COMMAND_PREFIX "!"
    
    // Keypad start position, key sizes and spacing
    #define KEY_X 120 // Centre of key
    #define KEY_Y 50
    #define KEY_W 240 // Width and height
    #define KEY_H 22
    #define KEY_SPACING_X 0 // X and Y gap
    #define KEY_SPACING_Y 1
    #define KEY_TEXTSIZE 1   // Font size multiplier
    #define ICON_W 22
    #define ICON_H 22
    #define BUTTON_PADDING 22
    //#define BUTTON_ARRAY_LEN 5
#endif






#ifdef MARAUDER_V7
    #define BANNER_TIME 100
    
    #define COMMAND_PREFIX "!"
    
    // Keypad start position, key sizes and spacing
    #define KEY_X 120 // Centre of key
    #define KEY_Y 50
    #define KEY_W 240 // Width and height
    #define KEY_H 22
    #define KEY_SPACING_X 0 // X and Y gap
    #define KEY_SPACING_Y 1
    #define KEY_TEXTSIZE 1   // Font size multiplier
    #define ICON_W 22
    #define ICON_H 22
    #define BUTTON_PADDING 22
    //#define BUTTON_ARRAY_LEN 5
#endif

#ifdef MARAUDER_V7_1
    #define BANNER_TIME 100
    
    #define COMMAND_PREFIX "!"
    
    // Keypad start position, key sizes and spacing
    #define KEY_X 120 // Centre of key
    #define KEY_Y 50
    #define KEY_W 240 // Width and height
    #define KEY_H 22
    #define KEY_SPACING_X 0 // X and Y gap
    #define KEY_SPACING_Y 1
    #define KEY_TEXTSIZE 1   // Font size multiplier
    #define ICON_W 22
    #define ICON_H 22
    #define BUTTON_PADDING 22
    //#define BUTTON_ARRAY_LEN 5
#endif

  



#if defined(MARAUDER_CARDPUTER_ADV)
    #define BANNER_TIME 50

    #define COMMAND_PREFIX "!"

    // Keypad start position, key sizes and spacing
    #define KEY_X (SCREEN_WIDTH/2) // Centre of key
    #define KEY_Y (TFT_HEIGHT/6)
    #define KEY_W SCREEN_WIDTH // Width and height
    #define KEY_H (TFT_HEIGHT/17)
    #define KEY_SPACING_X 0 // X and Y gap
    #define KEY_SPACING_Y 1
    #define KEY_TEXTSIZE 1   // Font size multiplier
    #define ICON_W 22
    #define ICON_H 22
    #define BUTTON_PADDING 7
#endif

  //// END MENU DEFINITIONS

  //// SD DEFINITIONS
#if defined(USE_SD)










#ifdef MARAUDER_V7
      #define SD_CS 4
#endif

#ifdef MARAUDER_V7_1
      #define SD_CS 4
#endif



#if defined(MARAUDER_CARDPUTER_ADV)
      //#define SS      12
      #define SD_CS   12
      #define SD_SCK  40
      #define SD_MISO 39
      #define SD_MOSI 14
#endif







#ifdef MARAUDER_V8
      #define SD_CS 10
#endif



#endif
  //// END SD DEFINITIONS

  //// SPACE SAVING COLORS
  #define TFTWHITE     1
  #define TFTCYAN      2
  #define TFTBLUE      3
  #define TFTRED       4
  #define TFTGREEN     5
  #define TFTGREY      6
  #define TFTGRAY      7
  #define TFTMAGENTA   8
  #define TFTVIOLET    9
  #define TFTORANGE    10
  #define TFTYELLOW    11
  #define TFTLIGHTGREY 12
  #define TFTPURPLE    13
  #define TFTNAVY      14
  #define TFTSILVER    15
  #define TFTDARKGREY  16
  #define TFTSKYBLUE   17
  #define TFTLIME      18
  #define TFTGOLD      19
  //// END SPACE SAVING COLORS

  #define TFT_FARTGRAY 0x528a

  //// SCREEN STUFF
#ifndef HAS_SCREEN

    #define BANNER_TIME GRAPH_REFRESH

    #define TFT_WIDTH 0
    #define TFT_HEIGHT 0

    #define TFT_BLACK 0
    #define TFT_WHITE 0
    #define TFT_CYAN 0
    #define TFT_BLUE 0
    #define TFT_RED 0
    #define TFT_GREEN 0
    #define TFT_GREY 0
    #define TFT_GRAY 0
    #define TFT_MAGENTA 0
    #define TFT_VIOLET 0
    #define TFT_ORANGE 0
    #define TFT_YELLOW 0
    #define STANDARD_FONT_CHAR_LIMIT 40
    #define FLASH_BUTTON -1

    #define CHAN_PER_PAGE 7

    #include <FS.h>
    #include <functional>
    #include <LinkedList.h>
    #include "SPIFFS.h"
    #include "Assets.h"

#endif
  //// END SCREEN STUFF

  //// MEMORY LOWER LIMIT STUFF
  // These values are in bytes
#if defined(MARAUDER_CARDPUTER_ADV)
    #define MEM_LOWER_LIM 10000
#elif defined(MARAUDER_V7)
    #define MEM_LOWER_LIM 10000
#elif defined(MARAUDER_V7_1)
    #define MEM_LOWER_LIM 10000
#elif defined(MARAUDER_V8)
    #define MEM_LOWER_LIM 10000
#else
    #define MEM_LOWER_LIM 10000
#endif
  //// END MEMORY LOWER LIMIT STUFF

  //// NEOPIXEL STUFF  
#ifdef HAS_NEOPIXEL_LED
    
#if defined(MARAUDER_V8)
      #define PIN 27
#elif defined(MARAUDER_CARDPUTER_ADV)
      #define PIN 21
#else
      #define PIN 25
#endif
  
#endif
  //// END NEOPIXEL STUFF

  //// EVIL PORTAL STUFF

#ifdef HAS_PSRAM
    #define MAX_HTML_SIZE 30000
#else
    #define MAX_HTML_SIZE 11400
#endif

  //// END EVIL PORTAL STUFF

  //// GPS STUFF
#ifdef HAS_GPS
#ifdef HAS_PSRAM
      #define mac_history_len 500
#else
      #define mac_history_len 100
#endif

    #define mac_history_len_half (mac_history_len / 2)

#if defined(MARAUDER_V7)
      #define GPS_SERIAL_INDEX 2
      #define GPS_TX 21
      #define GPS_RX 22
#elif defined(MARAUDER_V7_1)
      #define GPS_SERIAL_INDEX 2
      #define GPS_TX 21
      #define GPS_RX 22
#elif defined(MARAUDER_CARDPUTER_ADV)
      #define GPS_SERIAL_INDEX 1
      #define GPS_TX 15
      #define GPS_RX 13
#elif defined(MARAUDER_V8)
      #define GPS_SERIAL_INDEX 1
      #define GPS_TX 14
      #define GPS_RX 13
#endif
#else
    #define mac_history_len 100
    #define mac_history_len_half (mac_history_len / 2)
#endif
  //// END GPS STUFF

  //// BATTERY STUFF
#ifdef HAS_BATTERY

#if defined(MARAUDER_V7)
      #define I2C_SDA 33
      #define I2C_SCL 16
      // Our v7 rig carries a MAX17048 fuel gauge (detected fine on v1.12.1).
      // v1.13.0's stock config assigns v7 the IP5306 driver, which the selector
      // below then uses to #undef HAS_MAX1704X -> the fuel gauge is never probed
      // -> the battery readout goes blank. Point our v7 at the chip it actually has.
      #define HAS_MAX1704X

#elif defined(MARAUDER_V7_1)
      #define I2C_SDA 33
      #define I2C_SCL 27

#elif defined(MARAUDER_V8)
      #define I2C_SCL 4
      #define I2C_SDA 5

#endif


    //  If we know what we have, we can delete what we're not using
#ifdef BATTERY_ADC_PIN
      #undef HAS_AXP2101
      #undef HAS_IP5306
      #undef HAS_MAX1704X
      #undef HAS_AXP192

    // No driver for this LiPo charger
#elif defined(HAS_TP4057)
      #undef HAS_AXP2101
      #undef HAS_IP5306
      #undef HAS_MAX1704X
      #undef HAS_AXP192

#elif defined(HAS_IP5306)
      #undef HAS_AXP2101
      #undef HAS_MAX1704X
      #undef HAS_AXP192

#elif defined(HAS_AXP192)
      #undef HAS_AXP2101
      #undef HAS_IP5306
      #undef HAS_MAX1704X

#elif defined(HAS_AXP2101)
      #undef HAS_IP5306
      #undef HAS_MAX1704X

#elif defined(HAS_MAX1704X)
      #undef HAS_AXP2101
      #undef HAS_IP5306
      #undef HAS_AXP192


#else
       // Auto-detect fallback for a board that never named its gauge. Today that
       // is V7.1, and anything built with WARROOM_RIG_ALLOW_UNTESTED_BOARD.
       //
       // IP5306 and MAX17048 are I2C parts. BatteryInterface::RunSetup() probes
       // each by address on the board's real I2C bus (I2C_SDA/I2C_SCL) and only
       // latches on when the chip actually answers, so defining both is a
       // harmless "try one address, then the other" -- normal auto-detect.
       //
       // HAS_AXP192 was defined here too and does not belong: its probe is not
       // address-guarded. axp192_obj.begin() unconditionally does
       // Wire1.begin(21, 22) and then blind-writes PMU registers, whether or not
       // anything is on that bus. No board that reaches this fallback carries an
       // AXP192, and GPIO21/22 is a used bus on the boards that do reach it --
       // the GPS UART on V7.1, the C5's flash CLK/MOSI on V8. So the fallback was
       // firing a second I2C master onto live pins on every board that landed in
       // it. Dropped. The two address-guarded I2C probes above stay.
       // #define HAS_AXP2101
       #define HAS_IP5306
       #define HAS_MAX1704X
#endif

#endif


  //// MARAUDER TITLE STUFF
#if defined(MARAUDER_V7)
    #define MARAUDER_TITLE_BYTES 13578
#elif defined(MARAUDER_V7_1)
    #define MARAUDER_TITLE_BYTES 13578
#elif defined(MARAUDER_V8)
    #define MARAUDER_TITLE_BYTES 13578
#else
    #define MARAUDER_TITLE_BYTES 13578
#endif
  //// END MARAUDER TITLE STUFF

  //// PCAP BUFFER STUFF
  
#ifdef HAS_PSRAM
    #define BUF_SIZE 8 * 1024 // Had to reduce buffer size to save RAM. GG @spacehuhn
    #define SNAP_LEN 1 * 4096 // max len of each recieved packet
  //#elif !defined(HAS_ILI9341)
  //  #define BUF_SIZE 8 * 1024 // Had to reduce buffer size to save RAM. GG @spacehuhn
  //  #define SNAP_LEN 4096 // max len of each recieved packet
#else
    #define BUF_SIZE 3 * 1024 // Had to reduce buffer size to save RAM. GG @spacehuhn
    #define SNAP_LEN 2324 // max len of each recieved packet
#endif

  //// PCAP BUFFER STUFF

  //// STUPID CYD STUFF
#if defined(HAS_CYD_TOUCH) || defined(HAS_C5_SD) || defined(HAS_SEPARATE_SD)




#ifdef MARAUDER_V8
      #define SD_MISO TFT_MISO
      #define SD_MOSI TFT_MOSI
      #define SD_SCK  TFT_SCLK
#endif







#ifdef MARAUDER_V7
      #define SD_MISO TFT_MISO
      #define SD_MOSI TFT_MOSI
      #define SD_SCK  TFT_SCLK
#endif

#endif
  //// END STUPID CYD STUFF

  //// FUNNY FLIPPER LED STUFF

#ifdef HAS_FLIPPER_LED





#endif

  //// END FUNNY FLIPPER LED STUFF

  //// WIFI STUFF

#ifndef HAS_DUAL_BAND
    #define HOP_DELAY 1000
#else
    #define HOP_DELAY 250
#endif

  // ============================================================
  // MARAUDER_CORE_MODE Konstanten
  // ============================================================
#ifdef MARAUDER_CORE_MODE
    #define WARDRIVE_CORE_CHANNEL            6
    // 12 nodes plaintext; with encryption the real ceiling is 6 encrypted
    // ESP-NOW peers (see the note in WardriveCore.h).
    #define WARDRIVE_CORE_MAX_NODES          12
    #define WARDRIVE_CORE_QUEUE_LEN          12
    #define WARDRIVE_CORE_DISPLAY_REFRESH_MS 500
    #define WARDRIVE_CORE_RATE_WINDOW_MS     15000   // lines/min sampling window
    #define WARDRIVE_CORE_NODE_TIMEOUT_MS    60000
    #define WARDRIVE_CORE_HEAP_MIN_INIT      30000
    #define WARDRIVE_CORE_HEAP_MIN_RUNTIME   15000

    // Erste 14 Eintraege in scan_channels[] sind 2.4 GHz, die restlichen 26
    // sind 5 GHz. Wird in WardriveCore.cpp::recalculateChannelAssignments()
    // genutzt wenn WARDRIVE_2_4_ONLY definiert ist.
    #define WARDRIVE_2_4_CHANNEL_COUNT       14
#endif

  // ============================================================
  // MARAUDER_WDGWARS_UPLOAD Konstanten
  // ============================================================
#ifdef MARAUDER_WDGWARS_UPLOAD
    #define WDGWARS_DISPLAY_REFRESH_MS 500
    #define WDGWARS_EXIT_HOLD_MS       2000
    #define WDGWARS_AP_TIMEOUT_MS      20000
    #define WDGWARS_TLS_TIMEOUT_MS     15000
    #define WDGWARS_HTTP_TIMEOUT_MS    30000
#endif
  //// ACT LED STUFF
#ifdef HAS_ACT_LED

#ifdef MARAUDER_V8
      #define ACT_LED_PIN 28
#endif

#endif

#endif

// =========================================================================
// warroom-rig: boot trace
// =========================================================================
// Enable with -DWARROOM_BOOT_TRACE. Emits a marker before and after each step
// of setup(), so a boot that stops partway can be placed from the serial log
// alone. setup() is long and mostly silent, and a hang in it is indexed from
// the outside as "the device is dead" -- same symptom as a panic, same symptom
// as a boot loop. Compiles to nothing when the flag is absent.
#ifdef WARROOM_BOOT_TRACE
  #define WR_MARK(s) do { Serial.print(F("[BOOT] ")); Serial.println(F(s)); \
                          Serial.flush(); delay(8); } while (0)
#else
  #define WR_MARK(s) do {} while (0)
#endif

// =========================================================================
// warroom-rig: supported-hardware gate
// =========================================================================
// This file described ESP32Marauder's whole hardware matrix -- 27 selectable
// targets -- while the rig builds for three (V7, V7.1, Cardputer ADV) plus a V8
// that is named everywhere but is still an unfinished port, gated off in its own
// block below. The others compiled, booted, and then behaved in ways nobody here
// had ever seen, which is worse than not building at all: it looks like support.
// They are gone from this file now, so the gate mostly catches the case of no
// target being defined.
//
// The Cardputer is why the gate exists, and now also why it is not a wall.
// Someone found the Marauder config in this repo, reasonably assumed their
// board was supported, and got a console that accepted no input at all: the
// Cardputer has no D-pad (U/D/L/R_BTN are -1) and no touch panel, so every
// input path in RigUI compiled out. The answer was to build the missing path
// rather than to keep saying no -- the ADV is a supported target, driven by
// its keyboard.
//
// What a further port needs is the same thing: an input backend in RigInput
// and a layout that survives the screen's aspect. The scanning, logging and
// upload sides are board-agnostic. Define WARROOM_RIG_ALLOW_UNTESTED_BOARD to
// build for something else and find out; the gate is here to stop accidents,
// not to stop you.
#if !defined(WARROOM_RIG_ALLOW_UNTESTED_BOARD)
#if !defined(MARAUDER_V7) && !defined(MARAUDER_V7_1) && !defined(MARAUDER_V8) && \
    !defined(MARAUDER_CARDPUTER_ADV)
    #error "warroom-rig builds for Marauder V7 / V7.1 and the M5 Cardputer ADV \
(V8 is recognized but gated off as an unfinished port -- see the block below). \
No board target is defined, or the one defined is not one of those -- the rig's UI \
and input paths exist only for those, and other targets can boot with no working \
input at all. To port anyway, define WARROOM_RIG_ALLOW_UNTESTED_BOARD. \
See README.md, section Hardware."
#endif
#endif

// =========================================================================
// warroom-rig: Marauder V8 (ESP32-C5) is NOT a finished port
// =========================================================================
// V8 is a real board and its name is wired through this whole config, but the
// port was never actually done -- and this is verified broken against the
// shipped binary, not doubted on paper. Building it yields an image that fights
// its own flash during setup and then boots into a UI that accepts no input.
// Until someone with the hardware AND the schematic finishes it, the gate
// refuses V8 the same way the block above refuses an unlisted board. Define
// WARROOM_RIG_ALLOW_BROKEN_V8 to build it anyway and pick up the work. Four
// concrete things are wrong, all of them because the config still describes a
// classic-ESP32 Marauder rather than the C5:
//
//   1. Panel / pins. The V8 build passes no panel flags, so TFT_eSPI falls back
//      to libs/CustomTFT_eSPI/User_Setup_Select.h, whose single uncommented
//      include is User_Setup_dual_nrf24.h -- the OG Marauder ILI9341 panel
//      (CS 17, DC 26, MOSI 23, SCLK 18, BL 32). On the ESP32-C5 those are
//      MSPI/flash territory (flash CS0=16, MISO=17, WP=18, HD=20, CLK=21,
//      MOSI=22) and GPIO32 does not exist at all (SOC_GPIO_PIN_COUNT is 29). The
//      shipped image really does drive flash /WP and flash Q during setup. The
//      C5 needs its own panel config in build flags, the way the Cardputer ADV
//      does -- but we do not have the V8 schematic, so the right pins are
//      unknown from this end. Do not guess them.
//   2. Battery gauge. V8 sets I2C_SCL 4 / I2C_SDA 5 but no gauge macro, so it
//      used to land in the auto-detect fallback and pull in HAS_AXP192, whose
//      probe blind-writes on Wire1.begin(21, 22) -- the C5's flash CLK/MOSI.
//      The fallback no longer defines HAS_AXP192 (see the battery block above),
//      so that particular landmine is defused, but V8 still has no real gauge.
//   3. GPS UART. GPS_TX 14 / GPS_RX 13 collide with the C5 variant header's
//      USB_DP 14 / USB_DM 13. With CDCOnBoot=cdc (mandatory on the C5, see
//      RELEASING.md) that hands the USB-CDC console's own pins to the GPS driver
//      mid-setup.
//   4. Input. The V8 block sets HAS_TOUCH and leaves HAS_BUTTONS commented, so
//      touch is the only input path -- but the active TFT setup defines
//      TOUCH_CS -1, so the touch controller is never selected and getTouch()
//      can only ever return false. The board boots into a UI nothing can drive.
//
// None of these is guessable without the board in hand; this note exists so the
// person who has one starts from the right four problems instead of finding them
// again the hard way. See README.md, section Hardware.
#if defined(MARAUDER_V8) && !defined(WARROOM_RIG_ALLOW_BROKEN_V8)
    #error "warroom-rig: MARAUDER_V8 is not a finished port and is disabled. The \
ESP32-C5 config still describes a classic-ESP32 Marauder: the default panel pins \
land on the C5's flash bus, the GPS UART sits on the USB-CDC pins, and touch input \
is wired to TOUCH_CS -1 so nothing on screen responds. This was verified against \
the shipped binary, not guessed. To work on the port with the hardware in hand, \
define WARROOM_RIG_ALLOW_BROKEN_V8. See the note directly above this line and \
README.md, section Hardware."
#endif
