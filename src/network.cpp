#include "network.hpp"
#include "display.hpp"
#include "canvas.hpp"

// Network
HTTPClient http;

bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
  WiFi.setHostname(hostname);
  WiFi.begin(netSSID, netPassword);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
  }
  // What if the network cannot ever connect? How do we handle?
  return true;
}

std::vector<std::string> networkGetFriends()
{
  std::vector<std::string> friendNames;
  http.begin("http://192.168.1.8:8000/get/friends");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.GET();

  if (httpCode == 200)
  {
    // Store payload.
    String payload = http.getString();
    http.end();
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("Successfully retrieved friends list!");
    Serial.print("Payload:");
    Serial.println(payload);
#endif

    // Init json object.
    JsonDocument jsonDoc;
    // Deserialize JSON payload into document.
    DeserializationError error = deserializeJson(jsonDoc, payload);
    if (error)
    {
      Serial.print("Failed to parse JSON: ");
      // throw exception?
      Serial.println(error.c_str());
      return friendNames;
    }

    // Check if it's an array
    if (jsonDoc.is<JsonArray>())
    {
      JsonArray array = jsonDoc.as<JsonArray>();

      // Iterate through array and add each name
      for (JsonVariant v : array)
      {
        if (v.is<const char *>())
        {
          friendNames.push_back(v.as<const char *>());
        }
      }

      Serial.printf("Parsed %d friends\n", friendNames.size());
    }
    else
    {
      Serial.println("Response is not a JSON array!");
      return friendNames; // Return empty array.
      // throw exception?
    }
  }
  else
  {
    Serial.printf("Error: %d\n", httpCode);
    Serial.printf("Error Payload: %s\n", http.errorToString(httpCode).c_str());
    return friendNames; // Return empty array.
  }
  return friendNames;
}

void networkReceiveFramebuffer()
{
  // To implement
}

bool networkSendCanvas()
{
  HTTPClient http;

  // Calculate size
  size_t framebufferSize = (TFT_HOR_RES * TFT_VER_RES) / 2; // 76,800 bytes

  http.begin("http://192.168.1.8:8000/sketches/upload");
  http.addHeader("Content-Type", "application/octet-stream");

  // Send raw framebuffer data
  int httpCode = http.POST(canvas_framebuffer, framebufferSize);

  if (httpCode == 200)
  {
    String response = http.getString();
    Serial.println("Sketch uploaded successfully!");
    Serial.println(response);
    http.end();
    return true;
  }
  else
  {
    Serial.printf("Upload failed: %d\n", httpCode);
    http.end();
    return false;
  }
}

void networkSendFramebuffer(int userID)
{
  JsonDocument jsonDoc;
  jsonDoc["userID"] = userID;
  jsonDoc["data"] = "FASTAPI SUCKS!";

  String jsonString;
  serializeJson(jsonDoc, jsonString);

  http.begin("http://192.168.1.8:8000/posttest");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(jsonString);

  if (httpCode == 200)
  {
    String payload = http.getString();

    Serial.println(payload);
  }
  else
  {
    Serial.printf("Error: %d\n", httpCode);
    Serial.printf("Error Payload: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}