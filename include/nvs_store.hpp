// Minimal Arduino-Preferences-shaped wrapper around ESP-IDF's nvs_flash
// API. Just enough surface to cover the Friendbox call sites
// (begin/end + getUInt/putUInt). Add more accessors as needed.

#pragma once

#include <stdint.h>
#include <string>

#include <nvs.h>
#include <nvs_flash.h>

class NvsStore
{
public:
    bool begin(const char *ns, bool read_only)
    {
        end();
        esp_err_t err = nvs_open(ns, read_only ? NVS_READONLY : NVS_READWRITE, &_handle);
        if (err != ESP_OK) {
            _handle = 0;
            return false;
        }
        _open = true;
        return true;
    }

    void end()
    {
        if (_open) {
            nvs_close(_handle);
            _handle = 0;
            _open   = false;
        }
    }

    uint32_t getUInt(const char *key, uint32_t default_value = 0) const
    {
        if (!_open) return default_value;
        uint32_t v = default_value;
        nvs_get_u32(_handle, key, &v);
        return v;
    }

    bool putUInt(const char *key, uint32_t value)
    {
        if (!_open) return false;
        if (nvs_set_u32(_handle, key, value) != ESP_OK) return false;
        return nvs_commit(_handle) == ESP_OK;
    }

    bool putString(const char *key, const char *value)
    {
        if (!_open) return false;
        if (nvs_set_str(_handle, key, value) != ESP_OK) return false;
        return nvs_commit(_handle) == ESP_OK;
    }

    std::string getString(const char *key, const char *default_value = "") const
    {
        if (!_open) return std::string(default_value);
        size_t need = 0;
        if (nvs_get_str(_handle, key, nullptr, &need) != ESP_OK || need == 0) {
            return std::string(default_value);
        }
        std::string out(need, '\0');
        if (nvs_get_str(_handle, key, out.data(), &need) != ESP_OK) {
            return std::string(default_value);
        }
        if (!out.empty() && out.back() == '\0') out.pop_back();
        return out;
    }

private:
    nvs_handle_t _handle = 0;
    bool         _open   = false;
};

bool initNVS();
