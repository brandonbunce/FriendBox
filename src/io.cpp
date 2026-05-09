#include "io.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "ui_core.hpp"

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

bool readFboxFromDisk(File &f) {
    return false;
}

bool writeFboxToDisk(File $f) {
    return false;
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