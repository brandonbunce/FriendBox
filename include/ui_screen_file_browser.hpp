#ifndef UI_SCREEN_FILE_BROWSER_HPP
#define UI_SCREEN_FILE_BROWSER_HPP

#include "ui_core.hpp"

#define SCREEN_FILE_BROWSER_FILE_BUTTON_COUNT 5
#define SCREEN_FILE_BROWSER_NAVI_BUTTON_COUNT 4

void initUIForScreenFileBrowser();
void onEnterScreenFileBrowser();
void drawScreenFileBrowser(int page = 0);
void drawScreenFileBrowserDefault();
void handleTouchUIUpdateScreenFileBrowser();

#endif
