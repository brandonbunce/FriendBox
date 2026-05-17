#include "network.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include <SD_MMC.h>

// Network
HTTPClient http;

bool initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
  int connectionAttempts = 0;
  WiFi.setHostname(hostname);
  WiFi.begin(netSSID, netPassword);
  while (WiFi.status() != WL_CONNECTED)
  {
    if (connectionAttempts > MAX_CONNECTION_ATTEMPTS)
    {
      return false;
    }
    else
    {
      connectionAttempts++;
      delay(500);
    }
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

// Map RGB565 -> 4-bit palette index, copy of paletteIndexForColor in io.cpp.
// Kept local so this file doesn't need to expose io.cpp internals.
static uint8_t paletteIndexForColorNet(uint16_t color)
{
  for (uint8_t i = 0; i < 16; i++)
  {
    if (draw_color_palette[i] == color)
      return i;
  }
  uint8_t best_idx = 0;
  uint32_t best_dist = UINT32_MAX;
  int r = (color >> 11) & 0x1F;
  int g = (color >> 5) & 0x3F;
  int b = color & 0x1F;
  for (uint8_t i = 0; i < 16; i++)
  {
    uint16_t pc = draw_color_palette[i];
    int dr = ((pc >> 11) & 0x1F) - r;
    int dg = ((pc >> 5) & 0x3F) - g;
    int db = (pc & 0x1F) - b;
    uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
    if (dist < best_dist)
    {
      best_dist = dist;
      best_idx = i;
    }
  }
  return best_idx;
}

bool networkSendCanvas()
{
  HTTPClient http;

  // 4-bit packed: 480*480/2 = 115,200 bytes. Same wire format as before;
  // the server side does not need changes.
  constexpr size_t framebufferSize = (TFT_HOR_RES * TFT_VER_RES) / 2;
  constexpr size_t bytesPerRow = TFT_HOR_RES / 2;

  uint8_t *fb = (uint8_t *)malloc(framebufferSize);
  if (!fb)
  {
    Serial.println("networkSendCanvas: malloc failed for upload buffer");
    return false;
  }

  // Read the canvas line-by-line from LT7680 SDRAM and pack into fb.
  uint16_t lineBuf[TFT_HOR_RES];
  for (int y = 0; y < TFT_VER_RES; y++)
  {
    tft.readRect(0, y, TFT_HOR_RES, 1, lineBuf);
    uint8_t *row = fb + (size_t)y * bytesPerRow;
    for (int x = 0; x < TFT_HOR_RES; x += 2)
    {
      uint8_t hi = paletteIndexForColorNet(lineBuf[x]) & 0x0F;
      uint8_t lo = paletteIndexForColorNet(lineBuf[x + 1]) & 0x0F;
      row[x >> 1] = (hi << 4) | lo;
    }
  }

  http.begin("http://192.168.1.8:8000/sketches/upload");
  http.addHeader("Content-Type", "application/octet-stream");

  int httpCode = http.POST(fb, framebufferSize);
  free(fb);

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

bool networkDownloadFbox(const char *sketch_id, const char *dest_path)
{
    WiFiClientSecure client;
    client.setInsecure(); // accept any cert — personal server

    HTTPClient http;
    String url = "https://" FRIENDBOX_SERVER "/api/download/sketch/";
    url += sketch_id;
    Serial.printf("networkDownloadFbox: GET %s\n", url.c_str());

    if (!http.begin(client, url)) {
        Serial.println("networkDownloadFbox: http.begin failed");
        return false;
    }
    http.setTimeout(60000); // 60 s — large files may be slow on first request

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("networkDownloadFbox: HTTP %d\n", httpCode);
        http.end();
        return false;
    }

    int contentLen = http.getSize(); // -1 if chunked/unknown
    Serial.printf("networkDownloadFbox: %d bytes expected\n", contentLen);

    File f = SD_MMC.open(dest_path, FILE_WRITE);
    if (!f) {
        Serial.printf("networkDownloadFbox: cannot create %s\n", dest_path);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[512];
    int total = 0;
    bool write_err = false;

    while (http.connected() || stream->available()) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            if ((int)f.write(buf, n) != n) {
                Serial.println("networkDownloadFbox: SD write failed (card full?)");
                write_err = true;
                break;
            }
            total += n;
            if (total % 10240 == 0)
                Serial.printf("  %d KB...\n", total / 1024);
        } else {
            delay(1);
        }
        yield(); // feed watchdog during long downloads
        if (contentLen > 0 && total >= contentLen) break;
    }

    f.close();
    http.end();

    bool complete = !write_err && (contentLen < 0 || total == contentLen);
    Serial.printf("networkDownloadFbox: %s — %d / %d bytes to %s\n",
                  complete ? "OK" : "INCOMPLETE", total, contentLen, dest_path);

    if (!complete) {
        SD_MMC.remove(dest_path);
        Serial.println("networkDownloadFbox: partial file deleted");
        return false;
    }
    return true;
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