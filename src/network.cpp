#include "network.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "idf_compat.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_tls.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>

static const char *TAG = "net";

static EventGroupHandle_t s_wifi_events = nullptr;
static constexpr int      kWifiConnectedBit = BIT0;
static constexpr int      kWifiFailedBit    = BIT1;
static int                s_retry_count     = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count++ < MAX_CONNECTION_ATTEMPTS) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, kWifiFailedBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, kWifiConnectedBit);
    }
}

bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_events, kWifiConnectedBit | kWifiFailedBit);
    s_retry_count = 0;

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (sta && hostname) {
        esp_netif_set_hostname(sta, hostname);
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) return false;

    esp_event_handler_instance_t any_id, got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, nullptr, &any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, nullptr, &got_ip);

    wifi_config_t wifi_cfg = {};
    std::strncpy((char *)wifi_cfg.sta.ssid, netSSID, sizeof(wifi_cfg.sta.ssid) - 1);
    std::strncpy((char *)wifi_cfg.sta.password, netPassword, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        kWifiConnectedBit | kWifiFailedBit,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(20000));

    return (bits & kWifiConnectedBit) != 0;
}

// --- esp_http_client helpers --------------------------------------------

// Sink for HTTP GET body bytes. Each ON_DATA callback appends; the helper
// reads it back once perform() returns.
struct HttpStringSink {
    std::string body;
};

static esp_err_t http_string_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        auto *sink = (HttpStringSink *)evt->user_data;
        sink->body.append((const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static bool httpGetString(const char *url, std::string &out, bool https_with_bundle = false)
{
    HttpStringSink sink;
    esp_http_client_config_t cfg = {};
    cfg.url            = url;
    cfg.event_handler  = http_string_event;
    cfg.user_data      = &sink;
    cfg.timeout_ms     = 15000;
    if (https_with_bundle) {
        cfg.transport_type    = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status / 100 != 2) {
        printf("httpGetString failed: err=%s status=%d\n", esp_err_to_name(err), status);
        return false;
    }
    out = std::move(sink.body);
    return true;
}

static bool httpPostBytes(const char *url, const uint8_t *body, size_t len,
                          const char *content_type, std::string *resp_out = nullptr)
{
    HttpStringSink sink;
    esp_http_client_config_t cfg = {};
    cfg.url           = url;
    cfg.method        = HTTP_METHOD_POST;
    cfg.event_handler = http_string_event;
    cfg.user_data     = &sink;
    cfg.timeout_ms    = 30000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, (int)len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (resp_out) *resp_out = std::move(sink.body);
    if (err != ESP_OK || status / 100 != 2) {
        printf("httpPostBytes failed: err=%s status=%d\n", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

// --- Public API ---------------------------------------------------------

std::vector<std::string> networkGetFriends()
{
    std::vector<std::string> friendNames;
    std::string payload;
    if (!httpGetString("http://192.168.1.8:8000/get/friends", payload)) {
        return friendNames;
    }
#ifdef FRIENDBOX_DEBUG_MODE
    printf("Successfully retrieved friends list! Payload: %s\n", payload.c_str());
#endif

    JsonDocument jsonDoc;
    DeserializationError error = deserializeJson(jsonDoc, payload);
    if (error) {
        printf("Failed to parse JSON: %s\n", error.c_str());
        return friendNames;
    }
    if (!jsonDoc.is<JsonArray>()) {
        puts("Response is not a JSON array!");
        return friendNames;
    }
    JsonArray array = jsonDoc.as<JsonArray>();
    for (JsonVariant v : array) {
        if (v.is<const char *>()) {
            friendNames.emplace_back(v.as<const char *>());
        }
    }
    printf("Parsed %u friends\n", (unsigned)friendNames.size());
    return friendNames;
}

// Local copy of paletteIndexForColor (matches the one in io.cpp).
static uint8_t paletteIndexForColorNet(uint16_t color)
{
    for (uint8_t i = 0; i < 16; i++) {
        if (draw_color_palette[i] == color) return i;
    }
    uint8_t best_idx = 0;
    uint32_t best_dist = UINT32_MAX;
    int r = (color >> 11) & 0x1F;
    int g = (color >> 5)  & 0x3F;
    int b = color & 0x1F;
    for (uint8_t i = 0; i < 16; i++) {
        uint16_t pc = draw_color_palette[i];
        int dr = ((pc >> 11) & 0x1F) - r;
        int dg = ((pc >> 5)  & 0x3F) - g;
        int db = (pc & 0x1F) - b;
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < best_dist) { best_dist = dist; best_idx = i; }
    }
    return best_idx;
}

bool networkSendCanvas()
{
    constexpr size_t framebufferSize = (TFT_HOR_RES * TFT_VER_RES) / 2;
    constexpr size_t bytesPerRow     = TFT_HOR_RES / 2;

    uint8_t *fb = (uint8_t *)malloc(framebufferSize);
    if (!fb) {
        puts("networkSendCanvas: malloc failed for upload buffer");
        return false;
    }
    uint16_t lineBuf[TFT_HOR_RES];
    for (int y = 0; y < TFT_VER_RES; y++) {
        tft.readRect(0, y, TFT_HOR_RES, 1, lineBuf);
        uint8_t *row = fb + (size_t)y * bytesPerRow;
        for (int x = 0; x < TFT_HOR_RES; x += 2) {
            uint8_t hi = paletteIndexForColorNet(lineBuf[x]) & 0x0F;
            uint8_t lo = paletteIndexForColorNet(lineBuf[x + 1]) & 0x0F;
            row[x >> 1] = (hi << 4) | lo;
        }
    }
    std::string resp;
    bool ok = httpPostBytes("http://192.168.1.8:8000/sketches/upload",
                            fb, framebufferSize, "application/octet-stream", &resp);
    free(fb);
    if (ok) {
        puts("Sketch uploaded successfully!");
        printf("%s\n", resp.c_str());
    }
    return ok;
}

bool networkDownloadFbox(const char *sketch_id, const char *dest_path)
{
    std::string url = "https://" FRIENDBOX_SERVER "/api/download/sketch/";
    url += sketch_id;
    printf("networkDownloadFbox: GET %s\n", url.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url               = url.c_str();
    cfg.transport_type    = HTTP_TRANSPORT_OVER_SSL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 60000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        printf("networkDownloadFbox: open failed: %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    int64_t contentLen = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status / 100 != 2) {
        printf("networkDownloadFbox: HTTP %d\n", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    printf("networkDownloadFbox: %lld bytes expected\n", (long long)contentLen);

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        printf("networkDownloadFbox: cannot create %s\n", dest_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    uint8_t buf[1024];
    int total = 0;
    bool write_err = false;
    while (true) {
        int n = esp_http_client_read(client, (char *)buf, (int)sizeof(buf));
        if (n < 0) { write_err = true; break; }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            puts("networkDownloadFbox: SD write failed (card full?)");
            write_err = true;
            break;
        }
        total += n;
        if (total % 10240 == 0) printf("  %d KB...\n", total / 1024);
    }
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    bool complete = !write_err && (contentLen <= 0 || total == contentLen);
    printf("networkDownloadFbox: %s — %d / %lld bytes to %s\n",
           complete ? "OK" : "INCOMPLETE", total, (long long)contentLen, dest_path);

    if (!complete) {
        unlink(dest_path);
        puts("networkDownloadFbox: partial file deleted");
        return false;
    }
    return true;
}

void networkSendFramebuffer(int userID)
{
    JsonDocument jsonDoc;
    jsonDoc["userID"] = userID;
    jsonDoc["data"]   = "FASTAPI SUCKS!";

    std::string jsonString;
    serializeJson(jsonDoc, jsonString);

    std::string resp;
    if (httpPostBytes("http://192.168.1.8:8000/posttest",
                      (const uint8_t *)jsonString.data(), jsonString.size(),
                      "application/json", &resp)) {
        printf("%s\n", resp.c_str());
    }
}

void networkReceiveFramebuffer() {}
