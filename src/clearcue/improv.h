// Minimal Improv Wi-Fi Serial implementation (https://www.improv-wifi.com/serial/)
// Lets the ESP Web Tools flasher page push Wi-Fi credentials right after flashing
// and show a "Visit device" link to the settings page.
//
// Packet: "IMPROV" | ver(1)=1 | type(1) | len(1) | data[len] | checksum(1) = sum of all prior bytes
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include <vector>

namespace improv {

enum Type : uint8_t { CURRENT_STATE = 0x01, ERROR_STATE = 0x02, RPC = 0x03, RPC_RESULT = 0x04 };
enum State : uint8_t { STATE_READY = 0x02, STATE_PROVISIONING = 0x03, STATE_PROVISIONED = 0x04 };
enum Error : uint8_t { ERR_NONE = 0x00, ERR_INVALID_RPC = 0x01, ERR_UNKNOWN_RPC = 0x02,
                       ERR_UNABLE_TO_CONNECT = 0x03, ERR_UNKNOWN = 0xFF };
enum Cmd : uint8_t { CMD_WIFI_SETTINGS = 0x01, CMD_GET_STATE = 0x02, CMD_GET_INFO = 0x03,
                     CMD_GET_NETWORKS = 0x04 };

struct Callbacks {
  std::function<bool(const String& ssid, const String& pass)> connect;  // true on success
  std::function<bool()>   isConnected;
  std::function<String()> deviceUrl;                                    // "http://<ip>/"
  std::function<std::vector<String>()> info;                            // name, ver, chip, devname
};

class Serial_ {
 public:
  void begin(Stream& s, Callbacks cb) { io_ = &s; cb_ = cb; }

  void loop() {
    if (!io_) return;
    while (io_->available()) {
      uint8_t b = io_->read();
      if (!feed(b)) { buf_.clear(); if (b == 'I') buf_.push_back(b); }
    }
  }

 private:
  Stream* io_ = nullptr;
  Callbacks cb_;
  std::vector<uint8_t> buf_;

  // Returns false if the byte breaks the framing (caller resets).
  bool feed(uint8_t b) {
    static const char HDR[] = "IMPROV";
    size_t i = buf_.size();
    if (i < 6)  { if (b != (uint8_t)HDR[i]) return false; buf_.push_back(b); return true; }
    if (i == 6) { if (b != 1) return false; buf_.push_back(b); return true; }
    buf_.push_back(b);
    if (buf_.size() < 9) return true;
    size_t len = buf_[8];
    if (buf_.size() < 9 + len + 1) return true;      // need data + checksum
    uint8_t sum = 0;
    for (size_t k = 0; k < 9 + len; k++) sum += buf_[k];
    bool good = (sum == buf_.back());
    uint8_t type = buf_[7];
    std::vector<uint8_t> data(buf_.begin() + 9, buf_.begin() + 9 + len);
    buf_.clear();
    if (!good) { sendError(ERR_INVALID_RPC); return true; }
    if (type == RPC) handleRpc(data);
    return true;
  }

  void send(uint8_t type, const std::vector<uint8_t>& data) {
    uint8_t hdr[9] = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, (uint8_t)data.size()};
    uint8_t sum = 0;
    for (uint8_t c : hdr) sum += c;
    for (uint8_t c : data) sum += c;
    io_->write(hdr, sizeof(hdr));
    if (!data.empty()) io_->write(data.data(), data.size());
    io_->write(sum);
    io_->write('\n');
    io_->flush();
  }
  void sendState(State s)  { send(CURRENT_STATE, {(uint8_t)s}); }
  void sendError(Error e)  { send(ERROR_STATE, {(uint8_t)e}); }
  void sendResult(Cmd cmd, const std::vector<String>& strs) {
    std::vector<uint8_t> d{(uint8_t)cmd, 0};
    for (auto& s : strs) {
      d.push_back((uint8_t)s.length());
      for (size_t k = 0; k < s.length(); k++) d.push_back((uint8_t)s[k]);
    }
    d[1] = (uint8_t)(d.size() - 2);
    send(RPC_RESULT, d);
  }

  void handleRpc(const std::vector<uint8_t>& d) {
    if (d.size() < 2 || d[1] != d.size() - 2) { sendError(ERR_INVALID_RPC); return; }
    switch (d[0]) {
      case CMD_WIFI_SETTINGS: {
        if (d.size() < 3) { sendError(ERR_INVALID_RPC); return; }
        size_t p = 2;
        uint8_t sl = d[p++];
        if (p + sl >= d.size()) { sendError(ERR_INVALID_RPC); return; }
        String ssid; for (uint8_t k = 0; k < sl; k++) ssid += (char)d[p++];
        uint8_t pl = d[p++];
        if (p + pl > d.size()) { sendError(ERR_INVALID_RPC); return; }
        String pass; for (uint8_t k = 0; k < pl; k++) pass += (char)d[p++];
        sendError(ERR_NONE);
        sendState(STATE_PROVISIONING);
        if (cb_.connect && cb_.connect(ssid, pass)) {
          sendState(STATE_PROVISIONED);
          sendResult(CMD_WIFI_SETTINGS, {cb_.deviceUrl()});
        } else {
          sendError(ERR_UNABLE_TO_CONNECT);
          sendState(STATE_READY);
        }
        break;
      }
      case CMD_GET_STATE:
        if (cb_.isConnected && cb_.isConnected()) {
          sendState(STATE_PROVISIONED);
          sendResult(CMD_GET_STATE, {cb_.deviceUrl()});
        } else {
          sendState(STATE_READY);
        }
        break;
      case CMD_GET_INFO:
        sendResult(CMD_GET_INFO, cb_.info());
        break;
      case CMD_GET_NETWORKS: {
        int n = WiFi.scanNetworks();
        for (int k = 0; k < n; k++) {
          if (WiFi.SSID(k).isEmpty()) continue;
          sendResult(CMD_GET_NETWORKS, {WiFi.SSID(k), String(WiFi.RSSI(k)),
                                        WiFi.encryptionType(k) == WIFI_AUTH_OPEN ? "NO" : "YES"});
        }
        WiFi.scanDelete();
        sendResult(CMD_GET_NETWORKS, {});
        break;
      }
      default:
        sendError(ERR_UNKNOWN_RPC);
    }
  }
};

}  // namespace improv
