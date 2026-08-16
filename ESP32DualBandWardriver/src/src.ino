#include "configs.h"
#include "BatteryInterface.h"
#include "Buffer.h"
#include "settings.h"
#include "display.h"
#include "GpsInterface.h"
#include "SDInterface.h"
#include "Switches.h"
#include "WiFiOps.h"
#include "utils.h"
#include "ui.h"
#include "logger.h"

Buffer buffer;
Settings settings;
GpsInterface gps;
BatteryInterface battery;
WiFiOps wifi_ops;
Utils utils;
UI ui_obj;

SPIClass sharedSPI(SPI);
Display display = Display(&sharedSPI, TFT_CS, TFT_DC, TFT_RST);
SDInterface sd_obj = SDInterface(&sharedSPI, SD_CS);

Switches u_btn = Switches(U_BTN, 1000, U_PULL);
Switches d_btn = Switches(D_BTN, 1000, D_PULL);
Switches c_btn = Switches(C_BTN, 1000, C_PULL);

void setup() {
  Serial.begin(115200);

  // Wait a moment for a USB-CDC host, but never wait for one indefinitely.
  //
  // With CDCOnBoot=cdc, `Serial` only becomes true once a host opens the port.
  // A node on a bench supply or a power bank has no host and never will, so an
  // unbounded wait here is a node that boots on a desk and is dead in the field
  // -- which is exactly how it presented: the XIAOs joined when plugged into the
  // PC and never joined on the same supply that ran the Waveshare boards fine.
  //
  // The exemption used to be spelled `#ifndef C5_ZERO_NODE`, so every target
  // added afterwards inherited the blocking wait by default. A deadline instead
  // of a per-board exception keeps the convenience (attach a console within the
  // window and you still see the whole boot) and cannot single out the next
  // board somebody adds.
  {
    const uint32_t serial_wait_ms = 1500;
    const uint32_t serial_wait_start = millis();
    while (!Serial && (millis() - serial_wait_start < serial_wait_ms))
      delay(10);
  }

  // Diagnostic: log why we (re)booted and how much heap is free, so an
  // occasional brownout / watchdog / panic during unpaired operation is visible
  // on the next boot. Every headless node wants this, not just the Zero -- it is
  // the first thing worth reading when one of them fails to come up somewhere
  // you cannot attach a console.
  Serial.printf("\n[BOOT] reset_reason=%d  free_heap=%u\n",
                (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());

#ifdef ANT_SWITCH_PIN
  // Waveshare C5-Zero: select the external IPEX antenna via the onboard RF
  // switch BEFORE any radio init. The power-on default is the internal ceramic
  // antenna, which leaves a connected external antenna out of the RF path.
  pinMode(ANT_SWITCH_PIN, OUTPUT);
  digitalWrite(ANT_SWITCH_PIN, ANT_EXTERNAL_LEVEL);
#endif

  // Do SPI stuff first
  sharedSPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  // Give SPI some time I guess
  delay(100);

  // Init the display before SD
  display.begin();

  // Give SD some time
  delay(100);

  // Show us IDF information
  Logger::log(STD_MSG, "ESP-IDF version is: " + String(esp_get_idf_version()));

  pinMode(LED_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);

  // Load settings
  settings.begin();

  if (settings.getSettingType(SETTING_SANITY) == "") {
    Logger::log(WARN_MSG, "Current settings format not supported. Installing new default settings...");
    settings.createDefaultSettings(SPIFFS);
  }
  else {
    Logger::log(GUD_MSG, "Current settings format supported");
  }

  // Init our buffer for writing logs
  buffer = Buffer();

  // Init SD Card
  if(!sd_obj.initSD())
    Logger::log(WARN_MSG, "SD Card NOT Supported");

  // Check for firmware updates now
  Logger::log(STD_MSG, "Checking for firmware updates...");
  sd_obj.runUpdate();

  // Init battery
  battery.RunSetup();
  battery.battery_level = battery.getBatteryLevel();

  // Init GPS
#ifndef C5_ZERO_NODE
  gps.begin();
#else
  // Headless Zero node has no GPS — the HUB supplies position. Its inherited
  // JCMK UART pins (16/17) collide with C5 flash/reserved pins → skip the
  // UART init entirely. gps.getFixStatus() stays false downstream.
#endif

  ui_obj.begin();

  // Init wifi and bluetooth.
  // Headless NODE builds have no UI to use the web-admin SoftAP, so skip it
  // unconditionally — otherwise the device sits in admin mode for 60 s every
  // boot before falling through to scan+heartbeat.
#ifdef NODE
  wifi_ops.begin(true);
#else
  wifi_ops.begin(c_btn.justPressed());
#endif

  // Init UI
  ui_obj.begin();

  settings.printJsonSettings(settings.getSettingsString());

  Logger::log(GUD_MSG, "Initialization complete!");
}

void loop() {
  // Take current time of this loop for functions
  uint32_t currentTime = millis();

#ifdef C5_ZERO_NODE
  // Diagnostic: every 4 s emit reset reason (latched for this boot) + heap, so a
  // brownout/WDT/panic and any heap leak are visible without catching the boot.
  static uint32_t s_last_status = 0;
  if (currentTime - s_last_status > 4000) {
    s_last_status = currentTime;
    Serial.printf("[STATUS] reason=%d heap=%u minheap=%u up=%lus\n",
                  (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(), (unsigned long)(currentTime / 1000));
  }
#endif

  // Refresh all functions
  wifi_ops.main(currentTime);
  settings.main(currentTime);
  battery.main(currentTime);
  gps.main();
  sd_obj.main();
  buffer.save();
  ui_obj.main(currentTime);

  // Solo or Core modes
  if ((gps.getFixStatus()) && (sd_obj.supported) && (ui_obj.stat_display_mode != SD_FILES))
    wifi_ops.setCurrentScanMode(WIFI_WARDRIVING);
  // Nodes
  else if ((wifi_ops.run_mode == NODE_MODE) && (wifi_ops.getNodeReady())) {
    wifi_ops.setCurrentScanMode(WIFI_WARDRIVING);
    digitalWrite(LED_PIN, HIGH);
  }
  else {
    wifi_ops.setCurrentScanMode(WIFI_STANDBY);
    if (wifi_ops.run_mode == NODE_MODE)
      digitalWrite(LED_PIN, LOW);
  }
}
