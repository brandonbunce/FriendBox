#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#define LOCAL_HOSTNAME "friendbox"
#define MAX_CONNECTION_ATTEMPTS 5

// Functions
bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname);
void networkSendFramebuffer(int userID);
void networkReceiveFramebuffer();
bool networkSendCanvas();
std::vector<std::string> networkGetFriends();

#endif