#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#define LOCAL_HOSTNAME "friendbox"
#define MAX_CONNECTION_ATTEMPTS 5
#define FRIENDBOX_SERVER "friendbox.chocolatedonut.dev"

// Functions
bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname);
void networkSendFramebuffer(int userID);
void networkReceiveFramebuffer();
bool networkSendCanvas();
std::vector<std::string> networkGetFriends();
/** Download an .fbox sketch from the server by ID and write it to dest_path on SD.
 *  Uses HTTPS (certificate not verified — personal server). */
bool networkDownloadFbox(const char *sketch_id, const char *dest_path);

#endif