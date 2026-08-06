#pragma once

// =========================================================================
// TFT_eSPI panel configuration, chosen per board
// =========================================================================
// TFT_eSPI picks its configuration in exactly one of three ways, and the
// vendored copy of the library was using the bluntest of them: libs/
// CustomTFT_eSPI/User_Setup.h hard-codes ILI9341_DRIVER and the Marauder V7's
// SPI pins for every build. That works as long as every board is a V7.
//
// It stops working the moment one is not, and it stops working *silently*:
//
//   - TFT_Drivers/ILI9341_Defines.h sets TFT_WIDTH 240 / TFT_HEIGHT 320 with no
//     #ifndef guard, so it overwrites whatever configs.h decided for the board.
//     arduino-cli compiles with -w by default, so the macro-redefined warning
//     that would have given this away is never printed.
//   - The override lands only in translation units that reach TFT_eSPI.h after
//     configs.h, so the panel size ended up *different in different .cpp files*
//     of the same build: MenuFunctions.cpp and RigUI.cpp saw 240x320 while
//     WardriveCore.cpp and WdgwarsUpload.cpp saw the Cardputer's 135x240.
//     Every screen drawn from the first pair was laid out for the wrong panel.
//
// TFT_eSPI includes this file, if it exists, before its own setup selection and
// then treats the configuration as done (it defines USER_SETUP_LOADED itself
// afterwards). So this file has to describe *all* the boards, not just the new
// one -- which is why the V7/V8 branch below is a verbatim copy of what
// User_Setup.h was already applying to them. Their builds must not move.
// =========================================================================

#include "configs.h"

#if defined(MARAUDER_CARDPUTER_ADV)

  // M5Stack Cardputer ADV: ST7789, 135x240 in its native portrait orientation,
  // used in landscape (configs.h sets SCREEN_ORIENTATION 1). Pins mirror the
  // TFT_* values in the board's configs.h block; they are repeated here because
  // this file is read before those macros can be relied upon to survive.
  #define ST7789_DRIVER
  #define TFT_WIDTH  135
  #define TFT_HEIGHT 240

  #define TFT_MOSI 35
  #define TFT_SCLK 36
  #define TFT_CS   37
  #define TFT_DC   34
  #define TFT_RST  33
  #define TFT_BL   38
  #define TFT_BACKLIGHT_ON HIGH

  // Fonts: the same set the other boards load, so no drawing call has to ask
  // which font numbers exist on which panel.
  #define LOAD_GLCD
  #define LOAD_FONT2
  #define LOAD_FONT4
  #define LOAD_FONT6
  #define LOAD_FONT7
  #define LOAD_FONT8
  #define LOAD_GFXFF
  #define SMOOTH_FONT

  #define SPI_FREQUENCY       40000000
  #define SPI_READ_FREQUENCY  20000000

#else

  // Marauder V7 / V7.1 / V8 -- verbatim from libs/CustomTFT_eSPI/User_Setup.h,
  // which is what these boards were built with before this file existed.
  #define ILI9341_DRIVER

  #define TFT_MISO 19 // Matching T_DO
  #define TFT_MOSI 23 // Matching T_DIN
  #define TFT_SCLK 18 // Matching T_CLK
  #define TFT_CS   17 // Chip select control pin
  #define TFT_DC   16 // Data Command control pin
  #define TFT_RST   5 // Reset pin (could connect to RST pin)
  #define TFT_BL   32 // LED back-light
  #define TOUCH_CS 21 // Chip select pin (T_CS) of touch screen

  #define LOAD_GLCD
  #define LOAD_FONT2
  #define LOAD_FONT4
  #define LOAD_FONT6
  #define LOAD_FONT7
  #define LOAD_FONT8
  #define LOAD_GFXFF
  #define SMOOTH_FONT

  #define SPI_FREQUENCY        27000000 // Actually sets it to 26.67MHz = 80/3
  #define SPI_READ_FREQUENCY   20000000
  #define SPI_TOUCH_FREQUENCY   2500000

#endif
