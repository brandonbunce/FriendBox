// (C) 2025-2026 Brandon Bunce - FriendBox System Software
// ESP-IDF entry point.

#include <stdio.h>
#include <sys/stat.h>

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "idf_compat.hpp"

#include "secrets.hpp"
#include "auth.hpp"
#include "canvas.hpp"
#include "display.hpp"
#include "io.hpp"
#include "mem.hpp"
#include "network.hpp"
#include "ui.hpp"
#include "lt_assets.hpp"
#include "audio_i2s.hpp"

#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.4"

static const char *TAG = "friendbox";

static void initFriendbox()
{
    // Select random pallette color on startup.
    currentDrawColorIndex = 0 + (esp_random() % (15 - 0 + 1));
    initDisplay();
    initLTAssets();
    loadDisplayBrightnessFromNVS();
    ltShowSplash();
    registerFriendboxScreens();   // must precede any changeScreenContext()
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
            vTaskDelay(pdMS_TO_TICKS(1000));
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
        puts("Inited Touch!");
        drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 250, "Initializing Touch", "Done!");
    }
    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Initializing NVS");
    if (initNVS())
    {
        drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 500, "Initializing NVS", "Done!");
    }
    authInit();
    loadI2SVolumeFromNVS();
    setI2SVolume(10);
    ui::initSfx();
    tft.fillScreen(draw_color_palette_text_color[currentDrawColorIndex]);
    if (couldInitCanvasFrameBuffer)
    {
        nvs.begin("Friendbox", true);
        //loadImageFromSD(nvs.getUInt("lastActiveSlot", 8));
        nvs.end();
    }

    // ── First-run gate ──
    // No Wi-Fi credentials stored → onboarding (OOBE) walks the user through
    // Wi-Fi setup, then sign-in, before reaching the home shell.
    if (!wifiIsConfigured())
    {
        changeScreenContext(SCREEN_OOBE_WELCOME);
        return;
    }

    drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Initializing Wi-Fi");
    bool wifiOk = networkConnectSaved();
    memReport("post-wifi");
    if (wifiOk)
    {
        drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 250, "Initializing Wi-Fi", "Done!");
    }
    else
    {
        drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 1000, "Initializing Wi-Fi", "Failed!  Networked functions will not work.");
    }

    // ── Sign-in gate ──
    // No token → pairing screen. Token → validate against the server; only an
    // explicit rejection unpairs (a transport failure keeps the appliance
    // usable offline with the stored token intact).
    if (wifiOk && !authHasToken())
    {
        changeScreenContext(SCREEN_PAIRING);   // build() starts the pairing task
        return;
    }
    if (wifiOk)
    {
        drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 0, "Signing in");
        switch (authValidateToken())
        {
            case AuthCheck::VALID:
                drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 500, "Signing in",
                                           authUsername());
                break;
            case AuthCheck::INVALID:
                authForgetToken();
                drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 800, "Signing in",
                                           "Signed out by server.");
                changeScreenContext(SCREEN_PAIRING);
                return;
            case AuthCheck::NET_ERROR:
                drawFriendboxLoadingScreen(FRIENDBOX_SOFTWARE_VERSION, 800, "Signing in",
                                           "Server unreachable - staying signed in.");
                break;
        }
    }
    changeScreenContext(SCREEN_HOME);
}

/* Download by sketch ID and then play from SD. */
static void playSketchFromServer(const char *sketch_id)
{
    char path[80];
    snprintf(path, sizeof(path), "/sd/sketches/received/%s.fbox", sketch_id);
    mkdir("/sd/sketches", 0775);            // no-op if it already exists
    mkdir("/sd/sketches/received", 0775);
    struct stat st;
    if (stat(path, &st) != 0) {
        drawFriendboxLoadingScreen("Downloading sketch", 0, sketch_id, "Touch to skip not available");
        if (!networkDownloadFbox(sketch_id, path)) {
            drawFriendboxLoadingScreen("Download failed", 1500, sketch_id);
            return;
        }
    }
    drawFriendboxLoadingScreen("Playing Sketch", 0, sketch_id, "ENJOY :)");
    playFboxAnimationFromSD(path);
}

static void uiLoopTask(void *)
{
    while (true) {
        // A 401 from any API call signs the device out and re-enters pairing.
        // Playback blocks this task, so the transition can never tear down an
        // active playback pipeline.
        if (authConsume401() && currentScreen != SCREEN_PAIRING) {
            puts("[auth] server rejected token — returning to pairing");
            authForgetToken();
            changeScreenContext(SCREEN_PAIRING);
        }
        handleTouch();          // GT911 -> touchX/Y/Z globals
        ui::tick();             // custom tick (canvas paint) + widget dispatch + anim
        ui::sfxTick();          // close the idle SFX session (kills I2S underrun buzz)
        handleMenuButton(false);
        vTaskDelay(1);  // yield: 1 tick keeps the IDLE/WDT happy
    }
}


extern "C" void app_main(void)
{
    // NVS, esp_netif, and the default event loop must come up early so
    // any later component (WiFi, NvsStore, esp_http_client) can use them.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();

#ifdef FRIENDBOX_DEBUG_MODE
    ESP_LOGI(TAG, "FriendBox %s - DEBUG", FRIENDBOX_SOFTWARE_VERSION);
#endif

    // Reserve the 115 KB internal playback decode buffer NOW, while internal RAM
    // is still pristine and contiguous (before display/SD/WiFi fragment it). This
    // guarantees .fbox playback always gets the fast internal decode path.
    memInit();
    memReservePlaybackScratch();
    memReport("boot");

    initMenuButton();
    initFriendbox();
    //playSketchFromServer("1780336773819"); // F12 v2
    //playSketchFromServer("1779592535336"); // OW Gameplay
    //playSketchFromServer("1776797823148"); // Troll Physics 2
    //playSketchFromServer("1778969174678"); // Kitty Dithered 24fps
    //playSketchFromServer("1776836243916"); // Dithering Glitch Test
    //playSketchFromServer("1776835153465"); // Ben Troll Physics 24fps

    // Core 0 runs the producer/decoder for animations; pin
    // the UI loop to core 1 so it shares the main-task core.
    //
    // Stack: loadSketchFromSD() (home background + file browser) puts a
    // FboxRleReader — which holds a 4 KB buf[] — plus a 960 B scanline buffer on
    // the stack, then descends the f_open → FATFS → sdmmc → heap chain on top.
    // 8 KB overflowed into the heap (StoreProhibited in the allocator); 16 KB
    // leaves comfortable headroom.
    xTaskCreatePinnedToCore(uiLoopTask, "ui_loop", 16384, nullptr, 1, nullptr, 1);
}
