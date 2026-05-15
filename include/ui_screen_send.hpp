#ifndef UI_SCREEN_SEND_HPP
#define UI_SCREEN_SEND_HPP

#include "ui_core.hpp"

#define SCREEN_SEND_ADDRESSBOOK_BUTTON_COUNT 5
#define SCREEN_SEND_NAVI_BUTTON_COUNT 5

void initUIForScreenSend();
void onEnterScreenSend();
void drawScreenSend(int page = 0);
void drawScreenSendDefault();
void handleTouchUIUpdateScreenSend();

#endif
