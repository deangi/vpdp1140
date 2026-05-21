#pragma once

// ---- App metadata ----
#define APP_TITLE       "v8088"
#define APP_VERSION     "1.0"
#define APP_BUILD_DATE  "2026-05-20"

// ---- RGB LED (WS2812) ----
#define LED_PIN         42
#define LED_CHANNEL     0
#define LED_COUNT       1

// ---- Onboard button ----
#define BUTTON_PIN      0

// ---- TFT (ILI9341, configured via TFT_eSPI FNK0104B preset) ----
// Reference only - actual pins live in TFT_eSPI User_Setup_Select.h (FNK0104B):
//   TFT_MISO=13 TFT_MOSI=11 TFT_SCLK=12 TFT_CS=10 TFT_DC=46 TFT_BL=45 @ 40 MHz
#define TFT_W           320
#define TFT_H           240
#define TEXT_COLS       80
#define TEXT_ROWS       25
#define CELL_W          4
#define CELL_H          8

// ---- Capacitive touch FT6336U (I2C) ----
#define TOUCH_SDA       16
#define TOUCH_SCL       15
#define TOUCH_RST       18
#define TOUCH_INT       17
#define TOUCH_I2C_ADDR  0x38

// ---- SD_MMC 4-bit ----
#define SD_MMC_CMD      40
#define SD_MMC_CLK      38
#define SD_MMC_D0       39
#define SD_MMC_D1       41
#define SD_MMC_D2       48
#define SD_MMC_D3       47

// ---- File paths on SD (defaults; overridden by /config.ini) ----
#define CFG_PATH        "/config.ini"
#define DEFAULT_A_IMG   "/dos331.img"
#define DEFAULT_C_IMG   "/hd_c_32mb.img"

// ---- Network ----
#define TELNET_PORT     23

// ---- Disk geometries ----
#define FD_BYTES        1474560UL          // 80 x 2 x 18 x 512
#define FD_CYL          80
#define FD_HEADS        2
#define FD_SEC          18
#define HD_BYTES        33554432UL         // 1024 x 4 x 16 x 512 = exact 32 MB
#define HD_CYL          1024
#define HD_HEADS        4
#define HD_SEC          16

// ---- Boot tuning ----
#define WIFI_CONNECT_TIMEOUT_MS  20000
