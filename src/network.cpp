#include "network.hpp"
#include "auth.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "nvs_store.hpp"
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
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_tls.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>

static const char *TAG = "net";

// The global NvsStore lives in io.cpp; WiFi creds persist in the same
// "Friendbox" namespace as the device token, brightness, and volume.
extern NvsStore nvs;
static const char *NVS_NS         = "Friendbox";
static const char *NVS_KEY_SSID   = "wifi_ssid";
static const char *NVS_KEY_PASS   = "wifi_pass";

static EventGroupHandle_t s_wifi_events = nullptr;
static constexpr int      kWifiConnectedBit = BIT0;
static constexpr int      kWifiFailedBit    = BIT1;
static int                s_retry_count     = 0;
static bool               s_wifi_inited     = false;  // driver up + STA started
static bool               s_connecting      = false;  // a connect attempt is live

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    // Connects are issued explicitly by wifiConnect(); the START handler stays
    // passive so a scan-only bring-up doesn't thrash the auto-reconnect logic.
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connecting && s_retry_count++ < MAX_CONNECTION_ATTEMPTS) {
            esp_wifi_connect();
        } else if (s_connecting) {
            xEventGroupSetBits(s_wifi_events, kWifiFailedBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, kWifiConnectedBit);
    }
}

// Bring the WiFi driver up in STA mode exactly once (idempotent). Required
// before either a connect or a scan. Does not initiate a connection.
static void wifiEnsureInited(const char *hostname)
{
    if (s_wifi_inited) return;
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (sta && hostname) esp_netif_set_hostname(sta, hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) return;

    esp_event_handler_instance_t any_id, got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, nullptr, &any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, nullptr, &got_ip);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_wifi_inited = true;
}

bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
    wifiEnsureInited(hostname);
    if (!s_wifi_inited) return false;

    xEventGroupClearBits(s_wifi_events, kWifiConnectedBit | kWifiFailedBit);
    s_retry_count = 0;
    s_connecting  = true;

    esp_wifi_disconnect();   // drop any prior association before re-associating

    wifi_config_t wifi_cfg = {};
    std::strncpy((char *)wifi_cfg.sta.ssid, netSSID, sizeof(wifi_cfg.sta.ssid) - 1);
    std::strncpy((char *)wifi_cfg.sta.password, netPassword, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        kWifiConnectedBit | kWifiFailedBit,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(20000));

    s_connecting = false;
    return (bits & kWifiConnectedBit) != 0;
}

std::vector<std::string> networkScanSSIDs()
{
    wifiEnsureInited(LOCAL_HOSTNAME);
    std::vector<std::string> out;
    if (!s_wifi_inited) return out;

    wifi_scan_config_t sc = {};   // active scan, all channels
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return out;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return out;
    if (n > 40) n = 40;

    std::vector<wifi_ap_record_t> recs(n);
    if (esp_wifi_scan_get_ap_records(&n, recs.data()) != ESP_OK) return out;

    // esp_wifi returns records sorted by RSSI (descending); de-dup by SSID.
    for (uint16_t i = 0; i < n; i++) {
        std::string ssid((const char *)recs[i].ssid);
        if (ssid.empty()) continue;
        if (std::find(out.begin(), out.end(), ssid) != out.end()) continue;
        out.push_back(std::move(ssid));
    }
    return out;
}

bool wifiIsConfigured()
{
    nvs.begin(NVS_NS, true);
    std::string ssid = nvs.getString(NVS_KEY_SSID, "");
    nvs.end();
    return !ssid.empty();
}

bool networkSaveAndConnect(const char *ssid, const char *pass)
{
    nvs.begin(NVS_NS, false);
    nvs.putString(NVS_KEY_SSID, ssid ? ssid : "");
    nvs.putString(NVS_KEY_PASS, pass ? pass : "");
    nvs.end();
    return initNetwork(ssid ? ssid : "", pass ? pass : "", LOCAL_HOSTNAME);
}

bool networkConnectSaved()
{
    nvs.begin(NVS_NS, true);
    std::string ssid = nvs.getString(NVS_KEY_SSID, "");
    std::string pass = nvs.getString(NVS_KEY_PASS, "");
    nvs.end();
    if (ssid.empty()) return false;
    return initNetwork(ssid.c_str(), pass.c_str(), LOCAL_HOSTNAME);
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

// Enable the mbedTLS certificate bundle for https:// URLs so call sites don't
// have to know which transport the base URL uses.
static void applyTlsIfHttps(esp_http_client_config_t &cfg, const char *url)
{
    if (strncmp(url, "https://", 8) == 0) {
        cfg.transport_type    = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
}

bool httpGetString(const char *url, std::string &out, int *status_out)
{
    HttpStringSink sink;
    esp_http_client_config_t cfg = {};
    cfg.url            = url;
    cfg.event_handler  = http_string_event;
    cfg.user_data      = &sink;
    cfg.timeout_ms     = 15000;
    applyTlsIfHttps(cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    authApplyHeader(client);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status_out) *status_out = err == ESP_OK ? status : 0;
    if (status == 401) authNotify401();
    if (err != ESP_OK || status / 100 != 2) {
        printf("httpGetString failed: err=%s status=%d\n", esp_err_to_name(err), status);
        return false;
    }
    out = std::move(sink.body);
    return true;
}

bool httpPostBytes(const char *url, const uint8_t *body, size_t len,
                   const char *content_type, std::string *resp_out, int *status_out)
{
    HttpStringSink sink;
    esp_http_client_config_t cfg = {};
    cfg.url           = url;
    cfg.method        = HTTP_METHOD_POST;
    cfg.event_handler = http_string_event;
    cfg.user_data     = &sink;
    cfg.timeout_ms    = 30000;
    applyTlsIfHttps(cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    authApplyHeader(client);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, (int)len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (resp_out) *resp_out = std::move(sink.body);
    if (status_out) *status_out = err == ESP_OK ? status : 0;
    if (status == 401) authNotify401();
    if (err != ESP_OK || status / 100 != 2) {
        printf("httpPostBytes failed: err=%s status=%d\n", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

// --- Public API ---------------------------------------------------------

std::vector<Friend> networkGetFriends()
{
    std::vector<Friend> friends;
    std::string payload;
    if (!httpGetString(FRIENDBOX_BASE_URL "/api/friends", payload)) {
        return friends;
    }

    JsonDocument jsonDoc;
    DeserializationError error = deserializeJson(jsonDoc, payload);
    if (error) {
        printf("Failed to parse JSON: %s\n", error.c_str());
        return friends;
    }
    if (!jsonDoc.is<JsonArray>()) {
        puts("Response is not a JSON array!");
        return friends;
    }
    for (JsonVariant v : jsonDoc.as<JsonArray>()) {
        Friend f;
        f.uuid     = (const char *)(v["uuid"] | "");
        f.username = (const char *)(v["username"] | "");
        f.accent   = (const char *)(v["accent"] | "");
        f.streak   = v["streak"] | 0;
        f.conv_id  = v["conv_id"] | (int64_t)0;
        if (!f.username.empty()) friends.push_back(std::move(f));
    }
    printf("Parsed %u friends\n", (unsigned)friends.size());
    return friends;
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

    // POST /api/post/sketch is multipart/form-data. The whole body (~116 KB)
    // is assembled in PSRAM: form fields first, then the nibble-packed canvas
    // as the `frames` file part, read back row-by-row from the panel.
    static const char *BOUNDARY = "----FriendBoxUpload7d4a1c";
    char part[192];
    std::string head;
    auto addField = [&](const char *name, const char *value) {
        snprintf(part, sizeof(part),
                 "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
                 BOUNDARY, name, value);
        head += part;
    };
    char accentStr[8];
    snprintf(accentStr, sizeof(accentStr), "%d", (int)currentDrawColorIndex);
    addField("frame_count", "1");
    addField("fps", "12");
    addField("accent_index", accentStr);
    addField("visibility", "public");
    snprintf(part, sizeof(part),
             "--%s\r\nContent-Disposition: form-data; name=\"frames\"; "
             "filename=\"frames.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n",
             BOUNDARY);
    head += part;
    std::string tail = "\r\n--" + std::string(BOUNDARY) + "--\r\n";

    size_t bodyLen = head.size() + framebufferSize + tail.size();
    uint8_t *body = (uint8_t *)heap_caps_malloc(bodyLen, MALLOC_CAP_SPIRAM);
    if (!body) {
        puts("networkSendCanvas: PSRAM alloc failed for upload body");
        return false;
    }
    memcpy(body, head.data(), head.size());
    uint8_t *fb = body + head.size();
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
    memcpy(fb + framebufferSize, tail.data(), tail.size());

    char contentType[80];
    snprintf(contentType, sizeof(contentType), "multipart/form-data; boundary=%s", BOUNDARY);
    std::string resp;
    bool ok = httpPostBytes(FRIENDBOX_BASE_URL "/api/post/sketch",
                            body, bodyLen, contentType, &resp);
    heap_caps_free(body);
    if (ok) {
        // The endpoint reports validation problems as 200 {"error": "..."}.
        JsonDocument doc;
        if (deserializeJson(doc, resp) == DeserializationError::Ok &&
            !doc["error"].isNull()) {
            printf("networkSendCanvas: server rejected: %s\n",
                   (const char *)doc["error"]);
            return false;
        }
        puts("Sketch uploaded successfully!");
        printf("%s\n", resp.c_str());
    }
    return ok;
}

bool networkDownloadToFile(const char *url, const char *dest_path)
{
    std::string full_url = url;
    if (url[0] == '/') full_url = std::string(FRIENDBOX_BASE_URL) + url;
    printf("networkDownloadToFile: GET %s\n", full_url.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url        = full_url.c_str();
    cfg.timeout_ms = 60000;
    applyTlsIfHttps(cfg, full_url.c_str());
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    authApplyHeader(client);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        printf("networkDownloadToFile: open failed: %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    int64_t contentLen = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status / 100 != 2) {
        printf("networkDownloadToFile: HTTP %d\n", status);
        if (status == 401) authNotify401();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    printf("networkDownloadToFile: %lld bytes expected\n", (long long)contentLen);

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        printf("networkDownloadToFile: cannot create %s\n", dest_path);
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
            puts("networkDownloadToFile: SD write failed (card full?)");
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
    printf("networkDownloadToFile: %s — %d / %lld bytes to %s\n",
           complete ? "OK" : "INCOMPLETE", total, (long long)contentLen, dest_path);

    if (!complete) {
        unlink(dest_path);
        puts("networkDownloadToFile: partial file deleted");
        return false;
    }
    return true;
}

bool networkDownloadFbox(const char *sketch_id, const char *dest_path)
{
    std::string url = FRIENDBOX_BASE_URL "/api/download/sketch/";
    url += sketch_id;
    return networkDownloadToFile(url.c_str(), dest_path);
}
