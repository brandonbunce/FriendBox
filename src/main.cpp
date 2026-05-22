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
#include "canvas.hpp"
#include "display.hpp"
#include "io.hpp"
#include "network.hpp"
#include "ui_core.hpp"

#define FRIENDBOX_DEBUG_MODE true
#define FRIENDBOX_SOFTWARE_VERSION "Software v0.4"

static const char *TAG = "friendbox";

static void initFriendbox()
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
    drawFriendboxLoadingScreen("Playing Sketch", 250, sketch_id, "ENJOY :)");
    playFboxAnimationFromSD(path);
}

static void uiLoopTask(void *)
{
    while (true) {
        handleTouch();
        handleCanvasDraw();
        handleTouchUIUpdate();
        // We just gotta run this on loop until we can set up interrupts.
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

    initMenuButton();
    initFriendbox();

    // v4: streamed playback with interleaved per-frame audio. Single code path
    // for all sizes — no PSRAM-full constraint.
    playSketchFromServer("1778969174678"); // Kitty Dithered 24fps
    playSketchFromServer("1776836243916"); // Dithering Glitch Test
    playSketchFromServer("1776835153465"); // Ben Troll Physics 24fps
    //playSketchFromServer("1776836243916"); // Troll Physics 4 16fps

    // The old Arduino setup/loop split is replaced by app_main + a pinned
    // UI loop task. Core 0 runs the producer/decoder for animations; pin
    // the UI loop to core 1 so it shares the main-task core.
    xTaskCreatePinnedToCore(uiLoopTask, "ui_loop", 8192, nullptr, 1, nullptr, 1);
}
