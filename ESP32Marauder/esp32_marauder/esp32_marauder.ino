/* FLASH SETTINGS
Board: LOLIN D32
 Frequency: 80MHz
Partition Scheme: Minimal SPIFFS
https://www.online-utility.org/image/convert/to/XBM
*/

#include "configs.h"

#ifndef HAS_SCREEN
  #define MenuFunctions_h
  #define Display_h
#endif

#include <stdio.h>

#include "RigInput.h"

#ifdef HAS_GPS
  #include "GpsInterface.h"
#endif

#include "Assets.h"
#include "GorillaSplash.h"
#include "WiFiScan.h"
#ifdef MARAUDER_CORE_MODE
  #include "WardriveCore.h"
#endif
#ifdef MARAUDER_FILE_SERVER_AP
  #include "FileServerAP.h"
#endif
#ifdef MARAUDER_WDGWARS_UPLOAD
  #include "WdgwarsUpload.h"
#endif
#ifdef HAS_SD
  #include "SDInterface.h"
#endif
#include "Buffer.h"

#ifdef HAS_FLIPPER_LED
  #include "flipperLED.h"
#elif defined(XIAO_ESP32_S3)
  #include "xiaoLED.h"
#elif defined(MARAUDER_M5STICKC) || defined(MARAUDER_M5STICKCP2)
#elif defined(HAS_NEOPIXEL_LED)
  #include "LedInterface.h"
#endif

#include "settings.h"
#include "lang_var.h"

#ifdef HAS_BATTERY
  #include "BatteryInterface.h"
#endif

#ifdef HAS_SCREEN
  #include "Display.h"
  #include "MenuFunctions.h"
  #include "RigUI.h"
#endif

#ifdef HAS_BUTTONS
  #include "Switches.h"
  
  #if (U_BTN >= 0)
    Switches u_btn = Switches(U_BTN, 1000, U_PULL);
  #endif
  #if (D_BTN >= 0)
    Switches d_btn = Switches(D_BTN, 1000, D_PULL);
  #endif
  #if (L_BTN >= 0)
    Switches l_btn = Switches(L_BTN, 1000, L_PULL);
  #endif
  #if (R_BTN >= 0)
    Switches r_btn = Switches(R_BTN, 1000, R_PULL);
  #endif
  #if (C_BTN >= 0)
    Switches c_btn = Switches(C_BTN, 1000, C_PULL);
  #endif

#endif

WiFiScan wifi_scan_obj;
Buffer buffer_obj;
Settings settings_obj;

#ifdef MARAUDER_CORE_MODE
  WardriveCore wardrive_core_obj;
#endif

#ifdef MARAUDER_WDGWARS_UPLOAD
  WdgwarsUpload wdgwars_upload_obj;
#endif

#ifdef MARAUDER_FILE_SERVER_AP
  // FileServerAP also defines `file_server_ap_obj` in its .cpp as the global
  // singleton; this declaration is intentional (mirror of upstream pattern
  // used by WardriveCore). The header re-declares it `extern`.
#endif

#ifdef HAS_GPS
  GpsInterface gps_obj;
#endif

#ifdef HAS_BATTERY
  BatteryInterface battery_obj;
#endif

#ifdef HAS_SCREEN
  Display display_obj;
  MenuFunctions menu_function_obj;
  RigUI rig_ui_obj;              // owns the console; delegates the legacy tool tree
#endif

#if defined(HAS_SD) && !defined(HAS_C5_SD)
  SDInterface sd_obj;
#endif

#ifdef HAS_FLIPPER_LED
  flipperLED flipper_led;
#elif defined(XIAO_ESP32_S3)
  xiaoLED xiao_led;
#elif defined(MARAUDER_M5STICKC) || defined(MARAUDER_M5STICKCP2)
  stickcLED stickc_led;
#elif defined(HAS_NEOPIXEL_LED)
  LedInterface led_obj;
#endif

const String PROGMEM version_number = MARAUDER_VERSION;

#ifdef HAS_NEOPIXEL_LED
  Adafruit_NeoPixel strip = Adafruit_NeoPixel(Pixels, PIN, NEO_GRB + NEO_KHZ800);
#endif

uint32_t currentTime  = 0;

// PWM Brightness Control
#ifdef HAS_SCREEN
  #include <Preferences.h>
  #define BL_CHANNEL 0
  #define BL_FREQ 5000
  #define BL_RESOLUTION 8
  const uint8_t BL_LEVELS[] = {26, 51, 77, 102, 128, 153, 179, 204, 230, 255};
  const uint8_t BL_NUM_LEVELS = 10;
  uint8_t bl_level_idx = 9; // default full brightness
  Preferences bl_prefs;
#endif

// Helper macros for LEDC API compatibility (2.x vs 3.x board package)
#ifdef HAS_SCREEN
  #ifndef HAS_MINI_SCREEN
    #if ESP_ARDUINO_VERSION_MAJOR >= 3
      #define BL_SETUP()       ledcAttach(TFT_BL, BL_FREQ, BL_RESOLUTION)
      #define BL_SET(duty)     ledcWrite(TFT_BL, (duty))
    #else
      #define BL_SETUP()       do { ledcSetup(BL_CHANNEL, BL_FREQ, BL_RESOLUTION); ledcAttachPin(TFT_BL, BL_CHANNEL); } while(0)
      #define BL_SET(duty)     ledcWrite(BL_CHANNEL, (duty))
    #endif
  #endif
#endif

#ifndef HAS_MINI_SCREEN
  void brightnessInit() {
    #ifdef HAS_SCREEN
      BL_SETUP();
      bl_prefs.begin("backlight", false);
      bl_level_idx = bl_prefs.getUChar("level", 9);
      if (bl_level_idx >= BL_NUM_LEVELS) bl_level_idx = 9;
      BL_SET(BL_LEVELS[bl_level_idx]);
    #endif
  }

  void brightnessCycle() {
    #ifdef HAS_SCREEN
      bl_level_idx = (bl_level_idx + 1) % BL_NUM_LEVELS;
      BL_SET(BL_LEVELS[bl_level_idx]);
      bl_prefs.putUChar("level", bl_level_idx);
      Serial.print(F("[Brightness] Level "));
      Serial.print(bl_level_idx + 1);
      Serial.print(F("/"));
      Serial.print(BL_NUM_LEVELS);
      Serial.print(F(" ("));
      Serial.print(BL_LEVELS[bl_level_idx] * 100 / 255);
      Serial.println(F("%)"));
    #endif
  }

  uint8_t getBrightnessLevel() {
    #ifdef HAS_SCREEN
      return bl_level_idx;
    #else
      return 0;
    #endif
  }

  void brightnessSave(uint8_t level) {
    #ifdef HAS_SCREEN
      if (level >= BL_NUM_LEVELS) level = BL_NUM_LEVELS - 1;
      bl_level_idx = level;
      BL_SET(BL_LEVELS[bl_level_idx]);
      bl_prefs.putUChar("level", bl_level_idx);
    #endif
  }

  void backlightOn() {
    #ifdef HAS_SCREEN
      BL_SET(BL_LEVELS[bl_level_idx]);
    #endif
  }

  void backlightOff() {
    #ifdef HAS_SCREEN
      BL_SET(0);
    #endif
  }
#else
  void backlightOn() {
    #ifdef HAS_SCREEN
      #if defined(MARAUDER_MINI) || defined(MARAUDER_MINI_V3)
        digitalWrite(TFT_BL, LOW);
      #endif
    
      #if !defined(MARAUDER_MINI) && !defined(MARAUDER_MINI_V3)
        digitalWrite(TFT_BL, HIGH);
      #endif
    #endif
  }

  void backlightOff() {
    #ifdef HAS_SCREEN
      #if defined(MARAUDER_MINI) || defined(MARAUDER_MINI_V3)
        digitalWrite(TFT_BL, HIGH);
      #endif
    
      #if !defined(MARAUDER_MINI) && !defined(MARAUDER_MINI_V3)
        digitalWrite(TFT_BL, LOW);
      #endif
    #endif
  }
#endif

#ifdef HAS_C5_SD
  SPIClass sharedSPI(SPI);
  SDInterface sd_obj = SDInterface(&sharedSPI, SD_CS);
#endif

void setup()
{
  #if defined(WARROOM_BOOT_TRACE) && defined(HAS_SCREEN)
    // Liveness signal that needs no PC: if the backlight comes up, the image
    // booted and reached the first line of setup(). Normally the backlight is
    // held off here until the splash is drawn, which makes a healthy boot and a
    // dead board look identical from the outside for the whole of setup().
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  #endif

  randomSeed(esp_random());
  
  #ifndef DEVELOPER
    esp_log_level_set("*", ESP_LOG_NONE);
  #endif
  
  #ifndef HAS_IDF_3
    esp_spiram_init();
  #endif

  Serial.begin(115200);

  #ifdef HAS_ACT_LED
    pinMode(ACT_LED_PIN, OUTPUT);
    delay(100);
    digitalWrite(ACT_LED_PIN, LOW);
  #endif

  // Wait for the console, but never longer than this. On the V7 `Serial` is a
  // HardwareSerial and operator bool() is true the moment the driver is
  // installed, so this returns immediately and the wait was invisible. On any
  // board that boots with USB CDC (Cardputer ADV, and the C5 when built with
  // CDCOnBoot=cdc) `Serial` is HWCDC, whose operator bool() stays false until a
  // host actually opens the port -- so an unbounded wait here is a boot hang on
  // battery, on a charger, and on a PC with no terminal attached. The screen is
  // set up further down, so the device just looks dead.
  //
  // The diagnostic build waits far longer on purpose. Its whole output happens
  // in setup(), so a trace nobody is listening to is a trace that never
  // existed -- and on native USB there is no way to attach *during* a boot that
  // takes a second. Waiting instead means the sequence is flash, open the
  // terminal whenever, and the device starts talking at that moment. No reset
  // to time by hand.
  #ifdef WARROOM_BOOT_TRACE
    // The diagnostic build waits much longer than the shipping one, because its
    // entire output happens in setup() and a trace nobody is listening to is a
    // trace that never existed.
    //
    // It is still bounded, and that is not a detail. Waiting forever deadlocks
    // against the host: opening the port asserts DTR and RTS together, which is
    // the USB-Serial-JTAG reset gesture, so the board reboots straight back into
    // this wait -- open, reset, wait, open, reset, and not one byte ever comes
    // out. A deadline breaks the cycle: whichever attempt does not land inside
    // the window, the board carries on and the next one does. Ten seconds is
    // comfortably longer than a reset plus USB re-enumeration.
    const uint32_t serial_wait_ms = 10000;
    const uint32_t serial_wait_start = millis();
    while (!Serial && (millis() - serial_wait_start < serial_wait_ms))
      delay(10);
  #else
    const uint32_t serial_wait_ms = 1500;
    const uint32_t serial_wait_start = millis();
    while (!Serial && (millis() - serial_wait_start < serial_wait_ms))
      delay(10);
  #endif

  #ifdef HAS_C5_SD
    sharedSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
    delay(100);
  #endif

  #if defined(MARAUDER_M5STICKCP2) // Prevent StickCP2 from turning off when disconnect USB cable
    pinMode(POWER_HOLD_PIN, OUTPUT);
    digitalWrite(POWER_HOLD_PIN, HIGH);
  #endif
  
  #ifdef HAS_SCREEN
    pinMode(TFT_BL, OUTPUT);
  #endif
  
  backlightOff();
  #if BATTERY_ANALOG_ON == 1
    pinMode(BATTERY_PIN, OUTPUT);
    pinMode(CHARGING_PIN, INPUT);
  #endif
  
  // Preset SPI CS pins to avoid bus conflicts
  #ifdef HAS_SCREEN
    digitalWrite(TFT_CS, HIGH);
  #endif
  
  #if defined(HAS_SD) && !defined(HAS_C5_SD)
    pinMode(SD_CS, OUTPUT);

    delay(10);
  
    digitalWrite(SD_CS, HIGH);

    delay(10);
  #endif

  #ifdef MARAUDER_CARDPUTER_ADV
    // The Cap LoRa-1262 hangs its SX1262 on the same SPI bus as the microSD
    // (MOSI 14 / MISO 39 / SCK 40), with its own chip select on G5. We never
    // talk to the radio, but an undriven NSS lets it answer traffic meant for
    // the card. Park it high so the SD card owns the bus alone. Harmless when
    // no cap is attached: G5 is otherwise unused.
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);
  #endif

  //Serial.begin(115200);

  //while(!Serial)
  //  delay(10);

  Serial.println("ESP-IDF version is: " + String(esp_get_idf_version()));
  WR_MARK("setup entered, serial up");

  #ifdef HAS_PSRAM
    if (!psramInit()) {
      Serial.println(F("PSRAM not available"));
    }
  #endif

  #ifdef HAS_SIMPLEX_DISPLAY
    #if defined(HAS_SD)
      // Do some SD stuff
      if(!sd_obj.initSD())
        Serial.println(F("SD Card NOT Supported"));

    #endif
  #endif

  #ifdef HAS_SCREEN
    WR_MARK("-> display_obj.RunSetup()");
    display_obj.RunSetup();
    WR_MARK("<- display_obj.RunSetup()");
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  #endif

  // Init PWM brightness AFTER display init (so ledcAttach overrides TFT_eSPI's pinMode)
  #ifndef HAS_MINI_SCREEN
    brightnessInit();
    backlightOff();
  #endif

  #ifdef HAS_SCREEN
    #if !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
      // warroom-rig boot splash: berserker gorilla (shares the warroom PWA art).
      // The screen is drawn while the backlight is still off (backlightOff()
      // above), then the backlight is faded up so the gorilla materialises out
      // of the dark instead of snapping on.
      display_obj.tft.fillScreen(TFT_BLACK);
      display_obj.tft.setSwapBytes(true);
      display_obj.tft.pushImage(0, 0, GORILLA_SPLASH_W, GORILLA_SPLASH_H, gorilla_splash);
      display_obj.tft.setSwapBytes(false);
      display_obj.tft.fillRect(0, TFT_HEIGHT - 52, TFT_WIDTH, 52, TFT_BLACK);
      display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
      display_obj.tft.setTextSize(2);
      display_obj.tft.drawCentreString("warroom-rig", TFT_WIDTH/2, TFT_HEIGHT - 46, 1);
      display_obj.tft.setTextSize(1);
      display_obj.tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      display_obj.tft.drawCentreString(WARROOM_RIG_VERSION, TFT_WIDTH/2, TFT_HEIGHT - 16, 1);
      display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
      { // fade the backlight from black up to the saved level (~1.8 s rise)
        uint8_t bl_target = BL_LEVELS[bl_level_idx];
        for (int d = 0; d <= bl_target; d += 2) { BL_SET(d); delay(14); }
        BL_SET(bl_target);
      }
      delay(2600); // hold the gorilla on screen
    #else
      display_obj.tft.drawCentreString("warroom-rig", TFT_HEIGHT/2, TFT_WIDTH * 0.33, 1);
      display_obj.tft.drawCentreString("based on Marauder", TFT_HEIGHT/2, TFT_WIDTH * 0.5, 1);
      display_obj.tft.drawCentreString(display_obj.version_number, TFT_HEIGHT/2, TFT_WIDTH * 0.66, 1);
    #endif
  #endif


  WR_MARK("splash drawn");
  backlightOn(); // Need this

  #ifdef HAS_SCREEN
    // Do some stealth mode stuff
    #ifdef HAS_BUTTONS
      if (c_btn.justPressed()) {
        display_obj.headless_mode = true;

        backlightOff();
      }
    #endif
  #endif

  WR_MARK("-> settings_obj.begin()");
  settings_obj.begin();
  WR_MARK("<- settings_obj.begin()");

  const char* type = settings_obj.getSettingType("ChanHop");

  if (type == nullptr || type[0] == '\0') {
    Serial.println(F("Current settings format not supported. Installing new default settings..."));
    settings_obj.createDefaultSettings(SPIFFS);
  }

  buffer_obj = Buffer();

  #ifndef HAS_SIMPLEX_DISPLAY
    #if defined(HAS_SD)
      // Do some SD stuff
      if(!sd_obj.initSD())
        Serial.println(F("SD Card NOT Supported"));

    #endif
  #endif

  WR_MARK("-> wifi_scan_obj.RunSetup()");
  wifi_scan_obj.RunSetup();
  WR_MARK("<- wifi_scan_obj.RunSetup()");

  #ifdef HAS_SCREEN
    display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
    display_obj.tft.drawCentreString("Initializing...", TFT_WIDTH/2, TFT_HEIGHT * 0.82, 1);
  #endif

  #ifdef HAS_BATTERY
    battery_obj.RunSetup();
  #endif

  #ifdef HAS_BATTERY
    battery_obj.battery_level = battery_obj.getBatteryLevel();
  #endif

  // Do some LED stuff
  #ifdef HAS_FLIPPER_LED
    flipper_led.RunSetup();
  #elif defined(XIAO_ESP32_S3)
    xiao_led.RunSetup();
  #elif defined(MARAUDER_M5STICKC)
    stickc_led.RunSetup();
  #elif defined(HAS_NEOPIXEL_LED)
    led_obj.RunSetup();
  #endif

  #ifdef HAS_GPS
    WR_MARK("-> gps_obj.begin()");
    gps_obj.begin();
    WR_MARK("<- gps_obj.begin()");
  #endif

  #ifdef HAS_SCREEN  
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  #endif

  // Input comes up before anything can be asked to respond to it. On the
  // Cardputer ADV this is what brings the TCA8418 keyboard onto the I2C bus;
  // on the button boards it has nothing to do.
  WR_MARK("-> RigInput::begin()  [I2C keyboard]");
  RigInput::begin();
  WR_MARK("<- RigInput::begin()");

  #ifdef HAS_SCREEN
    #ifdef MARAUDER_CARDPUTER_ADV
      display_obj.clearScreen();
    #endif
    WR_MARK("-> menu_function_obj.RunSetup()");
    menu_function_obj.RunSetup();   // builds the menu tree (incl. the legacy tools)
    WR_MARK("-> rig_ui_obj.init()");
    rig_ui_obj.init();              // ...then RigUI takes the screen and draws the console
  #endif

  /*char ssidBuf[64] = {0};  // or prefill with existing SSID
  if (keyboardInput(ssidBuf, sizeof(ssidBuf), "Enter SSID")) {
    // user pressed OK
    Serial.println(ssidBuf);
  } else {
    Serial.println(F("User exited keyboard"));
  }

  menu_function_obj.changeMenu(menu_function_obj.current_menu);*/

  WR_MARK("setup complete, entering loop()");
  wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
}


void loop()
{
  currentTime = millis();
  bool mini = false;

  #ifdef WARROOM_BOOT_TRACE
    // Heartbeat. Everything else this flag prints happens once, inside setup(),
    // which makes reading it a race the host usually loses: on native USB the
    // console is only "attached" while a handle is open, and opening one resets
    // the board. Miss the window and a healthy boot is indistinguishable from a
    // dead one -- both are silent.
    //
    // A line per second removes the race entirely, and answers the question
    // that matters on its own: reaching loop() at all means setup() got past
    // display bring-up.
    static uint32_t wr_hb_ms = 0;
    if (currentTime - wr_hb_ms > 1000) {
      wr_hb_ms = currentTime;
      Serial.printf("[HB] up=%lus heap=%u\n",
                    (unsigned long)(currentTime / 1000),
                    (unsigned)ESP.getFreeHeap());
    }
  #endif

  #ifdef SCREEN_BUFFER
    #ifndef HAS_ILI9341
      mini = true;
    #endif
  #endif

  #if (defined(HAS_ILI9341) && !defined(MARAUDER_CYD_2USB))
    #ifdef HAS_BUTTONS
      if (c_btn.isHeld()) {
        if (menu_function_obj.disable_touch)
          menu_function_obj.disable_touch = false;
        else
          menu_function_obj.disable_touch = true;

        menu_function_obj.updateStatusBar();

        while (!c_btn.justReleased())
          delay(1);
      }
    #endif
  #endif

  // Update all of our objects
  wifi_scan_obj.main(currentTime);

  #ifdef HAS_GPS
    gps_obj.main();
  #endif

  // Save buffer to SD and/or serial
  buffer_obj.save();

  #ifdef HAS_BATTERY
    battery_obj.main(currentTime);
  #endif
  if ((wifi_scan_obj.currentScanMode != WIFI_PACKET_MONITOR) ||
      (mini)) {
    #ifdef HAS_SCREEN
      // RigUI is the entry point now. It handles our own screens and calls
      // MenuFunctions::main() itself while the legacy tool tree has the display.
      rig_ui_obj.main(currentTime);
    #endif
  }
  #ifdef HAS_FLIPPER_LED
    flipper_led.main();
  #elif defined(XIAO_ESP32_S3)
    xiao_led.main();
  #elif defined(MARAUDER_M5STICKC)
    stickc_led.main();
  #elif defined(HAS_NEOPIXEL_LED)
    led_obj.main(currentTime);
  #endif

  #ifdef HAS_SCREEN
    delay(1);
  #else
    delay(50);
  #endif
}
