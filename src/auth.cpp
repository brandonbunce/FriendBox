#include "auth.hpp"
#include "network.hpp"
#include "nvs_store.hpp"
#include "idf_compat.hpp"

#include <atomic>
#include <cstring>

#include <ArduinoJson.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char *NVS_NS        = "Friendbox";
static const char *NVS_KEY_TOKEN = "dev_token";
static const char *NVS_KEY_USER  = "srv_user";

// Token + username live in RAM behind a mutex: written by the pairing task
// and boot, read by every HTTP call site on whatever task it runs on.
static SemaphoreHandle_t s_lock = nullptr;
static std::string       s_token;
static std::string       s_username;

static std::atomic<int>  s_state{(int)AuthState::UNPAIRED};
static std::atomic<bool> s_got401{false};
static std::atomic<bool> s_pairingTaskRunning{false};

// Pairing code published for the UI. Written by the pairing task strictly
// before the state flips to AWAITING_CLAIM; the UI only reads it in that state.
static char s_code[8] = {0};

static void lock()   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_lock); }

/* restore the device token + username from NVS */
void authInit()
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    NvsStore nvs;
    nvs.begin(NVS_NS, true);
    lock();
    s_token    = nvs.getString(NVS_KEY_TOKEN, "");
    s_username = nvs.getString(NVS_KEY_USER, "");
    unlock();
    nvs.end();
    s_state.store((int)(s_token.empty() ? AuthState::UNPAIRED : AuthState::PAIRED));
    if (!s_token.empty()) {
        printf("[auth] token restored from NVS (%.8s…), user=%s\n",
               s_token.c_str(), s_username.c_str());
    }
}

AuthState   authState()       { return (AuthState)s_state.load(); }
const char *authPairingCode() { return s_code; }
const char *authUsername()    { return s_username.c_str(); }

bool authHasToken()
{
    lock();
    bool has = !s_token.empty();
    unlock();
    return has;
}

void authApplyHeader(esp_http_client_handle_t client)
{
    if (!client || !s_lock) return;
    lock();
    std::string bearer = s_token.empty() ? "" : ("Bearer " + s_token);
    unlock();
    if (!bearer.empty()) {
        esp_http_client_set_header(client, "Authorization", bearer.c_str());
    }
}

void authNotify401()  { s_got401.store(true); }
bool authConsume401() { return s_got401.exchange(false); }

void authForgetToken()
{
    lock();
    s_token.clear();
    s_username.clear();
    unlock();
    NvsStore nvs;
    nvs.begin(NVS_NS, false);
    nvs.putString(NVS_KEY_TOKEN, "");
    nvs.putString(NVS_KEY_USER, "");
    nvs.end();
    s_state.store((int)AuthState::UNPAIRED);
    puts("[auth] token forgotten");
}

static void authPersist(const std::string &token, const std::string &user)
{
    lock();
    s_token    = token;
    s_username = user;
    unlock();
    NvsStore nvs;
    nvs.begin(NVS_NS, false);
    nvs.putString(NVS_KEY_TOKEN, token.c_str());
    nvs.putString(NVS_KEY_USER, user.c_str());
    nvs.end();
}

AuthCheck authValidateToken()
{
    std::string body;
    int status = 0;
    bool ok = httpGetString(FRIENDBOX_BASE_URL "/api/me", body, &status);
    if (status == 401) return AuthCheck::INVALID;
    if (!ok) return AuthCheck::NET_ERROR;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return AuthCheck::NET_ERROR;
    // /api/me answers 200 with "user": null for an unknown/revoked token.
    if (doc["user"].isNull()) return AuthCheck::INVALID;
    lock();
    s_username = doc["user"].as<const char *>();
    unlock();
    return AuthCheck::VALID;
}

// ── Pairing task ────────────────────────────────────────────────────────────

static std::string hwIdHex()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[13];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

// POST a small JSON body and parse a JSON reply. Returns false on transport
// error; *status_out always carries the HTTP status (0 if none).
static bool postJson(const char *url, const JsonDocument &req, JsonDocument &resp,
                     int *status_out)
{
    std::string body;
    serializeJson(req, body);
    std::string respBody;
    bool ok = httpPostBytes(url, (const uint8_t *)body.data(), body.size(),
                            "application/json", &respBody, status_out);
    if (!ok) return false;
    return deserializeJson(resp, respBody) == DeserializationError::Ok;
}

static void pairingTask(void *)
{
    int backoff_ms = 5000;
    std::string poll_secret;
    int poll_interval_s = 3;

    while (true) {
        // ── Register: get a fresh pairing code ──
        s_state.store((int)AuthState::REGISTERING);
        {
            JsonDocument req, resp;
            req["hw_id"]      = hwIdHex();
            req["fw_version"] = "0.4";
            int status = 0;
            if (!postJson(FRIENDBOX_BASE_URL "/api/device/register", req, resp, &status) ||
                status / 100 != 2) {
                printf("[auth] register failed (status=%d), retry in %d ms\n",
                       status, backoff_ms);
                s_state.store((int)AuthState::NET_ERROR);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = backoff_ms < 30000 ? backoff_ms * 2 : 30000;
                continue;
            }
            backoff_ms = 5000;
            const char *code = resp["code"] | "";
            poll_secret      = (const char *)(resp["poll_secret"] | "");
            poll_interval_s  = resp["poll_interval"] | 3;
            strlcpy(s_code, code, sizeof(s_code));
            printf("[auth] pairing code: %s\n", s_code);
        }
        s_state.store((int)AuthState::AWAITING_CLAIM);

        // ── Poll until claimed, expired, or transport failure ──
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(poll_interval_s * 1000));
            JsonDocument req, resp;
            req["code"]        = (const char *)s_code;
            req["poll_secret"] = poll_secret;
            int status = 0;
            bool ok = postJson(FRIENDBOX_BASE_URL "/api/device/poll", req, resp, &status);
            if (status == 404) break;          // code expired — re-register
            if (!ok || status / 100 != 2) continue;  // transient — keep polling

            const char *st = resp["status"] | "";
            if (strcmp(st, "claimed") == 0) {
                const char *token = resp["token"] | "";
                const char *user  = resp["username"] | "";
                if (token[0] != '\0') {
                    authPersist(token, user);
                    s_state.store((int)AuthState::PAIRED);
                    printf("[auth] paired as %s (token %.8s…)\n", user, token);
                    s_pairingTaskRunning.store(false);
                    vTaskDelete(nullptr);
                }
                break;  // claimed but no token?! — re-register
            }
            // "pending" — keep polling.
        }
    }
}

void authStartPairing()
{
    if (s_pairingTaskRunning.exchange(true)) return;
    s_code[0] = '\0';
    // Core 0 — core 1 hosts the UI loop and the playback consumer.
    xTaskCreatePinnedToCore(pairingTask, "auth_pair", 6144, nullptr, 1, nullptr, 0);
}
