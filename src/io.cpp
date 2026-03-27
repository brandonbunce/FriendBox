#include "io.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "ui.hpp"

// Storage
Preferences nvs; // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html
SPIClass sdspi = SPIClass(HSPI);

bool initNVS()
{
  nvs.begin("Friendbox", true);
  // loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
  nvs.end();
  return true;
}

bool initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing SD...");
#endif
    sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdspi.setFrequency(40000000); // 40 MHz, explicitly otherwise will take 80mhz speed of display bus and cause corruption (?)
    if (!SD.begin(SD_CS, sdspi))
    {
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("ERROR: SD mount failed! Is it connected properly?");
#endif
        return false;
    }
    else
    {
        return true;
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("INFO: SD ready!");
#endif
    }
}

bool initMenuButton() {
    pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
    return true;
}

void saveImageToSD(int slot)
{
    drawFriendboxLoadingScreen("Saving...", 0);
    if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
    {
        // Invalid save slot.
        return;
    }
    char filename[50];
    snprintf(filename, sizeof(filename), "/sketches/slots/slot%d.fbox", slot);
    File f = SD.open(filename, FILE_WRITE);
    if (f)
    {
        f.write(canvas_framebuffer, (TFT_HOR_RES * TFT_VER_RES) / 2);
        f.close();
        currentSaveSlot = slot;
        nvs.begin("Friendbox", false);
        nvs.putUInt("lastActiveSlot", currentSaveSlot);
        nvs.end();
        drawFriendboxLoadingScreen("Saved!", 250);
        drawFramebuffer();
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.print("Saved image to save slot ");
        Serial.print(slot);
        Serial.println("!");
#endif
    }
    else
    {
        f.close();
        drawFriendboxLoadingScreen("ERROR: SAVE FAILED!", 1000);
        drawFramebuffer();
    }
}

void loadSketchFromSD(const char *path)
{
    drawFriendboxLoadingScreen("Loading...", 0);
    char filename[50];
    snprintf(filename, sizeof(filename), "/sketches/saved/%s", path);
    Serial.println("Loading: ");
    Serial.println(filename);
    File f = SD.open(filename, FILE_READ);
    if (f)
    {
        f.read(canvas_framebuffer, (TFT_VER_RES * TFT_HOR_RES) / 2);
        f.close();
        drawFramebuffer();
    }
    else
    {
        drawFriendboxLoadingScreen("Loading...", 500, "File Doesn't Exist :(");
    }
}

void loadImageFromSD(int slot)
{
    drawFriendboxLoadingScreen("Loading...", 0);
    if ((slot + 1) > SLOT_DROPDOWN_BUTTON_COUNT || slot < 0)
    {
        tft.print(slot);
        tft.println(" is not a valid save slot.");
        return;
    }
    char filename[50];
    snprintf(filename, sizeof(filename), "/sketches/slots/slot%d.fbox", slot);

    File f = SD.open(filename, FILE_READ);
    if (f)
    {
        f.read(canvas_framebuffer, (TFT_HOR_RES * TFT_VER_RES) / 2);
        f.close();
        drawFramebuffer();
        currentSaveSlot = slot;
        nvs.begin("Friendbox", false);
        nvs.putUInt("lastActiveSlot", currentSaveSlot);
        nvs.end();
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.print("Loaded image from save slot ");
        Serial.print(slot);
        Serial.println("!");
#endif
    }
    else
    {
        drawFriendboxLoadingScreen("No Sketch Saved!", 500);
        drawFramebuffer();
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.print("Cant load slot ");
        Serial.print(slot);
        Serial.println(" as it does not exist.");
#endif
    }
}

void handleMenuButton(bool recheckInput)
{
    if (currentScreen == SCREEN_CANVAS || currentScreen == SCREEN_CANVAS_MENU)
    {
        static unsigned long lastPress = 0;
        static unsigned int lastButtonState = 0;
        static bool alreadyPressed = false;
        if (digitalRead(HALL_SENSOR_PIN) == LOW) /*Button Pressed*/
        {
            if (lastPress == 0)
            {
                lastPress = millis();
            }
            if (((millis() >= (lastPress + DEBOUNCE_MILLISECONDS)) & !alreadyPressed) || recheckInput)
            {
                Serial.println("Logical Press");
                changeScreenContext(SCREEN_CANVAS_MENU);
                alreadyPressed = true;
            }
            else
            {
                return;
            }
        }
        else /*Button Released*/
        {
            if (lastPress && alreadyPressed)
            {
                lastPress = 0;
                alreadyPressed = false;
                Serial.println("Logical Release.");
                if (currentScreen != SCREEN_CANVAS)
                    changeScreenContext(SCREEN_CANVAS);
            }
        }
    }
}

std::vector<std::string> sdGetFboxFiles()
{
    std::vector<std::string> fileNames;
    File root = SD.open("/sketches/saved");
    if (root)
    {
        File entry;
        while (entry = root.openNextFile())
        {
            if (!entry.isDirectory())
            {
                Serial.println("Found file: " + String(entry.name()));
                fileNames.push_back(entry.name());
            }
            entry.close();
        }
        root.close();
    }
    return fileNames;
}