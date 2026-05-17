#include <Arduino.h>

#include "secrets.hpp"
#include "canvas.hpp"
#include "display.hpp"
#include "io.hpp"
#include "network.hpp"
#include "ui_core.hpp"

// (C) 2025-2026 Brandon Bunce - FriendBox System Software
#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.4"

// Functions
bool initNVS();

void initFriendbox()
{
  currentDrawColorIndex = 0 + (esp_random() % (15 - 0 + 1));
  initDisplay();
  drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Initializing SD");
  if (initSD(false))
  {
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 250, "Initializing SD", "Done!");
  }
  else
  {
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Initializing SD", "Error! Retrying...");
    while (!initSD())
    {
      delay(1000);
    }
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 250, "Initializing SD", "Done!");
  }
  if (initCanvas())
  {
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 250, "Initializing Canvas", "Done!");
  }
  else
  {
    drawFriendboxLoadingScreen("Fiddlesticks!", 2000, "Canvas allocation failed!", "Pixel data will not be saved.");
  }
  drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Initializing Touch");
  if (true) // initTouch(false))
  {
    Serial.println("Inited Touch!");
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 250, "Initializing Touch", "Done!");
  }
  drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Initializing Wi-Fi", NETWORK_SSID);
  if (initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME))
  {
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 250, "Initializing Wi-Fi", "Done!");
  }
  else
  {
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 1000, "Initializing Wi-Fi", "Failed!  Networked functions will not work.");
  }
  drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Initializing NVS");
  if (initNVS())
  {
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 500, "Initializing NVS", "Done!");
  }
  tft.fillScreen(draw_color_palette_text_color[currentDrawColorIndex]);
  if (couldInitCanvasFrameBuffer)
  {
    nvs.begin("Friendbox", true);
    //loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
    nvs.end();
  }
  changeScreenContext(SCREEN_CANVAS);
}

static void playSketchFromServer(const char *sketch_id)
{
    char path[64];
    snprintf(path, sizeof(path), "/sketches/received/%s.fbox", sketch_id);
    SD_MMC.mkdir("/sketches/received");
    if (!SD_MMC.exists(path)) {
        drawFriendboxLoadingScreen("Downloading sketch", 0, sketch_id, "Touch to skip not available");
        bool ok = networkDownloadFbox(sketch_id, path);
        if (!ok) {
            drawFriendboxLoadingScreen("Download failed", 1500, sketch_id);
            return;
        }
    }
    drawFriendboxLoadingScreen("Playing Sketch", 250, sketch_id, "ENJOY :)");
    playFboxAnimationFromSD(path);
}

void setup()
{
  // cawkins was here
  Serial.begin(115200);
#ifdef FRIENDBOX_DEBUG_MODE
  Serial.print("FriendBox ");
  Serial.print(FRIENDBOX_SOFTWARE_VERSION);
  Serial.println(" - DEBUG");
#endif
  initMenuButton();
  initFriendbox();
  playSketchFromServer("1778969174678"); // Kitty Dithered 24fps
  playFboxAnimationFromSDBuffered("/sketches/received/1778969174678.fbox"); // Kitty Dithered 24fps PSRAM
  playSketchFromServer("1775852425606"); // J's Animation :)
  playSketchFromServer("1776835153465"); // Ben Troll Physics 24fps
  playSketchFromServer("1776836243916"); // Troll Physics 4 16fps
  //playFboxAnimationFromSD("/sketches/received/1778392265348.fbox");
}
 
void loop()
{
  handleTouch();
  handleCanvasDraw();
  handleTouchUIUpdate();
  // We just gotta run this on loop until we can set up interrupts.
  handleMenuButton(false);
}
