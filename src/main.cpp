#include <Arduino.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/rmt.h>
#include "assets/icons/led_8884594_64_png.h"

namespace {
constexpr char kAccessPointSsid[] = "ESP32-Config";
constexpr char kAccessPointPassword[] = "configureme";
constexpr byte kDnsPort = 53;
constexpr unsigned long kStatusLogCapacity = 80;
constexpr unsigned long kWifiRetryIntervalMs = 15000;
constexpr unsigned long kWifiConnectGraceMs = 10000;
constexpr char kPreferencesNamespace[] = "device-config";
constexpr char kLedDefaultsVersionKey[] = "ledDefV";
constexpr uint8_t kLedDefaultsVersion = 3;
constexpr char kRedirectLocation[] = "http://192.168.4.1/config";
constexpr char kLedLogicIconUrl[] = "/assets/icons/led_8884594_64.png";
constexpr char kLedLogicFaviconUrl[] = "/assets/icons/led_8884594_64.png";
constexpr uint8_t kLedPin = 5;
constexpr uint8_t kLedMinCount = 1;
constexpr uint8_t kLedMaxCount = 12;
constexpr unsigned long kLedPreviewTimeoutMs = 30000;
constexpr unsigned long kLedAnimationFrameMs = 40;
constexpr uint8_t kMaxScriptSteps = 48;
constexpr size_t kMaxScriptJsonLen = 4096;

// ---- Script system ----
enum class ScriptOpType : uint8_t { kSet = 0, kWait = 1, kFade = 2, kAllOff = 3, kBrightness = 4 };

struct ScriptStep {
  ScriptOpType op;
  uint8_t led;           // 0-based
  uint8_t r1, g1, b1;
  uint8_t r2, g2, b2;
  uint8_t brightness;    // 0-255
  uint32_t durationMs;
};

struct LedConfig {
  bool enabled;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t brightness;
  bool colorChangeEnabled;
  uint8_t r2;
  uint8_t g2;
  uint8_t b2;
  uint16_t changeIntervalMs;  // 0-30000ms (0-30s)
  uint16_t startDelayMs;      // 0-30000ms (0-30s)
};

DNSServer dnsServer;
WebServer webServer(80);
Preferences preferences;
String configuredSsid;
String configuredPassword;
String statusLog[kStatusLogCapacity];
size_t statusLogCount = 0;
size_t statusLogHead = 0;
bool accessPointRunning = false;
bool wifiConnectInProgress = false;
bool wifiConnectedLogged = false;
unsigned long lastWifiAttemptMs = 0;
unsigned long wifiAttemptStartedMs = 0;
uint8_t ledCount = 4;
LedConfig ledConfig[kLedMaxCount];
uint8_t previewLedCount = 4;
LedConfig previewLedConfig[kLedMaxCount];
unsigned long ledAnimationStartMs[kLedMaxCount] = {};
unsigned long previewAnimationStartMs[kLedMaxCount] = {};
bool ledPreviewActive = false;
unsigned long lastLedPreviewMs = 0;
unsigned long lastLedRenderMs = 0;
bool otaRebootPending = false;
unsigned long otaRebootAtMs = 0;
String otaUploadResult = "";
String firmwareVersion = "1.000000.0000";
bool wsReady = false;
rmt_item32_t wsItems[kLedMaxCount * 24];

// Script runtime state
ScriptStep scriptSteps[kMaxScriptSteps];
uint8_t scriptStepCount = 0;
bool scriptLoop = false;
bool scriptRunning = false;
uint8_t scriptCurrentStep = 0;
unsigned long scriptStepStartMs = 0;
// Script output buffer (written by interpreter, read by ws2812Show)
uint8_t scriptR[kLedMaxCount] = {};
uint8_t scriptG[kLedMaxCount] = {};
uint8_t scriptB[kLedMaxCount] = {};
uint8_t scriptBr[kLedMaxCount] = {};
bool scriptEnabled[kLedMaxCount] = {};
String savedScriptJson = "";  // persisted JSON

void saveLedConfig();
bool parseHexColor(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b);
bool parseAndRunScript(const String& json);
bool validateScriptJson(const String& json);

void copyLedConfigArray(const LedConfig* source, LedConfig* target) {
  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    target[i] = source[i];
  }
}

void populateLedConfigFromRequest(LedConfig* targetConfig, uint8_t& targetCount) {
  int requested = webServer.arg("ledCount").toInt();
  if (requested < kLedMinCount) {
    requested = kLedMinCount;
  }
  if (requested > kLedMaxCount) {
    requested = kLedMaxCount;
  }
  targetCount = static_cast<uint8_t>(requested);

  copyLedConfigArray(ledConfig, targetConfig);

  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    const String base = "led" + String(i);
    if (i < targetCount) {
      targetConfig[i].enabled = webServer.hasArg(base + "_enabled");

      uint8_t r = targetConfig[i].r;
      uint8_t g = targetConfig[i].g;
      uint8_t b = targetConfig[i].b;
      if (webServer.hasArg(base + "_color")) {
        parseHexColor(webServer.arg(base + "_color"), r, g, b);
      }
      targetConfig[i].r = r;
      targetConfig[i].g = g;
      targetConfig[i].b = b;

      if (webServer.hasArg(base + "_brightness")) {
        const int pct = webServer.arg(base + "_brightness").toInt();
        targetConfig[i].brightness = static_cast<uint8_t>(constrain(pct * 255 / 100, 0, 255));
      }

      targetConfig[i].colorChangeEnabled = webServer.hasArg(base + "_colorchange");

      uint8_t r2 = targetConfig[i].r2;
      uint8_t g2 = targetConfig[i].g2;
      uint8_t b2 = targetConfig[i].b2;
      if (webServer.hasArg(base + "_color2")) {
        parseHexColor(webServer.arg(base + "_color2"), r2, g2, b2);
      }
      targetConfig[i].r2 = r2;
      targetConfig[i].g2 = g2;
      targetConfig[i].b2 = b2;

      if (webServer.hasArg(base + "_changeinterval")) {
        const float seconds = webServer.arg(base + "_changeinterval").toFloat();
        targetConfig[i].changeIntervalMs = static_cast<uint16_t>(constrain(seconds * 1000.0f, 0.0f, 30000.0f));
      }

      if (webServer.hasArg(base + "_startdelay")) {
        const float seconds = webServer.arg(base + "_startdelay").toFloat();
        targetConfig[i].startDelayMs = static_cast<uint16_t>(constrain(seconds * 1000.0f, 0.0f, 30000.0f));
      }
    } else {
      targetConfig[i].enabled = false;
      targetConfig[i].brightness = 0;
      targetConfig[i].colorChangeEnabled = false;
      targetConfig[i].startDelayMs = 0;
    }
  }
}

const LedConfig* activeLedConfig() {
  return ledPreviewActive ? previewLedConfig : ledConfig;
}

uint8_t activeLedCount() {
  return ledPreviewActive ? previewLedCount : ledCount;
}

unsigned long* activeAnimationStartMs() {
  return ledPreviewActive ? previewAnimationStartMs : ledAnimationStartMs;
}

void resetAnimationStartTimes(unsigned long* startTimes) {
  const unsigned long now = millis();
  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    startTimes[i] = now;
  }
}

bool hasAnimatedLeds(const LedConfig* config, uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (config[i].enabled && config[i].changeIntervalMs > 0) {
      return true;
    }
  }
  return false;
}

void resolveLedColor(const LedConfig& config,
                     uint32_t elapsedMs,
                     uint8_t& r,
                     uint8_t& g,
                     uint8_t& b,
                     uint8_t& brightness) {
  r = config.r;
  g = config.g;
  b = config.b;
  brightness = config.brightness;

  if (config.changeIntervalMs == 0) {
    return;
  }

  if (elapsedMs < config.startDelayMs) {
    return;
  }

  elapsedMs -= config.startDelayMs;

  const uint32_t halfCycleMs = config.changeIntervalMs;
  const uint32_t fullCycleMs = halfCycleMs * 2UL;
  const uint32_t phaseMs = elapsedMs % fullCycleMs;
  const uint32_t blendMs = phaseMs <= halfCycleMs ? phaseMs : fullCycleMs - phaseMs;
  const uint32_t blend255 = (blendMs * 255UL) / halfCycleMs;

  if (config.colorChangeEnabled) {
    r = static_cast<uint8_t>((static_cast<uint32_t>(config.r) * (255UL - blend255) + static_cast<uint32_t>(config.r2) * blend255) / 255UL);
    g = static_cast<uint8_t>((static_cast<uint32_t>(config.g) * (255UL - blend255) + static_cast<uint32_t>(config.g2) * blend255) / 255UL);
    b = static_cast<uint8_t>((static_cast<uint32_t>(config.b) * (255UL - blend255) + static_cast<uint32_t>(config.b2) * blend255) / 255UL);
  } else {
    // Puls-Modus: Helligkeit 0 → max → 0
    brightness = static_cast<uint8_t>((static_cast<uint32_t>(config.brightness) * blend255) / 255UL);
  }
}

uint8_t monthFromAbbrev(const String& month) {
  if (month == "Jan") return 1;
  if (month == "Feb") return 2;
  if (month == "Mar") return 3;
  if (month == "Apr") return 4;
  if (month == "May") return 5;
  if (month == "Jun") return 6;
  if (month == "Jul") return 7;
  if (month == "Aug") return 8;
  if (month == "Sep") return 9;
  if (month == "Oct") return 10;
  if (month == "Nov") return 11;
  if (month == "Dec") return 12;
  return 0;
}

String buildCompileVersion() {
  const String date = String(__DATE__);
  const String time = String(__TIME__);

  const uint8_t month = monthFromAbbrev(date.substring(0, 3));

  String dayString = date.substring(4, 6);
  dayString.trim();
  const uint8_t day = static_cast<uint8_t>(dayString.toInt());

  const String yearString = date.substring(7, 11);
  const uint8_t year = static_cast<uint8_t>(yearString.toInt() % 100);

  const uint8_t hour = static_cast<uint8_t>(time.substring(0, 2).toInt());
  const uint8_t minute = static_cast<uint8_t>(time.substring(3, 5).toInt());

  char buffer[16];
  snprintf(buffer, sizeof(buffer), "1.%02u%02u%02u.%02u%02u", year, month, day, hour, minute);
  return String(buffer);
}

void initWs2812Rmt() {
  rmt_config_t config = {};
  config.rmt_mode = RMT_MODE_TX;
  config.channel = RMT_CHANNEL_0;
  config.gpio_num = static_cast<gpio_num_t>(kLedPin);
  config.mem_block_num = 1;
  config.clk_div = 2;  // 40MHz => 25ns per tick
  config.tx_config.loop_en = false;
  config.tx_config.carrier_en = false;
  config.tx_config.idle_output_en = true;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

  if (rmt_config(&config) != ESP_OK) {
    return;
  }
  if (rmt_driver_install(config.channel, 0, 0) != ESP_OK) {
    return;
  }

  wsReady = true;
}

void ws2812Show() {
  if (!wsReady) {
    return;
  }

  constexpr uint16_t t0h = 14;
  constexpr uint16_t t0l = 32;
  constexpr uint16_t t1h = 28;
  constexpr uint16_t t1l = 24;

  uint16_t itemIndex = 0;
  for (uint8_t led = 0; led < kLedMaxCount; ++led) {
    uint8_t rr = 0;
    uint8_t gg = 0;
    uint8_t bb = 0;

    if (scriptRunning) {
      if (scriptEnabled[led]) {
        rr = (static_cast<uint16_t>(scriptR[led]) * scriptBr[led]) / 255;
        gg = (static_cast<uint16_t>(scriptG[led]) * scriptBr[led]) / 255;
        bb = (static_cast<uint16_t>(scriptB[led]) * scriptBr[led]) / 255;
      }
    } else {
      const LedConfig* currentConfig = activeLedConfig();
      const uint8_t currentLedCount = activeLedCount();
      unsigned long* animationStartTimes = activeAnimationStartMs();
      const unsigned long now = millis();

      if (led < currentLedCount && currentConfig[led].enabled) {
        uint8_t baseR = 0;
        uint8_t baseG = 0;
        uint8_t baseB = 0;
        uint8_t effectiveBrightness = 0;
        const uint32_t elapsedMs = static_cast<uint32_t>(now - animationStartTimes[led]);
        resolveLedColor(currentConfig[led], elapsedMs, baseR, baseG, baseB, effectiveBrightness);
        rr = (static_cast<uint16_t>(baseR) * effectiveBrightness) / 255;
        gg = (static_cast<uint16_t>(baseG) * effectiveBrightness) / 255;
        bb = (static_cast<uint16_t>(baseB) * effectiveBrightness) / 255;
      }
    }

    const uint8_t grb[3] = {gg, rr, bb};
    for (uint8_t c = 0; c < 3; ++c) {
      for (int8_t bit = 7; bit >= 0; --bit) {
        const bool one = (grb[c] >> bit) & 0x01;
        wsItems[itemIndex].level0 = 1;
        wsItems[itemIndex].duration0 = one ? t1h : t0h;
        wsItems[itemIndex].level1 = 0;
        wsItems[itemIndex].duration1 = one ? t1l : t0l;
        ++itemIndex;
      }
    }
  }

  rmt_write_items(RMT_CHANNEL_0, wsItems, itemIndex, true);
  rmt_wait_tx_done(RMT_CHANNEL_0, pdMS_TO_TICKS(20));
}

// ---- Script parser ----
// Extracts the string value of "key":"value" or "key":number from json
static String extractJsonStr(const String& json, const String& key) {
  const int ki = json.indexOf("\"" + key + "\"");
  if (ki < 0) return "";
  int ci = json.indexOf(':', ki);
  if (ci < 0) return "";
  ++ci;
  while (ci < (int)json.length() && json.charAt(ci) == ' ') ++ci;
  if (json.charAt(ci) == '"') {
    const int end = json.indexOf('"', ci + 1);
    if (end < 0) return "";
    return json.substring(ci + 1, end);
  }
  // number/bool
  int end = ci;
  while (end < (int)json.length() && json.charAt(end) != ',' &&
         json.charAt(end) != '}' && json.charAt(end) != ']') ++end;
  return json.substring(ci, end);
}

static void scriptAllOff() {
  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    scriptEnabled[i] = false;
    scriptR[i] = scriptG[i] = scriptB[i] = scriptBr[i] = 0;
  }
}

bool parseAndRunScript(const String& json) {
  // Parse "loop"
  const String loopVal = extractJsonStr(json, "loop");
  scriptLoop = (loopVal == "true" || loopVal == "1");

  // Find ops array
  const int opsStart = json.indexOf("[");
  if (opsStart < 0) return false;

  scriptStepCount = 0;
  int pos = opsStart + 1;
  const int jsonLen = json.length();

  while (pos < jsonLen && scriptStepCount < kMaxScriptSteps) {
    // Find next '{'
    const int opStart = json.indexOf('{', pos);
    if (opStart < 0) break;
    const int opEnd = json.indexOf('}', opStart);
    if (opEnd < 0) break;

    const String opJson = json.substring(opStart, opEnd + 1);
    const String opType = extractJsonStr(opJson, "op");

    ScriptStep step = {};
    bool valid = true;

    if (opType == "set") {
      step.op = ScriptOpType::kSet;
      step.led = static_cast<uint8_t>(constrain(extractJsonStr(opJson, "led").toInt(), 0, kLedMaxCount - 1));
      parseHexColor(extractJsonStr(opJson, "color"), step.r1, step.g1, step.b1);
      const int br = extractJsonStr(opJson, "br").toInt();
      step.brightness = static_cast<uint8_t>(constrain(br * 255 / 100, 0, 255));
    } else if (opType == "wait") {
      step.op = ScriptOpType::kWait;
      step.durationMs = static_cast<uint32_t>(extractJsonStr(opJson, "s").toFloat() * 1000.0f);
    } else if (opType == "brightness") {
      step.op = ScriptOpType::kBrightness;
      step.led = static_cast<uint8_t>(constrain(extractJsonStr(opJson, "led").toInt(), 0, kLedMaxCount - 1));
      const int br = extractJsonStr(opJson, "br").toInt();
      step.brightness = static_cast<uint8_t>(constrain(br * 255 / 100, 0, 255));
    } else if (opType == "fade") {
      step.op = ScriptOpType::kFade;
      step.led = static_cast<uint8_t>(constrain(extractJsonStr(opJson, "led").toInt(), 0, kLedMaxCount - 1));
      parseHexColor(extractJsonStr(opJson, "from"), step.r1, step.g1, step.b1);
      parseHexColor(extractJsonStr(opJson, "to"), step.r2, step.g2, step.b2);
      step.durationMs = static_cast<uint32_t>(extractJsonStr(opJson, "s").toFloat() * 1000.0f);
      const int br = extractJsonStr(opJson, "br").toInt();
      step.brightness = static_cast<uint8_t>(constrain(br * 255 / 100, 0, 255));
    } else if (opType == "all_off") {
      step.op = ScriptOpType::kAllOff;
    } else {
      valid = false;
    }

    if (valid) {
      scriptSteps[scriptStepCount++] = step;
    }
    pos = opEnd + 1;
  }

  if (scriptStepCount == 0) return false;

  scriptAllOff();
  scriptCurrentStep = 0;
  scriptStepStartMs = millis();
  scriptRunning = true;

  // Execute first SET/ALLOFF immediately
  const ScriptStep& first = scriptSteps[0];
  if (first.op == ScriptOpType::kSet) {
    scriptEnabled[first.led] = true;
    scriptR[first.led] = first.r1;
    scriptG[first.led] = first.g1;
    scriptB[first.led] = first.b1;
    scriptBr[first.led] = first.brightness;
    ws2812Show();
  } else if (first.op == ScriptOpType::kAllOff) {
    scriptAllOff();
    ws2812Show();
  }

  return true;
}

bool validateScriptJson(const String& json) {
  const int opsStart = json.indexOf("[");
  if (opsStart < 0) return false;

  uint8_t stepCount = 0;
  int pos = opsStart + 1;
  const int jsonLen = json.length();

  while (pos < jsonLen && stepCount < kMaxScriptSteps) {
    const int opStart = json.indexOf('{', pos);
    if (opStart < 0) break;
    const int opEnd = json.indexOf('}', opStart);
    if (opEnd < 0) break;

    const String opJson = json.substring(opStart, opEnd + 1);
    const String opType = extractJsonStr(opJson, "op");
    const bool valid = opType == "set" || opType == "wait" || opType == "brightness" ||
                       opType == "fade" || opType == "all_off";
    if (!valid) {
      return false;
    }

    ++stepCount;
    pos = opEnd + 1;
  }

  return stepCount > 0;
}

// Called from loop(): advance script state machine
void tickScript() {
  if (!scriptRunning || scriptStepCount == 0) return;

  const unsigned long now = millis();
  const ScriptStep& step = scriptSteps[scriptCurrentStep];
  const uint32_t elapsed = static_cast<uint32_t>(now - scriptStepStartMs);
  bool advance = false;

  switch (step.op) {
    case ScriptOpType::kSet:
      scriptEnabled[step.led] = true;
      scriptR[step.led] = step.r1;
      scriptG[step.led] = step.g1;
      scriptB[step.led] = step.b1;
      scriptBr[step.led] = step.brightness;
      ws2812Show();
      advance = true;
      break;

    case ScriptOpType::kAllOff:
      scriptAllOff();
      ws2812Show();
      advance = true;
      break;

    case ScriptOpType::kWait:
      if (elapsed >= step.durationMs) {
        advance = true;
      }
      break;

    case ScriptOpType::kBrightness:
      scriptEnabled[step.led] = true;
      scriptBr[step.led] = step.brightness;
      ws2812Show();
      advance = true;
      break;

    case ScriptOpType::kFade:
      if (step.durationMs == 0 || elapsed >= step.durationMs) {
        scriptEnabled[step.led] = true;
        scriptR[step.led] = step.r2;
        scriptG[step.led] = step.g2;
        scriptB[step.led] = step.b2;
        scriptBr[step.led] = step.brightness;
        ws2812Show();
        advance = true;
      } else {
        const uint32_t t256 = (elapsed * 256UL) / step.durationMs;
        scriptEnabled[step.led] = true;
        scriptR[step.led] = static_cast<uint8_t>((step.r1 * (256 - t256) + step.r2 * t256) >> 8);
        scriptG[step.led] = static_cast<uint8_t>((step.g1 * (256 - t256) + step.g2 * t256) >> 8);
        scriptB[step.led] = static_cast<uint8_t>((step.b1 * (256 - t256) + step.b2 * t256) >> 8);
        scriptBr[step.led] = step.brightness;
        ws2812Show();
      }
      break;
  }

  if (advance) {
    ++scriptCurrentStep;
    scriptStepStartMs = now;
    if (scriptCurrentStep >= scriptStepCount) {
      if (scriptLoop) {
        scriptCurrentStep = 0;
      } else {
        scriptRunning = false;
      }
    }
  }
}

void appendLog(const String& message);

String htmlEscape(const String& input) {
  String escaped;
  escaped.reserve(input.length());

  for (size_t i = 0; i < input.length(); ++i) {
    const char ch = input.charAt(i);
    switch (ch) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&#39;";
        break;
      default:
        escaped += ch;
        break;
    }
  }

  return escaped;
}

String jsonEscape(const String& input) {
  String escaped;
  escaped.reserve(input.length() + 8);

  for (size_t i = 0; i < input.length(); ++i) {
    const char ch = input.charAt(i);
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }

  return escaped;
}

String wifiStatusToText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "ssid_not_found";
    case WL_SCAN_COMPLETED:
      return "scan_completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect_failed";
    case WL_CONNECTION_LOST:
      return "connection_lost";
    case WL_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

String wifiDisconnectReasonToText(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
      return "unspecified";
    case WIFI_REASON_AUTH_EXPIRE:
      return "auth_expire";
    case WIFI_REASON_AUTH_FAIL:
      return "auth_fail";
    case WIFI_REASON_ASSOC_EXPIRE:
      return "assoc_expire";
    case WIFI_REASON_ASSOC_FAIL:
      return "assoc_fail";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return "4way_handshake_timeout";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "handshake_timeout";
    case WIFI_REASON_NO_AP_FOUND:
      return "no_ap_found";
    case WIFI_REASON_ASSOC_LEAVE:
      return "assoc_leave";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "beacon_timeout";
    case WIFI_REASON_CONNECTION_FAIL:
      return "connection_fail";
    default:
      return "reason_" + String(reason);
  }
}

String operationModeText() {
  const wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED && accessPointRunning) {
    return "sta+ap";
  }
  if (status == WL_CONNECTED) {
    return "sta";
  }
  if (accessPointRunning && !configuredSsid.isEmpty()) {
    return "fallback_ap";
  }
  if (accessPointRunning) {
    return "config_ap";
  }

  return "idle";
}

void appendLog(const String& message) {
  const String line = String("[") + millis() + " ms] " + message;
  Serial.println(line);
  statusLog[statusLogHead] = line;
  statusLogHead = (statusLogHead + 1) % kStatusLogCapacity;
  if (statusLogCount < kStatusLogCapacity) {
    ++statusLogCount;
  }
}

String collectLogsAsHtml() {
  String html;
  html.reserve(statusLogCount * 90);

  for (size_t i = 0; i < statusLogCount; ++i) {
    const size_t index = (statusLogHead + kStatusLogCapacity - statusLogCount + i) % kStatusLogCapacity;
    html += "<div>";
    html += htmlEscape(statusLog[index]);
    html += "</div>";
  }

  return html;
}

String collectLogsAsJson() {
  String json = "[";

  for (size_t i = 0; i < statusLogCount; ++i) {
    const size_t index = (statusLogHead + kStatusLogCapacity - statusLogCount + i) % kStatusLogCapacity;
    if (i > 0) {
      json += ',';
    }

    json += '"';
    json += jsonEscape(statusLog[index]);
    json += '"';
  }

  json += "]";
  return json;
}

String collectWifiNetworksAsJson() {
  const int networkCount = WiFi.scanNetworks();
  String json = "[";

  if (networkCount < 0) {
    appendLog("WLAN-Scan fehlgeschlagen.");
    return json + "]";
  }

  appendLog("WLAN-Scan abgeschlossen: " + String(networkCount) + " Netzwerke gefunden.");

  for (int index = 0; index < networkCount; ++index) {
    if (index > 0) {
      json += ',';
    }

    json += "{";
    json += "\"ssid\":\"" + jsonEscape(WiFi.SSID(index)) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(index)) + ",";
    json += "\"secure\":";
    json += WiFi.encryptionType(index) != WIFI_AUTH_OPEN ? "true" : "false";
    json += "}";
  }

  WiFi.scanDelete();
  json += "]";
  return json;
}

String colorToHex(uint8_t r, uint8_t g, uint8_t b) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", r, g, b);
  return String(buffer);
}

bool parseHexColor(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (hex.length() != 7 || hex.charAt(0) != '#') {
    return false;
  }

  const long rgb = strtol(hex.substring(1).c_str(), nullptr, 16);
  if (rgb < 0) {
    return false;
  }

  r = (rgb >> 16) & 0xFF;
  g = (rgb >> 8) & 0xFF;
  b = rgb & 0xFF;
  return true;
}

String buildColorWheelControl(const String& fieldName, const String& label, const String& value) {
  String html;
  html.reserve(360);
  html += "<div class=\"field color-field\">";
  html += "<label>" + label + "</label>";
  html += "<div class=\"color-wheel-control\" data-field=\"" + fieldName + "\">";
  html += "<div class=\"color-wheel-stage\">";
  html += "<canvas class=\"color-wheel\" width=\"156\" height=\"156\"></canvas>";
  html += "<div class=\"color-wheel-marker\"></div>";
  html += "</div>";
  html += "<input type=\"hidden\" name=\"" + fieldName + "\" class=\"color-wheel-input\" value=\"" + value + "\">";
  html += "<div class=\"color-readout\">";
  html += "<span class=\"color-swatch\" style=\"background:" + value + ";\"></span>";
  html += "<span class=\"color-hex\">" + value + "</span>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  return html;
}

void applyLedState() {
  ws2812Show();
}

void loadLedConfig() {
  preferences.begin(kPreferencesNamespace, true);
  ledCount = preferences.getUChar("ledCount", 4);
  const uint8_t storedDefaultsVersion = preferences.getUChar(kLedDefaultsVersionKey, 0);
  preferences.end();

  if (ledCount < kLedMinCount || ledCount > kLedMaxCount) {
    ledCount = 4;
  }

  preferences.begin(kPreferencesNamespace, true);
  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    const String prefix = "led" + String(i) + "_";
    ledConfig[i].enabled = preferences.getBool((prefix + "en").c_str(), false);
    ledConfig[i].r = preferences.getUChar((prefix + "r").c_str(), 255);
    ledConfig[i].g = preferences.getUChar((prefix + "g").c_str(), 180);
    ledConfig[i].b = preferences.getUChar((prefix + "b").c_str(), 80);
    ledConfig[i].brightness = preferences.getUChar((prefix + "br").c_str(), 0);
    ledConfig[i].colorChangeEnabled = preferences.getBool((prefix + "cc").c_str(), false);
    ledConfig[i].r2 = preferences.getUChar((prefix + "r2").c_str(), 80);
    ledConfig[i].g2 = preferences.getUChar((prefix + "g2").c_str(), 255);
    ledConfig[i].b2 = preferences.getUChar((prefix + "b2").c_str(), 180);
    ledConfig[i].changeIntervalMs = preferences.getUShort((prefix + "ci").c_str(), 1000);
    ledConfig[i].startDelayMs = preferences.getUShort((prefix + "sd").c_str(), 0);
  }
  preferences.end();

  // Migration: Fuer bestehende Installationen einmalig auf "alle LEDs aus" setzen.
  if (storedDefaultsVersion < kLedDefaultsVersion) {
    for (uint8_t i = 0; i < kLedMaxCount; ++i) {
      ledConfig[i].enabled = false;
      ledConfig[i].brightness = 0;
      ledConfig[i].startDelayMs = 0;
    }
    saveLedConfig();

    preferences.begin(kPreferencesNamespace, false);
    preferences.putUChar(kLedDefaultsVersionKey, kLedDefaultsVersion);
    preferences.end();

    appendLog("LED-Defaults wurden auf AUS migriert.");
  }

  // Load script
  preferences.begin(kPreferencesNamespace, true);
  savedScriptJson = preferences.getString("script", "");
  preferences.end();
  if (!savedScriptJson.isEmpty()) {
    parseAndRunScript(savedScriptJson);
  }
}

void saveLedConfig() {
  preferences.begin(kPreferencesNamespace, false);
  preferences.putUChar("ledCount", ledCount);
  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    const String prefix = "led" + String(i) + "_";
    preferences.putBool((prefix + "en").c_str(), ledConfig[i].enabled);
    preferences.putUChar((prefix + "r").c_str(), ledConfig[i].r);
    preferences.putUChar((prefix + "g").c_str(), ledConfig[i].g);
    preferences.putUChar((prefix + "b").c_str(), ledConfig[i].b);
    preferences.putUChar((prefix + "br").c_str(), ledConfig[i].brightness);
    preferences.putBool((prefix + "cc").c_str(), ledConfig[i].colorChangeEnabled);
    preferences.putUChar((prefix + "r2").c_str(), ledConfig[i].r2);
    preferences.putUChar((prefix + "g2").c_str(), ledConfig[i].g2);
    preferences.putUChar((prefix + "b2").c_str(), ledConfig[i].b2);
    preferences.putUShort((prefix + "ci").c_str(), ledConfig[i].changeIntervalMs);
    preferences.putUShort((prefix + "sd").c_str(), ledConfig[i].startDelayMs);
  }
  preferences.end();
}

void loadCredentials() {
  preferences.begin(kPreferencesNamespace, true);
  configuredSsid = preferences.getString("ssid", "");
  configuredPassword = preferences.getString("password", "");
  preferences.end();

  if (configuredSsid.isEmpty()) {
    appendLog("Keine gespeicherte SSID gefunden.");
  } else {
    appendLog("Gespeicherte SSID geladen: " + configuredSsid);
  }
}

void saveCredentials(const String& ssid, const String& password) {
  preferences.begin(kPreferencesNamespace, false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();

  configuredSsid = ssid;
  configuredPassword = password;
  appendLog("Neue WLAN-Konfiguration gespeichert: " + ssid);
}

void clearCredentials() {
  preferences.begin(kPreferencesNamespace, false);
  preferences.remove("ssid");
  preferences.remove("password");
  preferences.end();

  configuredSsid = "";
  configuredPassword = "";
  wifiConnectInProgress = false;
  appendLog("Gespeicherte WLAN-Konfiguration wurde geloescht.");
}

void startAccessPoint() {
  if (accessPointRunning) {
    return;
  }

  WiFi.softAP(kAccessPointSsid, kAccessPointPassword);
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  accessPointRunning = true;
  appendLog("Captive Portal aktiv auf " + WiFi.softAPIP().toString() + " mit SSID " + kAccessPointSsid);
}

void stopAccessPoint() {
  if (!accessPointRunning) {
    return;
  }

  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  accessPointRunning = false;
  appendLog("Captive Portal deaktiviert, da WLAN verbunden ist.");
}

void beginWifiConnection() {
  if (configuredSsid.isEmpty()) {
    return;
  }

  WiFi.disconnect(false, true);
  delay(100);
  WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
  lastWifiAttemptMs = millis();
  wifiAttemptStartedMs = millis();
  wifiConnectInProgress = true;
  appendLog("Starte WLAN-Verbindungsversuch zu " + configuredSsid);
}

void handleWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      appendLog("WiFi-Event: STA gestartet.");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      appendLog("WiFi-Event: Mit Access Point assoziiert: " +
                String(reinterpret_cast<const char*>(info.wifi_sta_connected.ssid)));
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      appendLog("WiFi-Event: IP erhalten: " + WiFi.localIP().toString());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      appendLog("WiFi-Event: STA getrennt, reason=" + String(info.wifi_sta_disconnected.reason) +
                " (" + wifiDisconnectReasonToText(info.wifi_sta_disconnected.reason) + ")");
      wifiConnectInProgress = false;
      break;
    default:
      break;
  }
}

String extractJsonStringField(const String& json, const String& key) {
  const String marker = "\"" + key + "\"";
  const int keyPos = json.indexOf(marker);
  if (keyPos < 0) {
    return "";
  }

  const int colon = json.indexOf(':', keyPos + marker.length());
  if (colon < 0) {
    return "";
  }

  int start = json.indexOf('"', colon + 1);
  if (start < 0) {
    return "";
  }
  ++start;

  int end = start;
  while (end < static_cast<int>(json.length())) {
    if (json.charAt(end) == '"' && json.charAt(end - 1) != '\\') {
      break;
    }
    ++end;
  }

  if (end >= static_cast<int>(json.length())) {
    return "";
  }

  return json.substring(start, end);
}

bool scheduleOtaFromUrl(const String& url, String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    message = "OTA von URL nur mit aktiver WLAN-Verbindung moeglich.";
    return false;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) {
    message = "HTTP begin fehlgeschlagen.";
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    message = "HTTP Fehler beim Download: " + String(code);
    http.end();
    return false;
  }

  const int length = http.getSize();
  WiFiClient* stream = http.getStreamPtr();

  if (!Update.begin(length > 0 ? length : UPDATE_SIZE_UNKNOWN)) {
    message = "Update.begin fehlgeschlagen: " + String(Update.errorString());
    http.end();
    return false;
  }

  const size_t written = Update.writeStream(*stream);
  if (length > 0 && written != static_cast<size_t>(length)) {
    message = "Unvollstaendiger Download: " + String(written) + " von " + String(length);
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end()) {
    message = "Update.end fehlgeschlagen: " + String(Update.errorString());
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    message = "Update nicht vollstaendig.";
    http.end();
    return false;
  }

  http.end();
  message = "OTA erfolgreich, Neustart wird ausgefuehrt.";
  return true;
}

String buildLedCardsHtml() {
  String html;
  html.reserve(ledCount * 320);

  for (uint8_t i = 0; i < ledCount; ++i) {
    html += "<div class=\"led-card\">";
    html += "<h3>LED " + String(i + 1) + "</h3>";
    html += "<label class=\"led-flag\"><input type=\"checkbox\" name=\"led" + String(i) + "_enabled\"";
    if (ledConfig[i].enabled) {
      html += " checked";
    }
    html += "> Aktiv</label>";

    html += "<div class=\"led-fields\">";

    html += buildColorWheelControl("led" + String(i) + "_color", "Farbe",
                     colorToHex(ledConfig[i].r, ledConfig[i].g, ledConfig[i].b));

    html += "<div class=\"field\">";
    html += "<label>Helligkeit</label>";
    html += "<div class=\"range-row\">";
    html += "<input type=\"range\" min=\"0\" max=\"100\" name=\"led" + String(i) + "_brightness\" value=\"";
    html += String((ledConfig[i].brightness * 100 + 127) / 255);
    html += "\">";
    html += "<span id=\"br" + String(i) + "_val\" class=\"range-value\">0%</span>";
    html += "</div>";
    html += "</div>";

    html += "<div class=\"field\">";
    html += "<label>Wechselzeit</label>";
    html += "<div class=\"range-row\">";
    html += "<input type=\"number\" min=\"0\" max=\"30\" step=\"0.5\" style=\"width:80px;\" name=\"led" + String(i) + "_changeinterval\" value=\"";
    html += String(ledConfig[i].changeIntervalMs / 1000.0f, 1);
    html += "\">";
    html += "<span style=\"font-weight:700;\">s</span>";
    html += "</div>";
    html += "</div>";

    html += "<div class=\"field\">";
    html += "<label>Delay (nur erster Start)</label>";
    html += "<div class=\"range-row\">";
    html += "<input type=\"number\" min=\"0\" max=\"30\" step=\"0.5\" style=\"width:80px;\" name=\"led" + String(i) + "_startdelay\" value=\"";
    html += String(ledConfig[i].startDelayMs / 1000.0f, 1);
    html += "\">";
    html += "<span style=\"font-weight:700;\">s</span>";
    html += "</div>";
    html += "</div>";

    html += "<div class=\"field cc-panel\">";
    html += "<label class=\"led-flag\"><input type=\"checkbox\" name=\"led" + String(i) + "_colorchange\" class=\"cc-toggle\" data-led=\"" + String(i) + "\"";
    if (ledConfig[i].colorChangeEnabled) {
      html += " checked";
    }
    html += "> 2. Farbe</label>";

    html += "<div id=\"cc" + String(i) + "\" class=\"cc-options\" style=\"display:" + (ledConfig[i].colorChangeEnabled ? "block" : "none") + ";\">";
    html += buildColorWheelControl("led" + String(i) + "_color2", "2. Farbe",
                     colorToHex(ledConfig[i].r2, ledConfig[i].g2, ledConfig[i].b2));
    html += "</div>";
    html += "</div>";
    html += "</div>";

    html += "</div>";
  }

  return html;
}

String buildLogicPage() {
  String page;
  page.reserve(28000);
  page += R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LED-Logic</title>
  <link rel="icon" type="image/png" href=")HTML";
  page += kLedLogicFaviconUrl;
  page += R"HTML(">
  <style>
    :root { --bg:#f6f8fb; --panel:#ffffff; --line:#d8deea; --text:#142033; --accent:#0c7a5b; --muted:#5f6f84; }
    body { margin:0; font-family:"Avenir Next","Segoe UI",sans-serif; background:linear-gradient(140deg,#edf2f8,#f7efe7); color:var(--text); padding:18px; }
    .wrap { margin:0 auto; display:grid; gap:16px; }
    .panel { background:var(--panel); border:1px solid var(--line); border-radius:20px; padding:20px; }
    h1,h2,h3 { margin-top:0; }
    .topbar { display:flex; align-items:center; gap:12px; flex-wrap:wrap; margin-bottom:4px; }
    .title-row { display:flex; align-items:center; gap:8px; }
    .title-icon { width:42px; height:42px; flex:0 0 auto; overflow:visible; }
    .title { margin:0; font-size:2rem; line-height:1.15; }
    .title-version { font-size:0.95rem; font-weight:400; color:var(--muted); }
    .menu { position:relative; }
    .menu summary { list-style:none; }
    .menu summary::-webkit-details-marker { display:none; }
    .menu-button { border:1px solid var(--line); border-radius:12px; padding:10px 14px; background:#ffffff; font-weight:700; cursor:pointer; color:var(--text); }
    .menu-icon { font-size:1.2rem; line-height:1; }
    .menu[open] .menu-button { border-color:#9cb3d9; box-shadow:0 4px 14px rgba(20,32,51,0.12); }
    .menu-list { position:absolute; top:46px; left:0; min-width:180px; display:grid; gap:2px; background:#fff; border:1px solid var(--line); border-radius:12px; padding:6px; box-shadow:0 10px 24px rgba(20,32,51,0.14); z-index:20; }
    .menu-list a { display:block; padding:10px 12px; border-radius:8px; text-decoration:none; font-weight:700; color:var(--text); }
    .menu-list a:hover { background:#eef3fa; }
    button { border:0; border-radius:12px; padding:10px 16px; font:inherit; font-weight:700; cursor:pointer; background:linear-gradient(135deg,var(--accent),#18a57a); color:#fff; }
    button.danger { background:linear-gradient(135deg,#c0392b,#e74c3c); }
    button.secondary { background:#f0f4fa; color:var(--text); border:1px solid var(--line); }
    .row { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
    label { display:block; font-size:0.88rem; color:var(--muted); margin:0 0 6px; }
    select, input[type="number"], input[type="text"] { border:1px solid var(--line); border-radius:8px; padding:7px 10px; font:inherit; font-size:0.9rem; background:#fff; color:var(--text); }
    .command-bar { display:flex; align-items:center; justify-content:space-between; gap:14px; flex-wrap:wrap; padding:12px 14px; border:1px solid rgba(141,160,188,0.34); border-radius:18px; background:linear-gradient(180deg,rgba(255,255,255,0.88),rgba(239,244,251,0.92)); box-shadow:0 10px 30px rgba(20,32,51,0.08), inset 0 1px 0 rgba(255,255,255,0.9); backdrop-filter:blur(10px); }
    .command-group { display:flex; align-items:stretch; gap:10px; flex-wrap:wrap; }
    .toolbar-btn { width:84px; height:74px; padding:5px 8px 6px; border:1px solid rgba(149,169,200,0.42); border-radius:16px; color:var(--text); display:grid; justify-items:center; align-content:center; gap:3px; box-sizing:border-box; box-shadow:0 1px 0 rgba(255,255,255,0.92) inset, 0 8px 18px rgba(20,32,51,0.06); transition:transform 140ms ease, box-shadow 140ms ease, border-color 140ms ease, background 140ms ease, color 140ms ease; }
    .toolbar-btn:hover { transform:translateY(-1px); border-color:rgba(92,122,170,0.52); box-shadow:0 12px 24px rgba(20,32,51,0.09); }
    .toolbar-btn.toggle-active:hover,
    .toolbar-btn.save.toggle-active:hover,
    .toolbar-btn.start.toggle-active:hover,
    .toolbar-btn.stop.toggle-active:hover,
    .toolbar-btn.clear.toggle-active:hover { transform:none; }
    .toolbar-btn:active { transform:translateY(1px); }
    .toolbar-btn.momentary:active { background:linear-gradient(180deg,#eef1f4,#dde2e8); border-color:rgba(124,133,144,0.66); color:#505863; box-shadow:inset 0 2px 8px rgba(20,32,51,0.12); }
    .toolbar-btn.toggle-active,
    .toolbar-btn.save.toggle-active,
    .toolbar-btn.start.toggle-active,
    .toolbar-btn.stop.toggle-active,
    .toolbar-btn.clear.toggle-active { background:linear-gradient(180deg,#eef1f4,#dde2e8); border-color:rgba(124,133,144,0.66); color:#505863; box-shadow:inset 0 1px 0 rgba(255,255,255,0.96), 0 10px 22px rgba(20,32,51,0.1); }
    .toolbar-btn.save { background:linear-gradient(180deg,#eef5ff,#dce8ff); color:#2f67c8; }
    .toolbar-btn.start { background:linear-gradient(180deg,#e9faef,#d4f1df); color:#17845f; }
    .toolbar-btn.stop { background:linear-gradient(180deg,#fff1f1,#ffdede); color:#ba3f45; }
    .toolbar-btn.clear { background:linear-gradient(180deg,#fff6ee,#ffe7d2); color:#b06724; }
    .toolbar-btn.toggle-active .toolbar-icon-wrap,
    .toolbar-btn.save.toggle-active .toolbar-icon-wrap,
    .toolbar-btn.start.toggle-active .toolbar-icon-wrap,
    .toolbar-btn.stop.toggle-active .toolbar-icon-wrap,
    .toolbar-btn.clear.toggle-active .toolbar-icon-wrap,
    .toolbar-btn.momentary:active .toolbar-icon-wrap { background:transparent; color:#505863; box-shadow:none; }
    .toolbar-btn.save .toolbar-icon-wrap { background:transparent; color:#2f67c8; }
    .toolbar-btn.start .toolbar-icon-wrap { background:transparent; color:#17845f; }
    .toolbar-btn.stop .toolbar-icon-wrap { background:transparent; color:#ba3f45; }
    .toolbar-btn.clear .toolbar-icon-wrap { background:transparent; color:#b06724; }
    .toolbar-icon-wrap { order:-1; width:42px; height:42px; display:grid; place-items:center; border-radius:12px; border:none; box-shadow:none; }
    .toolbar-icon-svg { width:38px; height:38px; fill:none; stroke:currentColor; stroke-width:2.1; stroke-linecap:round; stroke-linejoin:round; }
    .toolbar-text { font-size:0.68rem; font-weight:800; line-height:1; letter-spacing:0.01em; text-transform:uppercase; }
    .toolbar-separator { width:1px; background:linear-gradient(180deg,transparent,rgba(149,169,200,0.6),transparent); margin:4px 2px; }
    .led-sim-panel { display:flex; align-items:center; gap:10px; padding:8px 12px; border-radius:16px; background:rgba(255,255,255,0.74); border:1px solid rgba(149,169,200,0.28); min-height:60px; }
    .led-sim-label { font-size:0.74rem; font-weight:700; color:var(--muted); white-space:nowrap; }
    .led-sim-list { display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
    .led-sim-dot { width:20px; height:20px; border-radius:50%; background:#d9e1ee; border:1px solid rgba(20,32,51,0.18); box-shadow:inset 0 1px 2px rgba(255,255,255,0.95); opacity:0.45; transition:background 120ms linear, box-shadow 120ms linear, opacity 120ms linear, transform 120ms linear; }
    .led-sim-dot.active { opacity:1; transform:scale(1.04); }
    .logic-layout { display:grid; grid-template-columns:260px minmax(0,1fr); gap:14px; margin-top:12px; }
    .toolbox { border:1px solid var(--line); border-radius:14px; background:#f8fbff; padding:10px; display:grid; gap:8px; align-content:start; }
    .toolbox-setup { display:grid; gap:8px; padding:10px; border:1px solid rgba(149,169,200,0.24); border-radius:12px; background:linear-gradient(180deg,rgba(255,255,255,0.96),rgba(242,247,253,0.92)); }
    .toolbox-setup-title { font-size:0.74rem; font-weight:700; letter-spacing:0.03em; text-transform:uppercase; color:var(--muted); }
    .toolbox-setup-row { display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
    .toolbox-setup select { min-width:88px; }
    .toolbox-setup button { padding:8px 12px; border-radius:10px; }
    .toolbox h3 { margin:0 0 4px; font-size:1rem; }
    .tool-item { border:1px solid rgba(20,32,51,0.12); border-left-width:6px; border-radius:10px; padding:10px 10px; font-weight:700; cursor:grab; user-select:none; }
    .tool-item:active { cursor:grabbing; }
    .script-canvas { border:1px dashed #b8c6de; border-radius:14px; background:#fbfdff; padding:10px; min-height:220px; }
    .script-list { display:grid; gap:6px; margin:0; }
    .block { display:flex; gap:5px; align-items:flex-start; flex-wrap:wrap; border:1px solid var(--line); border-radius:10px; padding:8px 10px; box-shadow:0 1px 0 rgba(20,32,51,0.05); }
    .block:active { cursor:grabbing; }
    .block-action { cursor:grab; align-items:center; }
    .block-action .block-label { padding-top:0; }
    .block-set_color, .tool-set_color,
    .block-set_brightness, .tool-set_brightness,
    .block-fade, .tool-fade { background:#f1f6ff; border-left:6px solid #4e87d8; }
    .block-wait, .tool-wait { background:#fff8ee; border-left:6px solid #c4892b; }
    .block-repeat_start, .tool-repeat_start { background:#f7f3ff; border-left:6px solid #7c58c9; }
    .block-repeat_end, .tool-repeat_end { background:#f2edff; border-left:6px solid #5d40a8; }
    .block-all_off, .tool-all_off { background:#fff1f1; border-left:6px solid #d85a5a; }
    .block-script_start { background:#e9f5ef; border-left:6px solid #22a06b; }
    .block-script_end { background:#e9f5ef; border-left:6px solid #22a06b; }
    .block-script_start, .block-script_end { align-items:center; min-height:auto; }
    .block-script_start .block-label, .block-script_end .block-label { padding-top:0; }
    .drop-hint { border-top:3px solid #2f8eff; margin-top:-2px; }
    .canvas-empty { color:var(--muted); font-size:0.9rem; padding:8px; }
    .block-label { font-weight:700; font-size:0.88rem; min-width:84px; color:#1f2f49; padding-top:26px; }
    .field-stack { display:grid; gap:0; }
    .field-inline { display:flex; align-items:center; gap:6px; }
    .field-inline label { margin:0; white-space:nowrap; }
    .number-wrap { display:flex; align-items:center; gap:6px; }
    .block-remove { margin-left:auto; background:none; border:none; color:#c0392b; font-size:1.1rem; cursor:pointer; padding:0 4px; }
    .status-bar { font-size:0.88rem; color:var(--muted); padding:6px 0; }
    .status-bar.running { color:var(--accent); font-weight:700; }
    .command-bar .status-bar { display:flex; align-items:center; min-height:44px; padding:0 10px; margin-left:auto; border-radius:12px; background:rgba(255,255,255,0.68); border:1px solid rgba(149,169,200,0.28); }
    .inline-note { font-size:0.8rem; color:var(--muted); align-self:center; }
    .mini-wheel-control { position:relative; display:flex; align-items:center; gap:6px; }
    .color-chip-btn { width:26px; height:26px; border-radius:7px; border:1px solid rgba(20,32,51,0.25); cursor:pointer; }
    .mini-wheel-panel { display:none; position:absolute; top:34px; left:0; justify-items:center; gap:8px; padding:8px; border:1px solid var(--line); border-radius:14px; background:linear-gradient(180deg,#ffffff,#f6f9fd); box-shadow:0 12px 26px rgba(20,32,51,0.18); z-index:40; }
    .mini-wheel-panel.open { display:grid; }
    .mini-wheel-stage { position:relative; width:96px; height:96px; }
    .mini-wheel { width:96px; height:96px; border-radius:50%; cursor:crosshair; box-shadow:inset 0 0 0 1px rgba(20,32,51,0.08); }
    .mini-wheel-marker { position:absolute; top:0; left:0; width:12px; height:12px; border-radius:50%; border:2px solid #fff; box-shadow:0 0 0 1px rgba(20,32,51,0.35), 0 2px 8px rgba(20,32,51,0.22); pointer-events:none; transform:translate(-50%, -50%); }
    .color-readout { display:flex; align-items:center; gap:8px; font-weight:700; color:var(--text); }
    .color-swatch { width:18px; height:18px; border-radius:50%; border:1px solid rgba(20,32,51,0.16); }
    .color-hex { font-family:"SFMono-Regular","Consolas",monospace; font-size:0.82rem; }
    @media (max-width: 980px) {
      .logic-layout { grid-template-columns:1fr; }
    }
  </style>
</head>
<body>
<main class="wrap">
  <div class="topbar">
    <details class="menu">
      <summary class="menu-button" aria-label="Menue"><span class="menu-icon">&#9776;</span></summary>
      <nav class="menu-list">
        <a href="/">LED-Logic</a>
        <a href="/config">Konfiguration</a>
      </nav>
    </details>
    <div class="title-row">
      <svg class="title-icon" viewBox="0 0 64 64" aria-hidden="true">
        <image href=")HTML";
  page += kLedLogicIconUrl;
  page += R"HTML(" x="-2" y="-2" width="68" height="68" preserveAspectRatio="xMidYMid slice"/>
      </svg>
        <h1 class="title">LED-Logic <span class="title-version">V )HTML";
  page += firmwareVersion;
  page += R"HTML(</span></h1>
    </div>
  </div>

  <section class="panel">
    <div class="command-bar">
      <div class="command-group">
        <button id="saveScriptBtn" class="toolbar-btn save momentary" onclick="saveScript()" title="Speichern ohne Starten">
          <span class="toolbar-text">Speichern</span>
          <span class="toolbar-icon-wrap">
            <svg class="toolbar-icon-svg" viewBox="0 0 24 24" aria-hidden="true">
              <path d="M5 4h11l3 3v13H5z"/>
              <path d="M8 4v6h8V4"/>
              <path d="M9 17h6"/>
            </svg>
          </span>
        </button>
        <button id="startScriptBtn" class="toolbar-btn start" onclick="runScript()" title="Script starten" aria-pressed="false">
          <span class="toolbar-text">Start</span>
          <span class="toolbar-icon-wrap">
            <svg class="toolbar-icon-svg" viewBox="0 0 24 24" aria-hidden="true">
              <circle cx="12" cy="12" r="8.5"/>
              <path d="M10 8.5 16 12l-6 3.5z" fill="currentColor" stroke="none"/>
            </svg>
          </span>
        </button>
        <button id="stopScriptBtn" class="toolbar-btn stop toggle-active" onclick="stopScript()" title="Script anhalten" aria-pressed="true">
          <span class="toolbar-text">Stop</span>
          <span class="toolbar-icon-wrap">
            <svg class="toolbar-icon-svg" viewBox="0 0 24 24" aria-hidden="true">
              <circle cx="12" cy="12" r="8.5"/>
              <rect x="9" y="9" width="6" height="6" rx="1.2" fill="currentColor" stroke="none"/>
            </svg>
          </span>
        </button>
        <div class="toolbar-separator" aria-hidden="true"></div>
        <button id="clearScriptBtn" class="toolbar-btn clear momentary" onclick="clearScript()" title="Gespeichertes Script löschen">
          <span class="toolbar-text">Löschen</span>
          <span class="toolbar-icon-wrap">
            <svg class="toolbar-icon-svg" viewBox="0 0 24 24" aria-hidden="true">
              <path d="M8 7h8"/>
              <path d="M10 4h4"/>
              <path d="M7 7l1 11h8l1-11"/>
              <path d="M10 10v5"/>
              <path d="M14 10v5"/>
            </svg>
          </span>
        </button>
        <div class="led-sim-panel" aria-label="LED Simulation">
          <span class="led-sim-label">LEDs</span>
          <div class="led-sim-list" id="ledSimList"></div>
        </div>
      </div>
      <div class="status-bar" id="statusBar">Bereit.</div>
    </div>

    <div class="logic-layout">
      <aside class="toolbox" id="toolbox">
        <div class="toolbox-setup">
          <div class="toolbox-setup-title">Setup</div>
          <div class="toolbox-setup-row">
            <label for="ledCountSelect" style="margin:0;">LED Anzahl</label>
            <select id="ledCountSelect">
              <option value="1">1</option>
              <option value="2">2</option>
              <option value="3">3</option>
              <option value="4">4</option>
              <option value="5">5</option>
              <option value="6">6</option>
              <option value="7">7</option>
              <option value="8">8</option>
              <option value="9">9</option>
              <option value="10">10</option>
              <option value="11">11</option>
              <option value="12">12</option>
            </select>
            <button type="button" class="secondary" onclick="saveLedCount()">Übernehmen</button>
          </div>
        </div>
        <h3>Werkzeugleiste</h3>
        <div class="tool-item tool-set_color" draggable="true" data-tool="set_color">Farbe setzen</div>
        <div class="tool-item tool-set_brightness" draggable="true" data-tool="set_brightness">Helligkeit setzen</div>
        <div class="tool-item tool-fade" draggable="true" data-tool="fade">Überblenden</div>
        <div class="tool-item tool-wait" draggable="true" data-tool="wait">Warten</div>
        <div class="tool-item tool-repeat_start" draggable="true" data-tool="repeat">Repeat</div>
        <div class="tool-item tool-all_off" draggable="true" data-tool="all_off">Alles aus</div>
      </aside>

      <section class="script-canvas" id="scriptCanvas">
        <div class="script-list" id="scriptList"></div>
      </section>
    </div>
  </section>
</main>

<script>
  const MAX_LEDS = )HTML";
  page += String(ledCount);
  page += R"HTML(;
  const CURRENT_LED_COUNT = MAX_LEDS;
  let steps = [];
  let scriptLoopEnabled = false;
  let dragPayload = null;
  let dropIndex = -1;
  let dragDropEnabled = true;
  let ledPreviewFrameId = 0;
  let ledPreviewTimeoutId = 0;
  let ledPreviewRunId = 0;
  let ledSimulatorRunning = false;

  function syncToolbarStates() {
    const startButton = document.getElementById('startScriptBtn');
    const stopButton = document.getElementById('stopScriptBtn');
    if (!startButton || !stopButton) {
      return;
    }
    startButton.classList.toggle('toggle-active', ledSimulatorRunning);
    stopButton.classList.toggle('toggle-active', !ledSimulatorRunning);
    startButton.setAttribute('aria-pressed', ledSimulatorRunning ? 'true' : 'false');
    stopButton.setAttribute('aria-pressed', ledSimulatorRunning ? 'false' : 'true');
  }

  function hexToRgb(hex) {
    const clean = hex.replace('#', '');
    return {
      r: parseInt(clean.slice(0, 2), 16),
      g: parseInt(clean.slice(2, 4), 16),
      b: parseInt(clean.slice(4, 6), 16)
    };
  }

  function rgbToHex(r, g, b) {
    return '#' + [r, g, b].map(value => value.toString(16).padStart(2, '0')).join('').toUpperCase();
  }

  function rgbToHsv(r, g, b) {
    const red = r / 255;
    const green = g / 255;
    const blue = b / 255;
    const max = Math.max(red, green, blue);
    const min = Math.min(red, green, blue);
    const delta = max - min;
    let hue = 0;

    if (delta !== 0) {
      if (max === red) {
        hue = 60 * (((green - blue) / delta) % 6);
      } else if (max === green) {
        hue = 60 * (((blue - red) / delta) + 2);
      } else {
        hue = 60 * (((red - green) / delta) + 4);
      }
    }

    if (hue < 0) {
      hue += 360;
    }

    return {
      h: hue,
      s: max === 0 ? 0 : delta / max,
      v: max
    };
  }

  function hsvToRgb(h, s, v) {
    const chroma = v * s;
    const hueSection = h / 60;
    const x = chroma * (1 - Math.abs((hueSection % 2) - 1));
    let red = 0;
    let green = 0;
    let blue = 0;

    if (hueSection >= 0 && hueSection < 1) {
      red = chroma; green = x;
    } else if (hueSection < 2) {
      red = x; green = chroma;
    } else if (hueSection < 3) {
      green = chroma; blue = x;
    } else if (hueSection < 4) {
      green = x; blue = chroma;
    } else if (hueSection < 5) {
      red = x; blue = chroma;
    } else {
      red = chroma; blue = x;
    }

    const match = v - chroma;
    return {
      r: Math.round((red + match) * 255),
      g: Math.round((green + match) * 255),
      b: Math.round((blue + match) * 255)
    };
  }

  function drawColorWheel(canvas) {
    const ctx = canvas.getContext('2d');
    const size = canvas.width;
    const radius = size / 2;
    const image = ctx.createImageData(size, size);

    for (let y = 0; y < size; y += 1) {
      for (let x = 0; x < size; x += 1) {
        const dx = x + 0.5 - radius;
        const dy = y + 0.5 - radius;
        const distance = Math.sqrt(dx * dx + dy * dy);
        const pixel = (y * size + x) * 4;

        if (distance > radius) {
          image.data[pixel + 3] = 0;
          continue;
        }

        const hue = (Math.atan2(dy, dx) * 180 / Math.PI + 360) % 360;
        const saturation = Math.min(distance / radius, 1);
        const rgb = hsvToRgb(hue, saturation, 1);

        image.data[pixel] = rgb.r;
        image.data[pixel + 1] = rgb.g;
        image.data[pixel + 2] = rgb.b;
        image.data[pixel + 3] = 255;
      }
    }

    ctx.putImageData(image, 0, 0);
  }

  function initColorWheel(control) {
    const canvas = control.querySelector('.mini-wheel');
    const marker = control.querySelector('.mini-wheel-marker');
    const input = control.querySelector('.color-wheel-input');
    const chip = control.querySelector('.color-chip-btn');
    const panel = control.querySelector('.mini-wheel-panel');
    const swatch = control.querySelector('.color-swatch');
    const hexLabel = control.querySelector('.color-hex');
    const radius = canvas.width / 2;
    let dragging = false;

    drawColorWheel(canvas);

    function positionMarker(hex) {
      const rgb = hexToRgb(hex);
      const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
      const angle = hsv.h * Math.PI / 180;
      const distance = hsv.s * radius;
      const left = radius + Math.cos(angle) * distance;
      const top = radius + Math.sin(angle) * distance;
      marker.style.left = left + 'px';
      marker.style.top = top + 'px';
      chip.style.background = hex;
      swatch.style.background = hex;
      hexLabel.textContent = hex;
    }

    function updateFromPointer(event) {
      const rect = canvas.getBoundingClientRect();
      const x = event.clientX - rect.left;
      const y = event.clientY - rect.top;
      const dx = x - rect.width / 2;
      const dy = y - rect.height / 2;
      const distance = Math.min(Math.sqrt(dx * dx + dy * dy), rect.width / 2);
      const hue = (Math.atan2(dy, dx) * 180 / Math.PI + 360) % 360;
      const saturation = Math.min(distance / (rect.width / 2), 1);
      const rgb = hsvToRgb(hue, saturation, 1);
      const hex = rgbToHex(rgb.r, rgb.g, rgb.b);
      input.value = hex;
      positionMarker(hex);
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.dispatchEvent(new Event('change', { bubbles: true }));
    }

    positionMarker(input.value);

    canvas.addEventListener('pointerdown', event => {
      dragging = true;
      canvas.setPointerCapture(event.pointerId);
      updateFromPointer(event);
    });

    canvas.addEventListener('pointermove', event => {
      if (dragging) {
        updateFromPointer(event);
      }
    });

    canvas.addEventListener('pointerup', event => {
      dragging = false;
      canvas.releasePointerCapture(event.pointerId);
    });

    canvas.addEventListener('pointerleave', () => {
      dragging = false;
    });

    chip.addEventListener('click', event => {
      event.preventDefault();
      event.stopPropagation();
      panel.classList.toggle('open');
      syncDragDropWithColorWheel();
    });
  }

  function initAllColorWheels() {
    document.querySelectorAll('.mini-wheel-control').forEach(initColorWheel);
  }

  function defaultValuesForType(type) {
    if (type === 'set_color') {
      return { led: '0', color: '#FF0000', br: '100' };
    }
    if (type === 'set_brightness') {
      return { led: '0', br: '100' };
    }
    if (type === 'fade') {
      return { led: '0', from: '#FF0000', to: '#0000FF', br: '100', s: '1.0' };
    }
    if (type === 'wait') {
      return { s: '1.0' };
    }
    if (type === 'repeat_start') {
      return { count: '2' };
    }
    return {};
  }

  function ledOptions(selected) {
    let options = '';
    for (let i = 0; i < MAX_LEDS; i += 1) {
      options += `<option value="${i}"${String(selected) === String(i) ? ' selected' : ''}>LED ${i + 1}</option>`;
    }
    return options;
  }

  function buildWheelControl(key, value, labelText) {
    const label = labelText ? `<label>${labelText}</label>` : '';
    return `<div class="field-stack">${label}<div class="mini-wheel-control"><button type="button" class="color-chip-btn" style="background:${value};" title="Farbrad öffnen"></button><div class="mini-wheel-panel"><div class="mini-wheel-stage"><canvas class="mini-wheel" width="96" height="96"></canvas><div class="mini-wheel-marker"></div></div><input type="hidden" data-k="${key}" class="color-wheel-input" value="${value}"><div class="color-readout"><span class="color-swatch" style="background:${value};"></span><span class="color-hex">${value}</span></div></div></div></div>`;
  }

  function readBlockValues(id, type) {
    const el = document.getElementById('block_' + id);
    if (!el) {
      return null;
    }
    const get = name => {
      const field = el.querySelector(`[data-k="${name}"]`);
      return field ? field.value : null;
    };
    if (type === 'set_color') {
      return { led: get('led'), color: get('color'), br: get('br') };
    }
    if (type === 'set_brightness') {
      return { led: get('led'), br: get('br') };
    }
    if (type === 'fade') {
      return { led: get('led'), from: get('from'), to: get('to'), br: get('br'), s: get('s') };
    }
    if (type === 'wait') {
      return { s: get('s') };
    }
    if (type === 'repeat_start') {
      return { count: get('count') };
    }
    return {};
  }

  function syncStepsFromDom() {
    steps = steps.map(step => ({
      ...step,
      values: readBlockValues(step.id, step.type) || step.values || defaultValuesForType(step.type)
    }));
  }

  function removeBlock(id) {
    syncStepsFromDom();
    steps = steps.filter(step => step.id !== id);
    renderList();
  }

  function readBlock(id) {
    const el = document.getElementById('block_' + id);
    if (!el) {
      return null;
    }
    const type = el.dataset.type;
    const values = readBlockValues(id, type);
    const step = { op: type };
    if (type === 'set_color') {
      step.op = 'set';
      step.led = parseInt(values.led, 10);
      step.color = values.color;
      step.br = parseInt(values.br, 10);
    } else if (type === 'set_brightness') {
      step.op = 'brightness';
      step.led = parseInt(values.led, 10);
      step.br = parseInt(values.br, 10);
    } else if (type === 'fade') {
      step.led = parseInt(values.led, 10);
      step.from = values.from;
      step.to = values.to;
      step.br = parseInt(values.br, 10);
      step.s = parseFloat(values.s);
    } else if (type === 'wait') {
      step.s = parseFloat(values.s);
    } else if (type === 'repeat_start') {
      step.op = 'repeat_start';
      step.count = parseInt(values.count, 10);
    } else if (type === 'repeat_end') {
      step.op = 'repeat_end';
    }
    return step;
  }

  function createLedPreviewState() {
    return Array.from({ length: MAX_LEDS }, () => ({ enabled: false, r: 0, g: 0, b: 0, br: 0 }));
  }

  function ensureLedSimulatorDots() {
    const list = document.getElementById('ledSimList');
    if (!list) {
      return [];
    }
    if (list.children.length !== MAX_LEDS) {
      list.innerHTML = '';
      for (let i = 0; i < MAX_LEDS; i += 1) {
        const dot = document.createElement('div');
        dot.className = 'led-sim-dot';
        dot.title = `LED ${i + 1}`;
        list.appendChild(dot);
      }
    }
    return Array.from(list.children);
  }

  function applyLedPreviewState(state) {
    const dots = ensureLedSimulatorDots();
    dots.forEach((dot, index) => {
      const led = state[index] || { enabled: false, r: 0, g: 0, b: 0, br: 0 };
      if (!led.enabled || led.br <= 0) {
        dot.classList.remove('active');
        dot.style.background = '#d9e1ee';
        dot.style.boxShadow = 'inset 0 1px 2px rgba(255,255,255,0.95)';
        dot.style.opacity = '0.45';
        return;
      }
      const factor = Math.max(0, Math.min(led.br, 100)) / 100;
      const rr = Math.round(led.r * factor);
      const gg = Math.round(led.g * factor);
      const bb = Math.round(led.b * factor);
      dot.classList.add('active');
      dot.style.background = `rgb(${rr}, ${gg}, ${bb})`;
      dot.style.boxShadow = `0 0 0 1px rgba(20,32,51,0.14), 0 0 14px rgba(${rr}, ${gg}, ${bb}, 0.48)`;
      dot.style.opacity = String(0.35 + factor * 0.65);
    });
  }

  function stopLedPreviewPlayback() {
    if (ledPreviewFrameId) {
      cancelAnimationFrame(ledPreviewFrameId);
      ledPreviewFrameId = 0;
    }
    if (ledPreviewTimeoutId) {
      clearTimeout(ledPreviewTimeoutId);
      ledPreviewTimeoutId = 0;
    }
  }

  function resetLedPreviewPlayback() {
    stopLedPreviewPlayback();
    ledPreviewRunId += 1;
    applyLedPreviewState(createLedPreviewState());
  }

  function syncLedCountSelect() {
    const select = document.getElementById('ledCountSelect');
    if (select) {
      select.value = String(CURRENT_LED_COUNT);
    }
  }

  function saveLedCount() {
    const select = document.getElementById('ledCountSelect');
    if (!select) {
      return;
    }
    const formData = new URLSearchParams();
    formData.set('ledCount', select.value);
    fetch('/led/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
      body: formData.toString()
    }).then(response => {
      if (response.ok || response.status === 302) {
        window.location.reload();
        return;
      }
      response.text().then(text => setStatus('Fehler: ' + text, false));
    }).catch(() => setStatus('Speichern der LED-Anzahl fehlgeschlagen.', false));
  }

  function scheduleLedPreviewNext(callback, delayMs) {
    stopLedPreviewPlayback();
    if (delayMs <= 0) {
      callback();
      return;
    }
    ledPreviewTimeoutId = window.setTimeout(() => {
      ledPreviewTimeoutId = 0;
      callback();
    }, delayMs);
  }

  function startLedPreviewPlayback() {
    stopLedPreviewPlayback();
    if (!ledSimulatorRunning) {
      applyLedPreviewState(createLedPreviewState());
      return;
    }
    const payload = buildPayload();
    if (payload.error || !payload.ops || payload.ops.length === 0) {
      applyLedPreviewState(createLedPreviewState());
      return;
    }

    const runId = ++ledPreviewRunId;
    const state = createLedPreviewState();

    function runStep(stepIndex) {
      if (runId !== ledPreviewRunId) {
        return;
      }
      if (stepIndex >= payload.ops.length) {
        if (payload.loop) {
          scheduleLedPreviewNext(() => runStep(0), 0);
        }
        return;
      }

      const step = payload.ops[stepIndex];
      if (step.op === 'set') {
        const rgb = hexToRgb(step.color || '#000000');
        state[step.led] = { enabled: true, r: rgb.r, g: rgb.g, b: rgb.b, br: Number.isFinite(step.br) ? step.br : 100 };
        applyLedPreviewState(state);
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
        return;
      }

      if (step.op === 'brightness') {
        const current = state[step.led] || { enabled: false, r: 0, g: 0, b: 0, br: 0 };
        state[step.led] = { ...current, enabled: true, br: Number.isFinite(step.br) ? step.br : current.br };
        applyLedPreviewState(state);
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
        return;
      }

      if (step.op === 'all_off') {
        for (let i = 0; i < state.length; i += 1) {
          state[i] = { enabled: false, r: 0, g: 0, b: 0, br: 0 };
        }
        applyLedPreviewState(state);
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
        return;
      }

      if (step.op === 'wait') {
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), Math.max(0, (step.s || 0) * 1000));
        return;
      }

      if (step.op === 'fade') {
        const from = hexToRgb(step.from || '#000000');
        const to = hexToRgb(step.to || '#000000');
        const duration = Math.max(0, (step.s || 0) * 1000);
        const brightness = Number.isFinite(step.br) ? step.br : 100;
        if (duration === 0) {
          state[step.led] = { enabled: true, r: to.r, g: to.g, b: to.b, br: brightness };
          applyLedPreviewState(state);
          scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
          return;
        }

        const startTime = performance.now();
        function animateFade(now) {
          if (runId !== ledPreviewRunId) {
            return;
          }
          const progress = Math.min(1, (now - startTime) / duration);
          state[step.led] = {
            enabled: true,
            r: Math.round(from.r + (to.r - from.r) * progress),
            g: Math.round(from.g + (to.g - from.g) * progress),
            b: Math.round(from.b + (to.b - from.b) * progress),
            br: brightness
          };
          applyLedPreviewState(state);
          if (progress < 1) {
            ledPreviewFrameId = requestAnimationFrame(animateFade);
          } else {
            ledPreviewFrameId = 0;
            runStep(stepIndex + 1);
          }
        }
        ledPreviewFrameId = requestAnimationFrame(animateFade);
        return;
      }

      scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
    }

    applyLedPreviewState(state);
    runStep(0);
  }

  function insertStep(type, index) {
    const newStep = { id: Date.now() + Math.floor(Math.random() * 1000), type, values: defaultValuesForType(type) };
    if (index < 0 || index > steps.length) {
      steps.push(newStep);
    } else {
      steps.splice(index, 0, newStep);
    }
  }

  function insertRepeatPair(index) {
    const start = { id: Date.now() + Math.floor(Math.random() * 1000), type: 'repeat_start', values: defaultValuesForType('repeat_start') };
    const end = { id: Date.now() + Math.floor(Math.random() * 1000), type: 'repeat_end', values: {} };
    if (index < 0 || index > steps.length) {
      steps.push(start, end);
    } else {
      steps.splice(index, 0, start, end);
    }
  }

  function getDropIndexFromY(y) {
    const blocks = Array.from(document.querySelectorAll('.script-list .block-action'));
    for (let i = 0; i < blocks.length; i += 1) {
      const rect = blocks[i].getBoundingClientRect();
      if (y < rect.top + rect.height / 2) {
        return i;
      }
    }
    return blocks.length;
  }

  function clearDropHints() {
    document.querySelectorAll('.script-list .block-action').forEach(block => block.classList.remove('drop-hint'));
  }

  function setDragDropEnabled(enabled) {
    dragDropEnabled = enabled;
    document.querySelectorAll('.tool-item').forEach(tool => {
      tool.draggable = enabled;
    });
    document.querySelectorAll('.script-list .block-action').forEach(block => {
      block.draggable = enabled;
    });
    if (!enabled) {
      dragPayload = null;
      dropIndex = -1;
      clearDropHints();
    }
  }

  function syncDragDropWithColorWheel() {
    const hasOpenPanel = Boolean(document.querySelector('.mini-wheel-panel.open'));
    setDragDropEnabled(!hasOpenPanel);
  }

  function closeAllColorWheels() {
    document.querySelectorAll('.mini-wheel-panel.open').forEach(panel => panel.classList.remove('open'));
    syncDragDropWithColorWheel();
  }

  function paintDropHint(index) {
    clearDropHints();
    const blocks = Array.from(document.querySelectorAll('.script-list .block-action'));
    if (index >= 0 && index < blocks.length) {
      blocks[index].classList.add('drop-hint');
    }
  }

  function setupToolboxDnD() {
    document.querySelectorAll('.tool-item').forEach(tool => {
      tool.addEventListener('dragstart', event => {
        if (!dragDropEnabled) {
          event.preventDefault();
          return;
        }
        dragPayload = { source: 'tool', type: tool.dataset.tool };
        event.dataTransfer.effectAllowed = 'copy';
      });
      tool.addEventListener('dragend', () => {
        dragPayload = null;
        dropIndex = -1;
        clearDropHints();
      });
    });

    const canvas = document.getElementById('scriptCanvas');
    canvas.addEventListener('dragover', event => {
      if (!dragDropEnabled) {
        clearDropHints();
        return;
      }
      event.preventDefault();
      dropIndex = getDropIndexFromY(event.clientY);
      paintDropHint(dropIndex);
    });

    canvas.addEventListener('dragleave', event => {
      if (!canvas.contains(event.relatedTarget)) {
        clearDropHints();
      }
    });

    canvas.addEventListener('drop', event => {
      if (!dragDropEnabled) {
        return;
      }
      event.preventDefault();
      syncStepsFromDom();
      const index = getDropIndexFromY(event.clientY);
      if (!dragPayload) {
        return;
      }
      if (dragPayload.source === 'tool') {
        if (dragPayload.type === 'repeat') {
          insertRepeatPair(index);
        } else {
          insertStep(dragPayload.type, index);
        }
      } else if (dragPayload.source === 'block') {
        const from = steps.findIndex(step => step.id === dragPayload.id);
        if (from >= 0) {
          const [moved] = steps.splice(from, 1);
          let target = index;
          if (from < index) {
            target -= 1;
          }
          steps.splice(target, 0, moved);
        }
      }
      dragPayload = null;
      dropIndex = -1;
      clearDropHints();
      renderList();
    });
  }

  function renderList() {
    const list = document.getElementById('scriptList');
    list.innerHTML = '';

    const scriptStart = document.createElement('div');
    scriptStart.className = 'block block-script_start';
    scriptStart.innerHTML = `<span class="block-label">Script Start</span><div class="number-wrap"><input type="checkbox" id="scriptLoopChk" ${scriptLoopEnabled ? 'checked' : ''}><span>wiederholen</span></div>`;
    list.appendChild(scriptStart);

    steps.forEach(step => {
      const values = step.values || defaultValuesForType(step.type);
      const div = document.createElement('div');
      div.className = 'block block-action block-' + step.type;
      div.id = 'block_' + step.id;
      div.dataset.type = step.type;
      div.draggable = dragDropEnabled;
      div.addEventListener('dragstart', event => {
        if (!dragDropEnabled) {
          event.preventDefault();
          return;
        }
        dragPayload = { source: 'block', id: step.id };
        event.dataTransfer.effectAllowed = 'move';
      });
      div.addEventListener('dragend', () => {
        dragPayload = null;
        dropIndex = -1;
        clearDropHints();
      });

      // Einrueckung innerhalb Script/Repeat-Strukturen
      const all = steps;
      let depth = 1;
      for (let i = 0; i < all.length; i += 1) {
        if (all[i].id === step.id) {
          break;
        }
        if (all[i].type === 'repeat_start') {
          depth += 1;
        } else if (all[i].type === 'repeat_end') {
          depth = Math.max(1, depth - 1);
        }
      }
      if (step.type === 'repeat_end') {
        depth = Math.max(1, depth - 1);
      }
      div.style.marginLeft = String(depth * 18) + 'px';

      let inner = '';
      if (step.type === 'set_color') {
        inner = `<span class="block-label">Farbe</span><div class="field-inline"><label>LED</label><select data-k="led">${ledOptions(values.led)}</select></div><div class="field-inline"><label>Farbe</label>${buildWheelControl('color', values.color, '')}</div><div class="field-inline"><label>Helligkeit</label><div class="number-wrap"><input type="number" data-k="br" value="${values.br}" min="0" max="100" style="width:68px"><span>%</span></div></div>`;
      } else if (step.type === 'set_brightness') {
        inner = `<span class="block-label">Helligkeit</span><div class="field-inline"><label>LED</label><select data-k="led">${ledOptions(values.led)}</select></div><div class="field-inline"><label>Wert</label><div class="number-wrap"><input type="number" data-k="br" value="${values.br}" min="0" max="100" style="width:68px"><span>%</span></div></div>`;
      } else if (step.type === 'fade') {
        inner = `<span class="block-label">Blend</span><div class="field-inline"><label>LED</label><select data-k="led">${ledOptions(values.led)}</select></div><div class="field-inline"><label>Von</label>${buildWheelControl('from', values.from, '')}</div><div class="field-inline"><label>Nach</label>${buildWheelControl('to', values.to, '')}</div><div class="field-inline"><label>Helligkeit</label><div class="number-wrap"><input type="number" data-k="br" value="${values.br}" min="0" max="100" style="width:68px"><span>%</span></div></div><div class="field-inline"><label>Dauer</label><div class="number-wrap"><input type="number" data-k="s" value="${values.s}" min="0" max="30" step="0.5" style="width:68px"><span>s</span></div></div>`;
      } else if (step.type === 'wait') {
        inner = `<span class="block-label">Warten</span><div class="field-inline"><label>Dauer</label><div class="number-wrap"><input type="number" data-k="s" value="${values.s}" min="0" max="30" step="0.5" style="width:68px"><span>s</span></div></div>`;
      } else if (step.type === 'repeat_start') {
        inner = `<span class="block-label">Repeat</span><div class="field-inline"><label>Wiederhole</label><div class="number-wrap"><input type="number" data-k="count" value="${values.count}" min="1" max="16" step="1" style="width:68px"><span>mal</span></div></div><span class="inline-note">ab hier bis Repeat Ende</span>`;
      } else if (step.type === 'repeat_end') {
        inner = `<span class="block-label">Repeat Stop</span><span class="inline-note">Ende Wiederhol-Block</span>`;
      } else if (step.type === 'all_off') {
        inner = `<span class="block-label">Alles aus</span>`;
      }
      inner += `<button class="block-remove" onclick="removeBlock(${step.id})" title="Entfernen">&#10005;</button>`;
      div.innerHTML = inner;
      list.appendChild(div);
    });

    const scriptEnd = document.createElement('div');
    scriptEnd.className = 'block block-script_end';
    scriptEnd.innerHTML = `<span class="block-label">${scriptLoopEnabled ? 'Script wiederholen' : 'Script Ende'}</span>`;
    list.appendChild(scriptEnd);

    const loopCheckbox = document.getElementById('scriptLoopChk');
    if (loopCheckbox) {
      loopCheckbox.addEventListener('change', event => {
        scriptLoopEnabled = event.target.checked;
        renderList();
      });
    }

    if (steps.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'canvas-empty';
      empty.style.marginLeft = '18px';
      empty.textContent = 'Ziehe ein Element aus der Werkzeugleiste zwischen Script Start und Script Ende.';
      list.insertBefore(empty, scriptEnd);
    }

    initAllColorWheels();
    syncDragDropWithColorWheel();
    startLedPreviewPlayback();
  }

  function compileSequence(items, startIndex) {
    const ops = [];
    let index = startIndex;
    while (index < items.length) {
      const current = items[index];
      if (current.op === 'repeat_end') {
        return { ops, nextIndex: index + 1, closed: true };
      }
      if (current.op === 'repeat_start') {
        const nested = compileSequence(items, index + 1);
        if (!nested.closed) {
          return { error: 'Repeat Start ohne Repeat Ende.' };
        }
        const count = Math.max(1, Math.min(current.count || 1, 16));
        for (let i = 0; i < count; i += 1) {
          nested.ops.forEach(op => ops.push(JSON.parse(JSON.stringify(op))));
        }
        index = nested.nextIndex;
        continue;
      }
      ops.push(current);
      index += 1;
    }
    return { ops, nextIndex: index, closed: false };
  }

  function buildPayload() {
    syncStepsFromDom();
    const raw = steps.map(stepRef => readBlock(stepRef.id)).filter(Boolean);
    if (raw.some(item => item.op === 'repeat_end')) {
      // top-level repeat_end without matching start is handled below by compileSequence return
    }
    const compiled = compileSequence(raw, 0);
    if (compiled.error) {
      return { error: compiled.error };
    }
    if (compiled.closed) {
      return { error: 'Repeat Ende ohne passenden Repeat Start.' };
    }
    return {
      loop: scriptLoopEnabled,
      ops: compiled.ops
    };
  }

  function runScript() {
    const payload = buildPayload();
    if (payload.error) {
      setStatus(payload.error, false);
      return;
    }
    if (payload.ops.length === 0) {
      setStatus('Keine Schritte definiert.', false);
      return;
    }
    fetch('/script/run', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload) })
      .then(response => {
        if (response.ok) {
          setStatus('Script laeuft.', true);
          ledSimulatorRunning = true;
          syncToolbarStates();
          startLedPreviewPlayback();
          return;
        }
        response.text().then(text => setStatus('Fehler: ' + text, false));
      })
      .catch(() => setStatus('Verbindungsfehler.', false));
  }

  function saveScript() {
    const payload = buildPayload();
    if (payload.error) {
      setStatus(payload.error, false);
      return;
    }
    if (payload.ops.length === 0) {
      setStatus('Keine Schritte definiert.', false);
      return;
    }
    fetch('/script/save', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload) })
      .then(response => {
        if (response.ok) {
          setStatus('Script gespeichert.', false);
          if (ledSimulatorRunning) {
            startLedPreviewPlayback();
          }
          return;
        }
        response.text().then(text => setStatus('Fehler: ' + text, false));
      })
      .catch(() => setStatus('Verbindungsfehler.', false));
  }

  function stopScript() {
    fetch('/script/stop', { method: 'POST' })
      .then(response => {
        if (response.ok) {
          setStatus('Script angehalten.', false);
          ledSimulatorRunning = false;
          syncToolbarStates();
          resetLedPreviewPlayback();
        }
      })
      .catch(() => {});
  }

  function clearScript() {
    fetch('/script/clear', { method: 'POST' })
      .then(response => {
        if (response.ok) {
          setStatus('Gespeichertes Script geloescht.', false);
          ledSimulatorRunning = false;
          syncToolbarStates();
          resetLedPreviewPlayback();
        }
      })
      .catch(() => setStatus('Loeschen fehlgeschlagen.', false));
  }

  function setStatus(message, running) {
    const bar = document.getElementById('statusBar');
    bar.textContent = message;
    bar.className = 'status-bar' + (running ? ' running' : '');
  }

  document.getElementById('scriptList').addEventListener('input', () => {
    if (ledSimulatorRunning) {
      startLedPreviewPlayback();
    }
  });
  document.getElementById('scriptList').addEventListener('change', () => {
    if (ledSimulatorRunning) {
      startLedPreviewPlayback();
    }
  });
  document.addEventListener('pointerdown', event => {
    if (event.target.closest('.mini-wheel-control')) {
      return;
    }
    closeAllColorWheels();
  });
  syncLedCountSelect();
  syncToolbarStates();
  setupToolboxDnD();
  renderList();
</script>
</body>
</html>)HTML";
  return page;
}

String buildConfigPage() {
  const bool isConnected = WiFi.status() == WL_CONNECTED;

  String page;
  page.reserve(12000);
  page += R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Konfiguration</title>
  <link rel="icon" type="image/png" href=")HTML";
  page += kLedLogicFaviconUrl;
  page += R"HTML(">
  <style>
    :root { --bg:#f5efe6; --panel:#fffcf7; --line:#d8c9b8; --text:#1f2a30; --muted:#56656f; --accent:#0b6e4f; --danger:#ab2f2f; }
    body { margin:0; min-height:100vh; font-family:"Avenir Next","Segoe UI",sans-serif; color:var(--text); background:linear-gradient(160deg,#f8f3ea 0%,var(--bg) 100%); padding:20px; }
    .shell { width:min(1020px,100%); margin:0 auto; display:grid; gap:16px; }
    .panel { background:var(--panel); border:1px solid var(--line); border-radius:20px; padding:20px; }
    .layout { display:grid; grid-template-columns:minmax(280px,360px) minmax(0,1fr); gap:16px; }
    .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr)); gap:10px; }
    .pill { border:1px solid var(--line); border-radius:14px; padding:10px 12px; background:#fff; }
    label { display:block; margin-bottom:6px; color:var(--muted); }
    input, select { width:100%; padding:10px 12px; border-radius:10px; border:1px solid var(--line); margin-bottom:10px; }
    button { width:100%; border:0; border-radius:10px; padding:11px 14px; font:inherit; font-weight:700; cursor:pointer; margin-bottom:8px; }
    .primary { background:linear-gradient(135deg,var(--accent),#1f936e); color:#fff; }
    .secondary { background:linear-gradient(135deg,var(--danger),#d85656); color:#fff; }
    .neutral { background:linear-gradient(135deg,#8d5f18,#bd8528); color:#fff; }
    .logbox { background:#201b17; color:#e8f3ec; border-radius:14px; padding:14px; min-height:320px; max-height:60vh; overflow:auto; font-family:"SFMono-Regular","Consolas",monospace; font-size:0.9rem; }
    .tiny { color:var(--muted); font-size:0.9rem; }
    .link { color:#0f5bbf; font-weight:700; text-decoration:none; }
    .config-title { display:flex; align-items:center; gap:10px; }
    .config-title-icon { width:42px; height:42px; flex:0 0 auto; overflow:visible; }
    @media (max-width: 860px) { .layout { grid-template-columns:1fr; } }
  </style>
</head>
<body>
  <main class="shell">
    <section class="panel">
      <div class="config-title">
        <svg class="config-title-icon" viewBox="0 0 64 64" aria-hidden="true">
          <image href=")HTML";
  page += kLedLogicIconUrl;
  page += R"HTML(" x="-2" y="-2" width="68" height="68" preserveAspectRatio="xMidYMid slice"/>
        </svg>
        <h1>Konfigurationsseite</h1>
      </div>
      <p class="tiny">Alle WLAN-, Debug- und OTA-Funktionen wurden hier gebuendelt. Der Normalbetrieb (LED-Steuerung) ist auf der Startseite.</p>
      <p><a class="link" href="/">Zurueck zum Normalbetrieb</a></p>
      <div class="grid">
        <div class="pill"><strong>WLAN-Status</strong><br><span id="wifi-status"></span></div>
        <div class="pill"><strong>Betriebsmodus</strong><br><span id="operation-mode"></span></div>
        <div class="pill"><strong>Aktuelle SSID</strong><br><span id="ssid-value"></span></div>
        <div class="pill"><strong>Station-IP</strong><br><span id="station-ip"></span></div>
        <div class="pill"><strong>Portal-IP</strong><br><span id="portal-ip"></span></div>
      </div>
        <p class="tiny">Version: V )HTML";
  page += htmlEscape(firmwareVersion);
  page += R"HTML(</p>
    </section>

    <section class="layout">
      <article class="panel">
        <h2>WLAN</h2>
        <button class="neutral" type="button" id="scan-trigger">SSID scannen</button>
        <label for="scan-results">Gefundene Netzwerke</label>
        <select id="scan-results"><option value="">Noch kein Scan ausgefuehrt</option></select>

        <form method="post" action="/save">
          <label for="ssid">SSID</label>
          <input id="ssid" name="ssid" maxlength="32" required value=")HTML";
  page += '"';
  page += htmlEscape(configuredSsid);
  page += R"HTML(">
          <label for="password">Passwort</label>
          <input id="password" name="password" type="password" maxlength="64" value=")HTML";
  page += '"';
  page += htmlEscape(configuredPassword);
  page += R"HTML(">
          <label><input id="show-password" type="checkbox"> Passwort im Klartext anzeigen</label>
          <button class="primary" type="submit">Speichern und verbinden</button>
        </form>

        <form method="post" action="/clear">
          <button class="secondary" type="submit">WLAN-Konfiguration loeschen</button>
        </form>

        <p class="tiny">AP SSID: <strong>)HTML";
  page += htmlEscape(kAccessPointSsid);
  page += R"HTML(</strong><br>AP Passwort: <strong>)HTML";
  page += htmlEscape(kAccessPointPassword);
  page += R"HTML(</strong></p>
      </article>

      <article class="panel">
        <h2>OTA Update</h2>
        <form method="get" action="/ota/check">
          <label for="check-url">GitHub Metadata URL (JSON oder Text)</label>
          <input id="check-url" name="url" placeholder="https://.../latest.json oder version.txt">
          <button class="neutral" type="submit">Version pruefen</button>
        </form>

        <form method="post" action="/ota/update_url">
          <label for="bin-url">Direkter Firmware-URL (.bin, z. B. GitHub Release Asset)</label>
          <input id="bin-url" name="url" placeholder="https://.../firmware.bin">
          <button class="primary" type="submit">Update von URL starten</button>
        </form>

        <form method="post" action="/ota/upload" enctype="multipart/form-data">
          <label for="upload-file">Firmware Upload (bin/zip)</label>
          <input id="upload-file" type="file" name="firmware" accept=".bin,.zip,.bin.gz" required>
          <button class="neutral" type="submit">Upload und installieren</button>
        </form>
        <p class="tiny">Letztes OTA Ergebnis: )HTML";
  page += htmlEscape(otaUploadResult.isEmpty() ? String("-") : otaUploadResult);
  page += R"HTML(</p>
      </article>
    </section>

    <section class="panel">
      <h2>Debug-Ausgabe</h2>
      <div class="logbox" id="logbox">)HTML";
  page += collectLogsAsHtml();
  page += R"HTML(</div>
      <p class="tiny">Aktualisierung alle 3 Sekunden.</p>
    </section>
  </main>

  <script>
    function escapeHtml(entry) {
      return entry.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
    }

    function setupPasswordToggle() {
      const passwordInput = document.getElementById('password');
      const showPassword = document.getElementById('show-password');
      showPassword.addEventListener('change', () => {
        passwordInput.type = showPassword.checked ? 'text' : 'password';
      });
    }

    async function refreshScan() {
      const select = document.getElementById('scan-results');
      const ssidInput = document.getElementById('ssid');
      select.innerHTML = '<option value="">Scan laeuft...</option>';
      try {
        const response = await fetch('/scan');
        const networks = await response.json();
        if (!Array.isArray(networks) || networks.length === 0) {
          select.innerHTML = '<option value="">Keine Netzwerke gefunden</option>';
          return;
        }

        select.innerHTML = '<option value="">SSID aus Scan waehlen</option>';
        for (const network of networks) {
          const option = document.createElement('option');
          option.value = network.ssid;
          option.textContent = `${network.ssid} (${network.rssi} dBm${network.secure ? ', gesichert' : ', offen'})`;
          select.appendChild(option);
        }

        select.onchange = () => {
          ssidInput.value = select.value;
        };
      } catch (error) {
        select.innerHTML = '<option value="">Scan fehlgeschlagen</option>';
        console.log(error);
      }
    }

    async function refreshStatus() {
      try {
        const response = await fetch('/status');
        const data = await response.json();
        document.getElementById('wifi-status').textContent = data.wifiStatus;
        document.getElementById('operation-mode').textContent = data.operationMode;
        document.getElementById('ssid-value').textContent = data.ssid || '-';
        document.getElementById('station-ip').textContent = data.stationIp;
        document.getElementById('portal-ip').textContent = data.portalIp;
        document.getElementById('logbox').innerHTML = data.logs.map((entry) => `<div>${escapeHtml(entry)}</div>`).join('');
      } catch (error) {
        console.log(error);
      }
    }

    document.getElementById('scan-trigger').addEventListener('click', refreshScan);
    setupPasswordToggle();
    refreshStatus();
    setInterval(refreshStatus, 3000);
  </script>
</body>
</html>)HTML";

  return page;
}

void sendPortalRedirect() {
  webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.sendHeader("Expires", "-1");
  webServer.sendHeader("Location", kRedirectLocation, true);
  webServer.send(302, "text/plain", "Redirecting to captive portal config page");
}

void handleRoot() {
  webServer.send(200, "text/html", buildLogicPage());
}

void handleConfig() {
  webServer.send(200, "text/html", buildConfigPage());
}

void handleStatus() {
  String json = "{";
  json += "\"wifiStatus\":\"" + wifiStatusToText(WiFi.status()) + "\",";
  json += "\"operationMode\":\"" + operationModeText() + "\",";
  json += "\"version\":\"" + jsonEscape(firmwareVersion) + "\",";
  json += "\"ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : configuredSsid) + "\",";
  json += "\"stationIp\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("-")) + "\",";
  json += "\"portalIp\":\"" + (accessPointRunning ? WiFi.softAPIP().toString() : String("-")) + "\",";
  json += "\"logs\":" + collectLogsAsJson();
  json += "}";
  webServer.send(200, "application/json", json);
}

void handleScan() {
  webServer.send(200, "application/json", collectWifiNetworksAsJson());
}

void handleSave() {
  const String ssid = webServer.arg("ssid");
  const String password = webServer.arg("password");

  if (ssid.isEmpty()) {
    appendLog("Speichern ignoriert: leere SSID.");
    webServer.send(400, "text/plain", "SSID darf nicht leer sein.");
    return;
  }

  saveCredentials(ssid, password);
  beginWifiConnection();
  if (!accessPointRunning) {
    startAccessPoint();
  }
  sendPortalRedirect();
}

void handleClear() {
  clearCredentials();
  WiFi.disconnect(false, true);
  startAccessPoint();
  sendPortalRedirect();
}

void handleLedPreview() {
  populateLedConfigFromRequest(previewLedConfig, previewLedCount);
  resetAnimationStartTimes(previewAnimationStartMs);
  ledPreviewActive = true;
  lastLedPreviewMs = millis();
  applyLedState();
  webServer.send(204, "text/plain", "");
}

void handleLedPreviewClear() {
  if (ledPreviewActive) {
    ledPreviewActive = false;
    applyLedState();
  }
  webServer.send(204, "text/plain", "");
}

void handleLedSave() {
  populateLedConfigFromRequest(ledConfig, ledCount);
  resetAnimationStartTimes(ledAnimationStartMs);
  ledPreviewActive = false;

  saveLedConfig();
  applyLedState();
  appendLog("LED-Konfiguration gespeichert. Anzahl: " + String(ledCount));

  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "Saved");
}

void handleScriptRun() {
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "text/plain", "Body fehlt");
    return;
  }
  const String json = webServer.arg("plain");
  if (json.length() > kMaxScriptJsonLen) {
    webServer.send(413, "text/plain", "Script zu gross");
    return;
  }
  if (!parseAndRunScript(json)) {
    webServer.send(400, "text/plain", "Script ungueltig");
    return;
  }
  savedScriptJson = json;
  preferences.begin(kPreferencesNamespace, false);
  preferences.putString("script", savedScriptJson);
  preferences.end();
  appendLog("Script gestartet (" + String(scriptStepCount) + " Schritte).");
  webServer.send(204, "text/plain", "");
}

void handleScriptSave() {
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "text/plain", "Body fehlt");
    return;
  }
  const String json = webServer.arg("plain");
  if (json.length() > kMaxScriptJsonLen) {
    webServer.send(413, "text/plain", "Script zu gross");
    return;
  }
  if (!validateScriptJson(json)) {
    webServer.send(400, "text/plain", "Script ungueltig");
    return;
  }
  savedScriptJson = json;
  preferences.begin(kPreferencesNamespace, false);
  preferences.putString("script", savedScriptJson);
  preferences.end();
  appendLog("Script gespeichert.");
  webServer.send(204, "text/plain", "");
}

void handleScriptStop() {
  scriptRunning = false;
  scriptAllOff();
  ws2812Show();
  appendLog("Script angehalten.");
  webServer.send(204, "text/plain", "");
}

void handleScriptClear() {
  scriptRunning = false;
  scriptAllOff();
  ws2812Show();
  savedScriptJson = "";
  preferences.begin(kPreferencesNamespace, false);
  preferences.remove("script");
  preferences.end();
  appendLog("Script geloescht.");
  webServer.send(204, "text/plain", "");
}

void handleOtaCheck() {
  if (!webServer.hasArg("url")) {
    webServer.send(400, "application/json", "{\"error\":\"url fehlt\"}");
    return;
  }

  const String url = webServer.arg("url");
  if (WiFi.status() != WL_CONNECTED) {
    webServer.send(503, "application/json", "{\"error\":\"kein WLAN\"}");
    return;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) {
    webServer.send(500, "application/json", "{\"error\":\"http begin fehlgeschlagen\"}");
    return;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    const String msg = "{\"error\":\"http code " + String(code) + "\"}";
    http.end();
    webServer.send(502, "application/json", msg);
    return;
  }

  String payload = http.getString();
  http.end();
  payload.trim();

  String remoteVersion;
  String binUrl;

  if (payload.startsWith("{")) {
    remoteVersion = extractJsonStringField(payload, "version");
    binUrl = extractJsonStringField(payload, "bin_url");
  } else {
    const int newline = payload.indexOf('\n');
    if (newline >= 0) {
      remoteVersion = payload.substring(0, newline);
      binUrl = payload.substring(newline + 1);
      remoteVersion.trim();
      binUrl.trim();
    } else {
      remoteVersion = payload;
      remoteVersion.trim();
    }
  }

  const bool updateAvailable = !remoteVersion.isEmpty() && remoteVersion != firmwareVersion;

  String response = "{";
  response += "\"currentVersion\":\"" + firmwareVersion + "\",";
  response += "\"remoteVersion\":\"" + jsonEscape(remoteVersion) + "\",";
  response += "\"updateAvailable\":" + String(updateAvailable ? "true" : "false") + ",";
  response += "\"binUrl\":\"" + jsonEscape(binUrl) + "\"";
  response += "}";

  webServer.send(200, "application/json", response);
}

void handleOtaUpdateUrl() {
  if (!webServer.hasArg("url")) {
    webServer.send(400, "text/plain", "url fehlt");
    return;
  }

  String message;
  const bool ok = scheduleOtaFromUrl(webServer.arg("url"), message);
  appendLog("OTA URL: " + message);
  otaUploadResult = message;

  if (!ok) {
    webServer.send(500, "text/plain", message);
    return;
  }

  otaRebootPending = true;
  otaRebootAtMs = millis() + 1500;
  webServer.send(200, "text/plain", message);
}

void handleOtaUploadFinish() {
  if (Update.hasError()) {
    otaUploadResult = "Upload fehlgeschlagen: " + String(Update.errorString());
    appendLog("OTA Upload Fehler: " + otaUploadResult);
    webServer.send(500, "text/plain", otaUploadResult);
    return;
  }

  otaUploadResult = "Upload erfolgreich, Neustart wird ausgefuehrt.";
  appendLog("OTA Upload erfolgreich.");
  otaRebootPending = true;
  otaRebootAtMs = millis() + 1500;
  webServer.send(200, "text/plain", otaUploadResult);
}

void handleOtaUploadData() {
  HTTPUpload& upload = webServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    appendLog("OTA Upload gestartet: " + upload.filename);
    otaUploadResult = "Upload laeuft";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaUploadResult = "Update.begin fehlgeschlagen: " + String(Update.errorString());
      appendLog(otaUploadResult);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!Update.hasError()) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        otaUploadResult = "Update.write fehlgeschlagen: " + String(Update.errorString());
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      otaUploadResult = "Update.end fehlgeschlagen: " + String(Update.errorString());
      appendLog(otaUploadResult);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaUploadResult = "Upload abgebrochen";
    appendLog(otaUploadResult);
  }
}

void handleIconPng() {
  webServer.sendHeader("Cache-Control", "public, max-age=86400");
  webServer.send_P(200, "image/png", reinterpret_cast<const char*>(led_logic_icon_png), led_logic_icon_png_len);
}

void handleCaptiveProbe() {
  sendPortalRedirect();
}

void handleNotFound() {
  if (accessPointRunning) {
    sendPortalRedirect();
    return;
  }

  webServer.send(404, "text/plain", "Not found");
}

void configureWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/config", HTTP_GET, handleConfig);
  webServer.on(kLedLogicIconUrl, HTTP_GET, handleIconPng);
  webServer.on("/script/save", HTTP_POST, handleScriptSave);
  webServer.on("/script/run", HTTP_POST, handleScriptRun);
  webServer.on("/script/stop", HTTP_POST, handleScriptStop);
  webServer.on("/script/clear", HTTP_POST, handleScriptClear);
  webServer.on("/status", HTTP_GET, handleStatus);
  webServer.on("/scan", HTTP_GET, handleScan);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/clear", HTTP_POST, handleClear);
  webServer.on("/led/preview", HTTP_POST, handleLedPreview);
  webServer.on("/led/preview/clear", HTTP_POST, handleLedPreviewClear);
  webServer.on("/led/save", HTTP_POST, handleLedSave);
  webServer.on("/ota/check", HTTP_GET, handleOtaCheck);
  webServer.on("/ota/update_url", HTTP_POST, handleOtaUpdateUrl);
  webServer.on("/ota/upload", HTTP_POST, handleOtaUploadFinish, handleOtaUploadData);

  webServer.on("/generate_204", HTTP_ANY, handleCaptiveProbe);
  webServer.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveProbe);
  webServer.on("/connecttest.txt", HTTP_ANY, handleCaptiveProbe);
  webServer.on("/redirect", HTTP_ANY, handleCaptiveProbe);
  webServer.on("/fwlink", HTTP_ANY, handleCaptiveProbe);
  webServer.on("/ncsi.txt", HTTP_ANY, handleCaptiveProbe);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
}

void maintainWifiState() {
  const wl_status_t status = WiFi.status();
  const unsigned long now = millis();

  if (status == WL_CONNECTED) {
    if (wifiConnectInProgress || !wifiConnectedLogged) {
      appendLog("WLAN verbunden. IP: " + WiFi.localIP().toString());
    }
    wifiConnectInProgress = false;
    wifiConnectedLogged = true;
    stopAccessPoint();
    return;
  }

  wifiConnectedLogged = false;

  if (!configuredSsid.isEmpty()) {
    if (!accessPointRunning) {
      startAccessPoint();
    }

    if (wifiConnectInProgress && now - wifiAttemptStartedMs > kWifiConnectGraceMs) {
      wifiConnectInProgress = false;
      appendLog("WLAN-Verbindungsversuch beendet mit Status: " + wifiStatusToText(status));
    }

    if (!wifiConnectInProgress && now - lastWifiAttemptMs >= kWifiRetryIntervalMs) {
      beginWifiConnection();
    }
    return;
  }

  startAccessPoint();
}
}  // namespace

void setup() {
  firmwareVersion = buildCompileVersion();
  Serial.begin(115200);
  delay(400);
  appendLog("ESP32 startet.");
  appendLog("Firmware-Version: " + firmwareVersion);

  initWs2812Rmt();
  // Mehrfach senden, damit beim Boot sicher ein OFF-Frame anliegt.
  for (uint8_t i = 0; i < 3; ++i) {
    ws2812Show();
    delay(5);
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.onEvent(handleWifiEvent);
  WiFi.setSleep(false);

  loadCredentials();
  loadLedConfig();
  resetAnimationStartTimes(ledAnimationStartMs);
  resetAnimationStartTimes(previewAnimationStartMs);
  applyLedState();
  configureWebServer();
  startAccessPoint();

  if (!configuredSsid.isEmpty()) {
    beginWifiConnection();
  }
}

void loop() {
  const unsigned long now = millis();

  if (accessPointRunning) {
    dnsServer.processNextRequest();
  }
  webServer.handleClient();
  maintainWifiState();

  if (ledPreviewActive && now - lastLedPreviewMs > kLedPreviewTimeoutMs) {
    ledPreviewActive = false;
    applyLedState();
    appendLog("LED-Vorschau beendet.");
  }

  if ((ledPreviewActive || hasAnimatedLeds(activeLedConfig(), activeLedCount())) &&
      now - lastLedRenderMs >= kLedAnimationFrameMs) {
    lastLedRenderMs = now;
    applyLedState();
  }

  if (otaRebootPending && now > otaRebootAtMs) {
    appendLog("Neustart nach OTA.");
    delay(80);
    ESP.restart();
  }
}
