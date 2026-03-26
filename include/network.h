#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#define LOCAL_HOSTNAME "friendbox"

// Functions
bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname);
void networkSendFramebuffer(int userID);
void networkReceiveFramebuffer();
bool networkSendCanvas();
std::vector<std::string> networkGetFriends();

#endif