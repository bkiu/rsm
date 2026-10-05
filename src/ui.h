// Screen UI: node list, chat, region picker and an info page. Everything is drawn into an off-screen canvas and
// pushed in one go, using large fonts sized for the Cardputer's 240x135 display.
#pragma once
#include <stdint.h>

#include "input.h"

void uiBegin();
void uiTick(); // handles input events and redraws when needed
void uiNotifyIncoming(uint32_t thread);
