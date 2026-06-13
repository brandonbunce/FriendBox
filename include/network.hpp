#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <stdint.h>
#include <string>
#include <vector>
#include <ArduinoJson.h>

#define LOCAL_HOSTNAME "friendbox"
#define MAX_CONNECTION_ATTEMPTS 5

// Single base URL for every server call (HTTPS via the mbedTLS certificate
// bundle). Point this at a local uvicorn instance (e.g. http://192.168.1.8:8000)
// to test against a dev server — plain http URLs skip TLS automatically.
#define FRIENDBOX_BASE_URL "https://friendbox.chocolatedonut.dev"

/** A friend on the paired account, as returned by GET /api/friends. */
struct Friend
{
    std::string uuid;     // canonical user id (32 hex chars)
    std::string username;
    std::string accent;   // "#RRGGBB" accent color
    int         streak  = 0;
    int64_t     conv_id = 0;  // DM conversation id for future sends
};

// Functions
bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname);

/** Scan for nearby access points. Brings the WiFi driver up if needed (does not
 *  connect), returns de-duplicated SSID names sorted by signal strength. */
std::vector<std::string> networkScanSSIDs();
/** True when WiFi credentials are stored in NVS (the device has been provisioned). */
bool wifiIsConfigured();
/** Persist ssid/pass to NVS, then connect. Returns true once an IP is acquired. */
bool networkSaveAndConnect(const char *ssid, const char *pass);
/** Connect using the credentials saved in NVS. False if none are stored. */
bool networkConnectSaved();

bool networkSendCanvas();
std::vector<Friend> networkGetFriends();
/** Download an .fbox sketch from the server by ID and write it to dest_path on SD. */
bool networkDownloadFbox(const char *sketch_id, const char *dest_path);
/** Stream any server URL to a file on SD. url may be server-relative
 *  ("/api/…") or absolute. Deletes a partial file on failure. */
bool networkDownloadToFile(const char *url, const char *dest_path);

// ── HTTP helpers ───────────────────────────────────────────────────────────
// Both attach the device bearer token (auth.hpp) when paired, auto-enable the
// HTTPS certificate bundle for https:// URLs, report the HTTP status through
// status_out (0 if the request never completed), and raise the auth 401 flag
// on a rejected request.

/** GET url, collecting the response body into out. True on 2xx. */
bool httpGetString(const char *url, std::string &out, int *status_out = nullptr);

/** POST a byte body with the given Content-Type. True on 2xx. */
bool httpPostBytes(const char *url, const uint8_t *body, size_t len,
                   const char *content_type, std::string *resp_out = nullptr,
                   int *status_out = nullptr);

#endif
