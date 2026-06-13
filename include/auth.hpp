// Device authentication against FriendBox-Server.
//
// Pairing model (OAuth-device-flow shaped): the box registers itself with
// POST /api/device/register, shows the returned 6-char code on screen, and
// polls /api/device/poll until the owner claims the code in the web app. The
// server then hands over a long-lived opaque token ("fbd_" + 64 hex) which is
// persisted in NVS and attached as `Authorization: Bearer` to every API call.
//
// Stability rules:
//  - All pairing HTTP runs on a dedicated low-priority task on core 0 — never
//    on the UI task. The UI only reads the published state/code.
//  - Only a *genuine* auth rejection (HTTP 401, or /api/me answering
//    "user": null) ever wipes the token. Transport errors keep the device
//    paired so a flaky network can't sign the appliance out.
//  - A 401 seen mid-request anywhere sets an atomic flag (authNotify401);
//    the UI task consumes it between ticks and routes back to the pairing
//    screen. Because fbox playback blocks the UI task, the transition can
//    never tear down an active playback pipeline.
#ifndef AUTH_HPP
#define AUTH_HPP

#include <string>
#include <esp_http_client.h>

enum class AuthState {
    UNPAIRED,       // no token, pairing not running
    REGISTERING,    // requesting a pairing code from the server
    AWAITING_CLAIM, // code on screen, polling for the owner's claim
    PAIRED,         // token in NVS (claimed now or restored at boot)
    NET_ERROR,      // pairing task hit a transport error; retrying with backoff
};

enum class AuthCheck {
    VALID,     // server recognizes the token
    INVALID,   // server explicitly rejected it — unpair
    NET_ERROR, // could not reach the server — keep the token
};

/** Load token/username from NVS into RAM. Call once at boot after initNVS(). */
void authInit();

AuthState   authState();
/** The on-screen pairing code. Valid only while authState()==AWAITING_CLAIM. */
const char *authPairingCode();
bool        authHasToken();
/** Cached username of the paired account ("" when unpaired). */
const char *authUsername();

/** Attach `Authorization: Bearer <token>` to a not-yet-performed request.
 *  No-op when unpaired. Safe from any task. */
void authApplyHeader(esp_http_client_handle_t client);

/** Record that a request was rejected with HTTP 401. Sets an atomic flag
 *  only — never touches the UI or NVS directly. */
void authNotify401();
/** Consume the 401 flag. The UI task polls this once per tick and, when set,
 *  forgets the token and routes to the pairing screen. */
bool authConsume401();

/** Spawn the pairing task (register → show code → poll until claimed).
 *  No-op if pairing is already in progress. */
void authStartPairing();

/** Blocking GET /api/me with the stored token. Run from a boot/background
 *  task, not the UI tick. Note the server answers 200 {"user": null} for an
 *  unknown token on this route, so the body is checked, not just the status. */
AuthCheck authValidateToken();

/** Wipe the token from NVS + RAM and return to UNPAIRED. */
void authForgetToken();

#endif // AUTH_HPP
