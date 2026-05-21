#pragma once
#include <stdint.h>

class TFT_eSPI;

void ui_init();

// Open the settings menu (caller detects the double-tap gesture).
void ui_open();

// True while the settings menu is open (the caller should pause the CPU).
bool ui_is_open();

// Feed a tap to the open menu. Returns true if the UI consumed it.
bool ui_handle_tap(int x, int y);

// Draw the menu overlay when open and the screen needs a repaint.
void ui_draw(TFT_eSPI& tft);

// One-shot: returns true once if the user asked to reboot the 8088.
bool ui_consume_reboot();
