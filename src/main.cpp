#include <Arduino.h>

#include "secrets.hpp"
#include "canvas.hpp"
#include "display.hpp"
#include "io.hpp"
#include "network.hpp"
#include "ui.hpp"

// (C) 2025-2026 Brandon Bunce - FriendBox System Software
#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.3"

struct Friend
{
  String name;
  int userID;
};

// Functions
bool initNVS();

void initFriendbox()
{
  currentDrawColorIndex = 0 + (esp_random() % (15 - 0 + 1));
  initDisplay();
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing SD");
  if (initSD(false))
  {
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing SD", "Done!");
  }
  else
  {
    drawFriendboxLoadingScreen("Starting...", 0, "Initializing SD", "Error! Retrying...");
    while (!initSD())
    {
      delay(1000);
    }
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing SD", "Done!");
  }
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing Touch");
  if (initTouch(false))
  {
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing Touch", "Done!");
  }
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing Wi-Fi", NETWORK_SSID);
  if (initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME))
  {
    drawFriendboxLoadingScreen("Starting...", 250, "Initializing Wi-Fi", "Done!");
  }
  drawFriendboxLoadingScreen("Starting...", 0, "Initializing NVS");
  if (initNVS())
  {
    drawFriendboxLoadingScreen("Starting...", 500, "Initializing NVS", "Done!");
  }
  nvs.begin("Friendbox", true);
  loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
  nvs.end();
  changeScreenContext(SCREEN_CANVAS);
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
}

void loop()
{
  handleTouch();
  handleCanvasDraw();
  handleTouchUIUpdate();
  // We just gotta run this on loop until we can set up interrupts.
  handleMenuButton(false);
}
