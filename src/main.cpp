#include <Arduino.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/rmt.h>
#include "build_version_override.h"
#include "assets/icons/led_icon_75x75_png.h"
#include "assets/icons/favicon_ico.h"
#include "assets/icons/save_script_16_png.h"

namespace {
constexpr char kAccessPointSsid[] = "ESP32-Config";
constexpr char kAccessPointPassword[] = "configureme";
constexpr char kDeviceHostname[] = "150_195_ESP32LEDLogic";
constexpr byte kDnsPort = 53;
constexpr unsigned long kStatusLogCapacity = 80;
constexpr unsigned long kWifiRetryIntervalMs = 15000;
constexpr unsigned long kWifiConnectGraceMs = 10000;
constexpr char kPreferencesNamespace[] = "device-config";
constexpr char kLedDefaultsVersionKey[] = "ledDefV";
constexpr uint8_t kLedDefaultsVersion = 3;
constexpr char kRedirectLocation[] = "http://192.168.4.1/config";
constexpr char kLedLogicIconUrl[] = "/assets/icons/led_icon_75x75.png";
constexpr char kLedLogicFaviconUrl[] = "/favicon.ico";
constexpr char kSetupSaveIconUrl[] = "/assets/icons/save_script_16.png";
constexpr char kDefaultOtaCheckUrl[] = "https://raw.githubusercontent.com/HJS72/LEDLogic/main/ota/latest.json";
constexpr char kDefaultOtaBinUrl[] = "https://raw.githubusercontent.com/HJS72/LEDLogic/main/firmware/firmware.bin";
constexpr uint8_t kLedPin = 5;
constexpr uint8_t kLedMinCount = 1;
constexpr uint8_t kLedMaxCount = 12;
constexpr unsigned long kLedPreviewTimeoutMs = 30000;
constexpr unsigned long kLedAnimationFrameMs = 40;
constexpr uint8_t kMaxScriptSteps = 48;
constexpr size_t kMaxScriptJsonLen = 4096;
constexpr char kScriptDir[] = "/scripts";
constexpr uint8_t kScriptMaxNameLen = 32;
constexpr uint8_t kMaxSavedScripts = 20;
constexpr uint8_t kMaxVariables = 16;

// ---- Script system ----
enum class ScriptOpType : uint8_t { 
  kSet = 0, 
  kWait = 1, 
  kFade = 2, 
  kAllOff = 3, 
  kBrightness = 4,
  // V2: Variable operations
  kSetVariable = 5,      // var_name = value
  kChangeVariable = 6    // var_name += value (or -=, *=, etc)
};

enum class VarType : uint8_t { 
  kLedMask = 0,          // bitmask 0-4095
  kBrightness = 1,       // 0-255
  kDuration = 2,         // milliseconds 0-65535
  kColor = 3             // RGB color
};

struct Variable {
  char name[16];         // variable identifier
  VarType type;
  uint16_t value;        // for ledMask, brightness, duration
  uint8_t r, g, b;       // only used for kColor type
};

struct ScriptStep {
  ScriptOpType op;
  uint16_t ledMask;      // bitmask for LED selection
  uint8_t r1, g1, b1;
  uint8_t r2, g2, b2;
  uint8_t brightness;    // 0-255
  uint32_t durationMs;
  
  // V2: Variable operation fields
  char varName[16];
  VarType varType;
  uint8_t varIndex;      // index into variables array
  uint8_t operation;     // 0=set, 1=add, 2=subtract, 3=multiply
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

// V2: Variables runtime state
Variable scriptVariables[kMaxVariables];
uint8_t scriptVariableCount = 0;

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
void loadPersistedVariables();
void persistVariables();
String jsonEscape(const String& input);
static uint16_t parseLedMaskValue(const String& rawValue, const uint16_t defaultMask);

static const char* varTypeToKey(const VarType type) {
  switch (type) {
    case VarType::kLedMask:
      return "led_mask";
    case VarType::kBrightness:
      return "brightness";
    case VarType::kDuration:
      return "duration";
    case VarType::kColor:
      return "color";
  }
  return "duration";
}

static bool parseVarType(const String& raw, VarType& type) {
  if (raw == "led_mask") {
    type = VarType::kLedMask;
    return true;
  }
  if (raw == "brightness") {
    type = VarType::kBrightness;
    return true;
  }
  if (raw == "duration") {
    type = VarType::kDuration;
    return true;
  }
  if (raw == "color") {
    type = VarType::kColor;
    return true;
  }
  return false;
}

static bool isValidVariableName(const String& rawName) {
  if (rawName.isEmpty() || rawName.length() >= 16) {
    return false;
  }
  if (!isAlpha(rawName.charAt(0)) && rawName.charAt(0) != '_') {
    return false;
  }
  for (size_t i = 1; i < rawName.length(); ++i) {
    const char ch = rawName.charAt(i);
    if (!isAlphaNumeric(ch) && ch != '_') {
      return false;
    }
  }
  return true;
}

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
#ifdef BUILD_VERSION_OVERRIDE
  return String(BUILD_VERSION_OVERRIDE);
#else
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
#endif
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

static uint16_t parseLedMaskFromCsv(const String& csv) {
  uint16_t mask = 0;
  int start = 0;
  while (start < static_cast<int>(csv.length())) {
    int end = csv.indexOf(',', start);
    if (end < 0) {
      end = csv.length();
    }
    String token = csv.substring(start, end);
    token.trim();
    if (!token.isEmpty()) {
      const int led = constrain(token.toInt(), 0, kLedMaxCount - 1);
      mask |= static_cast<uint16_t>(1U << led);
    }
    start = end + 1;
  }
  return mask;
}

static uint16_t parseLedMaskFromOp(const String& opJson, const uint16_t defaultMask) {
  const String ledsCsv = extractJsonStr(opJson, "leds");
  if (!ledsCsv.isEmpty()) {
    const uint16_t resolvedMask = parseLedMaskValue(ledsCsv, 0);
    if (resolvedMask != 0) {
      return resolvedMask;
    }
  }

  // Backward compatibility with older scripts that stored only one LED index.
  const String ledField = extractJsonStr(opJson, "led");
  if (!ledField.isEmpty()) {
    const int led = constrain(ledField.toInt(), 0, kLedMaxCount - 1);
    return static_cast<uint16_t>(1U << led);
  }

  return defaultMask;
}

static uint16_t allLedMask() {
  return static_cast<uint16_t>((1UL << kLedMaxCount) - 1U);
}

static int findVariableIndex(const String& varName) {
  for (uint8_t i = 0; i < scriptVariableCount; ++i) {
    if (String(scriptVariables[i].name) == varName) {
      return i;
    }
  }
  return -1;
}

static bool createOrUpdateVariable(const String& varName, VarType type, uint16_t value, 
                                    uint8_t r = 0, uint8_t g = 0, uint8_t b = 0) {
  int idx = findVariableIndex(varName);
  if (idx >= 0) {
    // Update existing
    scriptVariables[idx].type = type;
    scriptVariables[idx].value = value;
    scriptVariables[idx].r = r;
    scriptVariables[idx].g = g;
    scriptVariables[idx].b = b;
    return true;
  }
  
  // Create new if space available
  if (scriptVariableCount < kMaxVariables) {
    idx = scriptVariableCount++;
    strncpy(scriptVariables[idx].name, varName.c_str(), sizeof(scriptVariables[idx].name) - 1);
    scriptVariables[idx].name[sizeof(scriptVariables[idx].name) - 1] = '\0';
    scriptVariables[idx].type = type;
    scriptVariables[idx].value = value;
    scriptVariables[idx].r = r;
    scriptVariables[idx].g = g;
    scriptVariables[idx].b = b;
    return true;
  }
  return false;
}

static bool deleteVariable(const String& varName) {
  const int idx = findVariableIndex(varName);
  if (idx < 0) {
    return false;
  }
  for (uint8_t i = idx; i + 1 < scriptVariableCount; ++i) {
    scriptVariables[i] = scriptVariables[i + 1];
  }
  --scriptVariableCount;
  memset(&scriptVariables[scriptVariableCount], 0, sizeof(Variable));
  return true;
}

static bool getVariable(const String& varName, Variable& outVar) {
  const int idx = findVariableIndex(varName);
  if (idx < 0) {
    return false;
  }
  outVar = scriptVariables[idx];
  return true;
}

static uint16_t getVariableValue(const String& varName, uint16_t defaultValue = 0) {
  int idx = findVariableIndex(varName);
  if (idx >= 0) {
    return scriptVariables[idx].value;
  }
  return defaultValue;
}

static void clearVariables() {
  scriptVariableCount = 0;
  memset(scriptVariables, 0, sizeof(scriptVariables));
}

static String variableJsonObject(const Variable& var) {
  String json = "{";
  json += "\"name\":\"" + jsonEscape(String(var.name)) + "\",";
  json += "\"type\":\"" + String(varTypeToKey(var.type)) + "\",";
  json += "\"value\":" + String(var.value) + ",";
  json += "\"hex\":\"#";
  char hexBuf[7];
  snprintf(hexBuf, sizeof(hexBuf), "%02x%02x%02x", var.r, var.g, var.b);
  json += String(hexBuf);
  json += "\",";
  json += "\"r\":" + String(var.r) + ",";
  json += "\"g\":" + String(var.g) + ",";
  json += "\"b\":" + String(var.b);
  json += "}";
  return json;
}

static String collectVariablesAsJson() {
  String json = "[";
  for (uint8_t i = 0; i < scriptVariableCount; ++i) {
    if (i > 0) {
      json += ",";
    }
    json += variableJsonObject(scriptVariables[i]);
  }
  json += "]";
  return json;
}

void persistVariables() {
  preferences.begin(kPreferencesNamespace, false);
  preferences.putString("vars", collectVariablesAsJson());
  preferences.end();
}

static bool parseAndStoreVariableObject(const String& varJson) {
  const String varName = extractJsonStr(varJson, "name");
  const String varTypeStr = extractJsonStr(varJson, "type");
  const String varValueStr = extractJsonStr(varJson, "value");

  if (!isValidVariableName(varName)) {
    return false;
  }

  VarType varType = VarType::kDuration;
  if (!parseVarType(varTypeStr, varType)) {
    return false;
  }

  uint16_t value = 0;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  if (varType == VarType::kColor) {
    if (!parseHexColor(varValueStr, r, g, b)) {
      const String hexField = extractJsonStr(varJson, "hex");
      if (!parseHexColor(hexField, r, g, b)) {
        return false;
      }
    }
  } else {
    value = static_cast<uint16_t>(constrain(varValueStr.toInt(), 0, 65535));
    if (varType == VarType::kBrightness && value > 255) {
      value = 255;
    }
    if (varType == VarType::kLedMask) {
      value &= allLedMask();
    }
  }

  return createOrUpdateVariable(varName, varType, value, r, g, b);
}

static void parseVariablesArrayIntoStore(const String& arrayJson) {
  int pos = 0;
  const int jsonLen = arrayJson.length();
  while (pos < jsonLen && scriptVariableCount < kMaxVariables) {
    const int varStart = arrayJson.indexOf('{', pos);
    if (varStart < 0) {
      break;
    }
    const int varEnd = arrayJson.indexOf('}', varStart);
    if (varEnd < 0) {
      break;
    }
    parseAndStoreVariableObject(arrayJson.substring(varStart, varEnd + 1));
    pos = varEnd + 1;
  }
}

void loadPersistedVariables() {
  clearVariables();
  preferences.begin(kPreferencesNamespace, true);
  const String stored = preferences.getString("vars", "[]");
  preferences.end();
  parseVariablesArrayIntoStore(stored);
}

static bool resolveVariableReference(const String& rawValue, Variable& outVar) {
  if (!rawValue.startsWith("$")) {
    return false;
  }
  const String varName = rawValue.substring(1);
  return getVariable(varName, outVar);
}

static uint16_t parseLedMaskValue(const String& rawValue, const uint16_t defaultMask) {
  if (rawValue.isEmpty()) {
    return defaultMask;
  }

  Variable var = {};
  if (resolveVariableReference(rawValue, var) && var.type == VarType::kLedMask) {
    return static_cast<uint16_t>(var.value & allLedMask());
  }

  const uint16_t csvMask = parseLedMaskFromCsv(rawValue);
  return csvMask == 0 ? defaultMask : csvMask;
}

static uint8_t parseBrightnessValue(const String& rawValue) {
  Variable var = {};
  if (resolveVariableReference(rawValue, var) && var.type == VarType::kBrightness) {
    return static_cast<uint8_t>(constrain(var.value, 0, 255));
  }
  const int brPercent = rawValue.toInt();
  return static_cast<uint8_t>(constrain(brPercent * 255 / 100, 0, 255));
}

static uint32_t parseDurationValueMs(const String& rawValue) {
  Variable var = {};
  if (resolveVariableReference(rawValue, var) && var.type == VarType::kDuration) {
    return var.value;
  }
  return static_cast<uint32_t>(rawValue.toFloat() * 1000.0f);
}

static bool parseColorValue(const String& rawValue, uint8_t& r, uint8_t& g, uint8_t& b) {
  Variable var = {};
  if (resolveVariableReference(rawValue, var) && var.type == VarType::kColor) {
    r = var.r;
    g = var.g;
    b = var.b;
    return true;
  }
  return parseHexColor(rawValue, r, g, b);
}

static void copyVariableName(char* target, const String& value) {
  strncpy(target, value.c_str(), 15);
  target[15] = '\0';
}

static void scriptAllOff() {
  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    scriptEnabled[i] = false;
    scriptR[i] = scriptG[i] = scriptB[i] = scriptBr[i] = 0;
  }
}

static void scriptAllOffMask(const uint16_t ledMask) {
  for (uint8_t i = 0; i < kLedMaxCount; ++i) {
    if ((ledMask & static_cast<uint16_t>(1U << i)) == 0) {
      continue;
    }
    scriptEnabled[i] = false;
    scriptR[i] = scriptG[i] = scriptB[i] = scriptBr[i] = 0;
  }
}

bool parseAndRunScript(const String& json) {
  loadPersistedVariables();

  const int varsStart = json.indexOf("\"vars\"");
  if (varsStart >= 0) {
    const int varsArrayStart = json.indexOf('[', varsStart);
    const int varsArrayEnd = json.indexOf(']', varsArrayStart);
    if (varsArrayStart >= 0 && varsArrayEnd > varsArrayStart) {
      parseVariablesArrayIntoStore(json.substring(varsArrayStart, varsArrayEnd + 1));
    }
  }

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
      step.ledMask = parseLedMaskFromOp(opJson, static_cast<uint16_t>(1U));
      valid = parseColorValue(extractJsonStr(opJson, "color"), step.r1, step.g1, step.b1);
      step.brightness = parseBrightnessValue(extractJsonStr(opJson, "br"));
    } else if (opType == "wait") {
      step.op = ScriptOpType::kWait;
      step.durationMs = parseDurationValueMs(extractJsonStr(opJson, "s"));
    } else if (opType == "brightness") {
      step.op = ScriptOpType::kBrightness;
      step.ledMask = parseLedMaskFromOp(opJson, static_cast<uint16_t>(1U));
      step.brightness = parseBrightnessValue(extractJsonStr(opJson, "br"));
    } else if (opType == "fade") {
      step.op = ScriptOpType::kFade;
      step.ledMask = parseLedMaskFromOp(opJson, static_cast<uint16_t>(1U));
      valid = parseColorValue(extractJsonStr(opJson, "from"), step.r1, step.g1, step.b1) &&
              parseColorValue(extractJsonStr(opJson, "to"), step.r2, step.g2, step.b2);
      step.durationMs = parseDurationValueMs(extractJsonStr(opJson, "s"));
      step.brightness = parseBrightnessValue(extractJsonStr(opJson, "br"));
    } else if (opType == "all_off") {
      step.op = ScriptOpType::kAllOff;
      step.ledMask = parseLedMaskFromOp(opJson, allLedMask());
    } else if (opType == "set_var") {
      const String varName = extractJsonStr(opJson, "name");
      const String varTypeRaw = extractJsonStr(opJson, "type");
      step.op = ScriptOpType::kSetVariable;
      step.operation = 0;
      valid = isValidVariableName(varName) && parseVarType(varTypeRaw, step.varType);
      if (valid) {
        copyVariableName(step.varName, varName);
        if (step.varType == VarType::kColor) {
          valid = parseColorValue(extractJsonStr(opJson, "value"), step.r1, step.g1, step.b1);
        } else {
          step.durationMs = constrain(extractJsonStr(opJson, "value").toInt(), 0, 65535);
        }
      }
    } else if (opType == "change_var") {
      const String varName = extractJsonStr(opJson, "name");
      step.op = ScriptOpType::kChangeVariable;
      valid = isValidVariableName(varName);
      if (valid) {
        copyVariableName(step.varName, varName);
      }
      const String opTypeStr = extractJsonStr(opJson, "op_type");
      if (opTypeStr == "add") step.operation = 1;
      else if (opTypeStr == "subtract") step.operation = 2;
      else if (opTypeStr == "multiply") step.operation = 3;
      else step.operation = 0;  // default to set
      step.durationMs = constrain(extractJsonStr(opJson, "value").toInt(), 0, 65535);
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
    for (uint8_t led = 0; led < kLedMaxCount; ++led) {
      if ((first.ledMask & static_cast<uint16_t>(1U << led)) == 0) {
        continue;
      }
      scriptEnabled[led] = true;
      scriptR[led] = first.r1;
      scriptG[led] = first.g1;
      scriptB[led] = first.b1;
      scriptBr[led] = first.brightness;
    }
    ws2812Show();
  } else if (first.op == ScriptOpType::kAllOff) {
    scriptAllOffMask(first.ledMask);
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
               opType == "fade" || opType == "all_off" || opType == "set_var" ||
               opType == "change_var";
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
      for (uint8_t led = 0; led < kLedMaxCount; ++led) {
        if ((step.ledMask & static_cast<uint16_t>(1U << led)) == 0) {
          continue;
        }
        scriptEnabled[led] = true;
        scriptR[led] = step.r1;
        scriptG[led] = step.g1;
        scriptB[led] = step.b1;
        scriptBr[led] = step.brightness;
      }
      ws2812Show();
      advance = true;
      break;

    case ScriptOpType::kAllOff:
      scriptAllOffMask(step.ledMask);
      ws2812Show();
      advance = true;
      break;

    case ScriptOpType::kWait:
      if (elapsed >= step.durationMs) {
        advance = true;
      }
      break;

    case ScriptOpType::kBrightness:
      for (uint8_t led = 0; led < kLedMaxCount; ++led) {
        if ((step.ledMask & static_cast<uint16_t>(1U << led)) == 0) {
          continue;
        }
        scriptEnabled[led] = true;
        scriptBr[led] = step.brightness;
      }
      ws2812Show();
      advance = true;
      break;

    case ScriptOpType::kFade:
      if (step.durationMs == 0 || elapsed >= step.durationMs) {
        for (uint8_t led = 0; led < kLedMaxCount; ++led) {
          if ((step.ledMask & static_cast<uint16_t>(1U << led)) == 0) {
            continue;
          }
          scriptEnabled[led] = true;
          scriptR[led] = step.r2;
          scriptG[led] = step.g2;
          scriptB[led] = step.b2;
          scriptBr[led] = step.brightness;
        }
        ws2812Show();
        advance = true;
      } else {
        const uint32_t t256 = (elapsed * 256UL) / step.durationMs;
        for (uint8_t led = 0; led < kLedMaxCount; ++led) {
          if ((step.ledMask & static_cast<uint16_t>(1U << led)) == 0) {
            continue;
          }
          scriptEnabled[led] = true;
          scriptR[led] = static_cast<uint8_t>((step.r1 * (256 - t256) + step.r2 * t256) >> 8);
          scriptG[led] = static_cast<uint8_t>((step.g1 * (256 - t256) + step.g2 * t256) >> 8);
          scriptB[led] = static_cast<uint8_t>((step.b1 * (256 - t256) + step.b2 * t256) >> 8);
          scriptBr[led] = step.brightness;
        }
        ws2812Show();
      }
      break;

    case ScriptOpType::kSetVariable: {
      const String varName(step.varName);
      if (step.varType == VarType::kColor) {
        createOrUpdateVariable(varName, step.varType, 0, step.r1, step.g1, step.b1);
      } else {
        uint16_t numericValue = static_cast<uint16_t>(step.durationMs);
        if (step.varType == VarType::kBrightness && numericValue > 255) {
          numericValue = 255;
        }
        if (step.varType == VarType::kLedMask) {
          numericValue &= allLedMask();
        }
        createOrUpdateVariable(varName, step.varType, numericValue);
      }
      advance = true;
      break;
    }

    case ScriptOpType::kChangeVariable: {
      const String varName(step.varName);
      Variable var = {};
      if (getVariable(varName, var) && var.type != VarType::kColor) {
        long nextValue = var.value;
        if (step.operation == 1) {
          nextValue += static_cast<long>(step.durationMs);
        } else if (step.operation == 2) {
          nextValue -= static_cast<long>(step.durationMs);
        } else if (step.operation == 3) {
          nextValue *= static_cast<long>(step.durationMs);
        } else {
          nextValue = static_cast<long>(step.durationMs);
        }

        long maxValue = 65535;
        if (var.type == VarType::kBrightness) {
          maxValue = 255;
        }
        if (var.type == VarType::kLedMask) {
          maxValue = allLedMask();
        }
        nextValue = constrain(nextValue, 0L, maxValue);
        createOrUpdateVariable(varName, var.type, static_cast<uint16_t>(nextValue), var.r, var.g, var.b);
      }
      advance = true;
      break;
    }
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

  // Migration: Für bestehende Installationen einmalig auf "alle LEDs aus" setzen.
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
  loadPersistedVariables();
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
  appendLog("Gespeicherte WLAN-Konfiguration wurde gelöscht.");
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

String normalizeVersionForCompare(String version) {
  version.trim();
  if (!version.isEmpty() && (version.charAt(0) == 'v' || version.charAt(0) == 'V')) {
    version.remove(0, 1);
    version.trim();
  }
  version.toLowerCase();
  return version;
}

int compareVersionsAlphabetical(const String& leftRaw, const String& rightRaw) {
  const String left = normalizeVersionForCompare(leftRaw);
  const String right = normalizeVersionForCompare(rightRaw);
  return left.compareTo(right);
}

bool scheduleOtaFromUrl(const String& url, String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    message = "OTA von URL nur mit aktiver WLAN-Verbindung möglich.";
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
    message = "Unvollständiger Download: " + String(written) + " von " + String(length);
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
    message = "Update nicht vollständig.";
    http.end();
    return false;
  }

  http.end();
  message = "OTA erfolgreich, Neustart wird ausgeführt.";
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

void sendLogicPageStreamed() {
  webServer.sendContent(R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LED-Logic</title>
  <link rel="icon" type="image/png" href=")HTML");
  webServer.sendContent(kLedLogicFaviconUrl);
  webServer.sendContent(R"HTML(">
  <style>
    :root { --bg:#f3eeff; --panel:#ffffff; --line:#ddd0f0; --text:#1a1030; --accent:#9b3db8; --muted:#6d5a84; }
    body { margin:0; font-family:"Avenir Next","Segoe UI",sans-serif; background:linear-gradient(140deg,#e8f0ff,#f5e6ff); color:var(--text); padding:18px; }
    .wrap { margin:0 auto; display:grid; gap:6px; }
    .panel { background:var(--panel); border:1px solid var(--line); border-radius:20px; padding:20px; }
    h1,h2,h3 { margin-top:0; }
    .topbar { display:flex; align-items:center; gap:12px; flex-wrap:wrap; margin-bottom:4px; }
    .title-row { display:flex; align-items:center; gap:8px; }
    .title-icon { width:84px; height:84px; flex:0 0 auto; overflow:visible; }
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
    .logic-layout { display:grid; grid-template-columns:220px minmax(0,1fr); gap:14px; margin-top:12px; }
    .toolbox { border:1px solid var(--line); border-radius:14px; width:fit-content; max-width:100%; justify-self:start; background:#f8fbff; padding:8px; display:grid; gap:8px; align-content:start; }
    .toolbox-setup { display:grid; gap:8px; width:fit-content; max-width:100%; justify-self:start; padding:8px; border:1px solid rgba(149,169,200,0.24); border-radius:12px; background:linear-gradient(180deg,rgba(255,255,255,0.96),rgba(242,247,253,0.92)); }
    .toolbox-setup-title { font-size:0.74rem; font-weight:700; letter-spacing:0.03em; text-transform:uppercase; color:var(--muted); }
    .toolbox-setup-row { display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
      .toolbox-setup select { width:48px; min-width:48px; }
    .toolbox-setup button { padding:8px 12px; border-radius:10px; }
      .setup-save-icon { width:36px; height:36px; padding:0 !important; display:flex; align-items:center; justify-content:center; line-height:0; }
      .setup-save-icon img { width:24px; height:24px; display:block; object-fit:contain; }
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
    .block-set_variable, .tool-set_variable,
    .block-change_variable, .tool-change_variable { background:#eefbf5; border-left:6px solid #2f9d68; }
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
    .led-checklist { display:flex; align-items:center; flex-wrap:wrap; gap:6px; }
    .led-check { display:inline-flex; align-items:center; gap:4px; font-size:0.82rem; color:var(--text); padding:2px 6px; border:1px solid rgba(20,32,51,0.16); border-radius:999px; background:#ffffff; }
    .led-check input { margin:0; }
    .field-stack.compact { gap:6px; min-width:120px; }
    .field-stack.compact label { margin:0; }
    .variable-picker { min-width:118px; max-width:160px; }
    .field-inline input.var-direct-input { width:96px; }
    .number-wrap { display:flex; align-items:center; gap:6px; }
    .block-actions { margin-left:auto; display:flex; align-items:center; gap:4px; }
    .block-mini-btn { width:28px; height:28px; display:inline-flex; align-items:center; justify-content:center; border:1px solid rgba(20,32,51,0.18); border-radius:7px; background:#f4f7fc; color:#405070; padding:0; cursor:pointer; }
    .block-mini-btn svg { width:14px; height:14px; fill:none; stroke:currentColor; stroke-width:1.9; stroke-linecap:round; stroke-linejoin:round; }
    .block-mini-btn.copy { background:#edf6ff; color:#2f67c8; border-color:rgba(47,103,200,0.25); }
    .block-wait .block-mini-btn.copy { background:#fff7eb; color:#c4892b; border-color:rgba(196,137,43,0.3); }
    .block-repeat_start .block-mini-btn.copy,
    .block-repeat_end .block-mini-btn.copy { background:#f4efff; color:#6d4dc1; border-color:rgba(109,77,193,0.28); }
    .block-all_off .block-mini-btn.copy { background:#fff1f1; color:#d85a5a; border-color:rgba(216,90,90,0.3); }
    .block-mini-btn.remove { background:#fff1f1; color:#c0392b; border-color:rgba(192,57,43,0.24); }
    .status-bar { font-size:0.9rem; color:#000000; font-weight:400; padding:0; }
    .status-bar.running { color:#000000; font-weight:400; }
    .footer-debug-box { border:1px solid rgba(149,169,200,0.34); border-radius:14px; background:linear-gradient(180deg,rgba(255,255,255,0.95),rgba(242,247,253,0.92)); padding:10px 12px; }
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
    .color-hex-input { width:96px; padding:5px 7px; border:1px solid rgba(20,32,51,0.22); border-radius:8px; font-family:"SFMono-Regular","Consolas",monospace; font-size:0.78rem; text-transform:uppercase; }
    .color-hex-input.invalid { border-color:#d85a5a; box-shadow:0 0 0 1px rgba(216,90,90,0.2); }
    .overlay-backdrop { position:fixed; inset:0; background:rgba(20,32,51,0.52); display:none; align-items:center; justify-content:center; z-index:120; padding:16px; }
    .overlay-backdrop.open { display:flex; }
    .overlay-dialog { width:min(460px,100%); background:#ffffff; border:1px solid rgba(149,169,200,0.4); border-radius:16px; box-shadow:0 20px 46px rgba(20,32,51,0.34); padding:16px; display:grid; gap:10px; }
    .overlay-title { margin:0; font-size:1.05rem; }
    .overlay-note { margin:0; font-size:0.85rem; color:var(--muted); }
    .overlay-select { width:100%; min-height:180px; border:1px solid var(--line); border-radius:10px; padding:8px; font-size:0.92rem; }
    .overlay-actions { display:flex; gap:8px; justify-content:flex-end; flex-wrap:wrap; }
    .overlay-btn { padding:8px 12px; border-radius:10px; border:1px solid rgba(149,169,200,0.42); background:#eef5ff; color:#2f67c8; font-weight:700; cursor:pointer; }
    .overlay-btn.danger { background:#fff1f1; color:#ba3f45; border-color:rgba(216,90,90,0.35); }
    .overlay-btn.neutral { background:#f4f6fb; color:#4e5874; }
    @media (max-width: 980px) {
      .logic-layout { grid-template-columns:1fr; }
    }
  </style>
</head>
<body>
<main class="wrap">
  <div class="topbar">
    <details class="menu">
      <summary class="menu-button" aria-label="Menü"><span class="menu-icon">&#9776;</span></summary>
      <nav class="menu-list">
        <a href="/">LED-Logic</a>
        <a href="/config">Konfiguration</a>
      </nav>
    </details>
    <div class="title-row">
      <svg class="title-icon" viewBox="0 0 64 64" aria-hidden="true">
          <image href=")HTML");
        webServer.sendContent(kLedLogicIconUrl);
        webServer.sendContent(R"HTML(" x="-2" y="-2" width="68" height="68" preserveAspectRatio="xMidYMid slice"/>
        </svg>
          <h1 class="title">LED-Logic <span class="title-version">V )HTML");
        webServer.sendContent(firmwareVersion);
        webServer.sendContent(R"HTML(</span></h1>
    </div>
  </div>

  <section class="panel">
    <div class="command-bar">
      <div class="command-group">
        <button id="loadScriptBtn" class="toolbar-btn save momentary" onclick="openLoadScriptPopup()" title="Gespeichertes Script laden">
          <span class="toolbar-text">Laden</span>
          <span class="toolbar-icon-wrap">
            <svg class="toolbar-icon-svg" viewBox="0 0 24 24" aria-hidden="true">
              <path d="M12 5v10"/>
              <path d="M8.5 11.5 12 15l3.5-3.5"/>
              <path d="M6 18h12"/>
            </svg>
          </span>
        </button>
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
            <button type="button" class="secondary setup-save-icon" onclick="saveLedCount()" title="Übernehmen" aria-label="Übernehmen"><img src=")HTML");
  webServer.sendContent(kSetupSaveIconUrl);
  webServer.sendContent(R"HTML(" alt="Speichern"></button>
          </div>
        </div>
        <h3>Werkzeugleiste</h3>
        <div class="tool-item tool-set_color" draggable="true" data-tool="set_color">Farbe setzen</div>
        <div class="tool-item tool-set_brightness" draggable="true" data-tool="set_brightness">Helligkeit setzen</div>
        <div class="tool-item tool-fade" draggable="true" data-tool="fade">Überblenden</div>
        <div class="tool-item tool-wait" draggable="true" data-tool="wait">Warten</div>
        <div class="tool-item tool-set_variable" draggable="true" data-tool="set_variable">Variable setzen</div>
        <div class="tool-item tool-change_variable" draggable="true" data-tool="change_variable">Variable ändern</div>
        <div class="tool-item tool-repeat_start" draggable="true" data-tool="repeat">Wiederholen</div>
        <div class="tool-item tool-all_off" draggable="true" data-tool="all_off">Ausschalten</div>
      </aside>

      <section class="script-canvas" id="scriptCanvas">
        <div class="script-list" id="scriptList"></div>
      </section>
    </div>
  </section>

  <section class="panel footer-debug-box">
    <div class="status-bar" id="statusBar">Bereit.</div>
  </section>
</main>

<div id="loadScriptOverlay" class="overlay-backdrop" aria-hidden="true">
  <div class="overlay-dialog" role="dialog" aria-modal="true" aria-labelledby="loadScriptOverlayTitle">
    <h3 id="loadScriptOverlayTitle" class="overlay-title">Script laden</h3>
    <p id="loadScriptOverlayHint" class="overlay-note">Wähle ein gespeichertes Script.</p>
    <select id="loadScriptSelect" class="overlay-select" size="8"></select>
    <div class="overlay-actions">
      <button type="button" class="overlay-btn" onclick="loadSelectedScriptFromOverlay()">Laden</button>
      <button type="button" class="overlay-btn danger" onclick="deleteSelectedScriptFromOverlay()">Löschen</button>
      <button type="button" class="overlay-btn neutral" onclick="closeLoadScriptPopup()">Schließen</button>
    </div>
  </div>
</div>

<script>
  const MAX_LEDS = )HTML");
  webServer.sendContent(String(ledCount));
  webServer.sendContent(R"HTML(;
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
  let loadOverlayScriptNames = [];
  let currentVariables = [];

  function escapeHtml(value) {
    return String(value || '')
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;')
      .replaceAll("'", '&#39;');
  }

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

  function refreshLogicVariables() {
    fetch('/variables')
      .then(response => {
        if (!response.ok) {
          throw new Error('variables fetch failed');
        }
        return response.json();
      })
      .then(data => {
        currentVariables = Array.isArray(data.variables) ? data.variables : [];
        renderList();
      })
      .catch(() => {
        currentVariables = [];
      });
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
    const hexField = control.querySelector('.color-hex-input');
    const radius = canvas.width / 2;
    let dragging = false;

    drawColorWheel(canvas);

    function normalizeHex(value) {
      const trimmed = String(value || '').trim();
      const withHash = trimmed.startsWith('#') ? trimmed : ('#' + trimmed);
      if (!/^#[0-9a-fA-F]{6}$/.test(withHash)) {
        return null;
      }
      return withHash.toUpperCase();
    }

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
      hexField.value = hex;
      hexField.classList.remove('invalid');
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

    function applyHexFromText() {
      const parsed = normalizeHex(hexField.value);
      if (!parsed) {
        hexField.classList.add('invalid');
        return;
      }
      input.value = parsed;
      positionMarker(parsed);
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.dispatchEvent(new Event('change', { bubbles: true }));
    }

    hexField.addEventListener('input', () => {
      const parsed = normalizeHex(hexField.value);
      hexField.classList.toggle('invalid', !parsed);
    });

    hexField.addEventListener('keydown', event => {
      if (event.key === 'Enter') {
        event.preventDefault();
        applyHexFromText();
      }
    });

    hexField.addEventListener('blur', () => {
      const parsed = normalizeHex(hexField.value);
      if (!parsed) {
        positionMarker(input.value);
        return;
      }
      if (parsed !== input.value) {
        input.value = parsed;
        positionMarker(parsed);
        input.dispatchEvent(new Event('input', { bubbles: true }));
        input.dispatchEvent(new Event('change', { bubbles: true }));
      } else {
        positionMarker(parsed);
      }
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

  function defaultLedsCsv() {
    return Array.from({ length: MAX_LEDS }, (_, i) => String(i)).join(',');
  }

  function defaultValuesForType(type) {
    if (type === 'set_color') {
      return { leds: defaultLedsCsv(), color: '#FF0000', br: '100' };
    }
    if (type === 'set_brightness') {
      return { leds: defaultLedsCsv(), br: '100' };
    }
    if (type === 'fade') {
      return { leds: defaultLedsCsv(), from: '#FF0000', to: '#0000FF', br: '100', s: '1.0' };
    }
    if (type === 'wait') {
      return { s: '1.0' };
    }
    if (type === 'set_variable') {
      return { name: 'var_name', varType: 'brightness', value: '100' };
    }
    if (type === 'change_variable') {
      return { name: 'var_name', op_type: 'add', value: '10' };
    }
    if (type === 'repeat_start') {
      return { count: '2' };
    }
    if (type === 'all_off') {
      return { leds: defaultLedsCsv() };
    }
    return {};
  }

  function normalizeLedCsv(value) {
    const seen = new Set();
    const result = [];
    String(value || '')
      .split(',')
      .map(part => part.trim())
      .filter(part => part !== '')
      .forEach(part => {
        const index = Number.parseInt(part, 10);
        if (!Number.isInteger(index) || index < 0 || index >= MAX_LEDS || seen.has(index)) {
          return;
        }
        seen.add(index);
        result.push(index);
      });
    if (result.length === 0) {
      result.push(0);
    }
    return result;
  }

  function readSelectedLeds(el) {
    const selected = Array.from(el.querySelectorAll('input[type="checkbox"][data-k="leds"]:checked'))
      .map(input => Number.parseInt(input.value, 10));
    const valid = selected.filter(index => Number.isInteger(index) && index >= 0 && index < MAX_LEDS);
    if (valid.length > 0) {
      return valid;
    }
    return [0];
  }

  function ledCheckboxes(selectedCsv) {
    const selectedSet = new Set(normalizeLedCsv(selectedCsv));
    let options = '';
    for (let i = 0; i < MAX_LEDS; i += 1) {
      options += `<label class="led-check"><input type="checkbox" data-k="leds" value="${i}"${selectedSet.has(i) ? ' checked' : ''}><span>${i + 1}</span></label>`;
    }
    return options;
  }

  function buildWheelControl(key, value, labelText) {
    const label = labelText ? `<label>${labelText}</label>` : '';
    return `<div class="field-stack">${label}<div class="mini-wheel-control"><button type="button" class="color-chip-btn" style="background:${value};" title="Farbrad öffnen"></button><div class="mini-wheel-panel"><div class="mini-wheel-stage"><canvas class="mini-wheel" width="96" height="96"></canvas><div class="mini-wheel-marker"></div></div><input type="hidden" data-k="${key}" class="color-wheel-input" value="${value}"><div class="color-readout"><span class="color-swatch" style="background:${value};"></span><input type="text" class="color-hex-input" value="${value}" maxlength="7" spellcheck="false" aria-label="Farbe als Hex eingeben"></div></div></div></div>`;
  }

  function getEditorVariablePool() {
    const merged = [];
    const seenNames = new Set();

    currentVariables.forEach(variable => {
      if (!variable || !variable.name || !variable.type) {
        return;
      }
      const name = String(variable.name).trim();
      const type = String(variable.type).trim();
      if (!name || !type || seenNames.has(name)) {
        return;
      }
      seenNames.add(name);
      merged.push({ name, type });
    });

    steps.forEach(step => {
      if (!step || step.type !== 'set_variable') {
        return;
      }
      const values = step.values || {};
      const name = String(values.name || '').trim();
      const type = String(values.varType || '').trim();
      if (!name || !type || seenNames.has(name)) {
        return;
      }
      if (type !== 'brightness' && type !== 'duration' && type !== 'led_mask' && type !== 'color') {
        return;
      }
      seenNames.add(name);
      merged.push({ name, type });
    });

    return merged;
  }

  function getVariablesByType(type) {
    return getEditorVariablePool().filter(variable => variable.type === type);
  }

  function isVariableReference(value) {
    return typeof value === 'string' && value.startsWith('$');
  }

  function getVariableReferenceName(value) {
    return isVariableReference(value) ? value.slice(1) : '';
  }

  function buildVariablePicker(key, type, currentValue) {
    const selectedName = getVariableReferenceName(currentValue);
    let options = '<option value="">- Variable</option>';
    getVariablesByType(type).forEach(variable => {
      const selected = variable.name === selectedName ? ' selected' : '';
      options += `<option value="${escapeHtml(variable.name)}"${selected}>$${escapeHtml(variable.name)}</option>`;
    });
    return `<div class="field-stack compact"><select data-k="${key}" class="variable-picker">${options}</select></div>`;
  }

  function buildVariableNameSelect(key, currentValue, allowedTypes) {
    const types = Array.isArray(allowedTypes) ? allowedTypes : [];
    const filteredVariables = getEditorVariablePool().filter(variable => types.length === 0 || types.includes(variable.type));
    let options = '';

    filteredVariables.forEach(variable => {
      const selected = variable.name === currentValue ? ' selected' : '';
      options += `<option value="${escapeHtml(variable.name)}"${selected}>$${escapeHtml(variable.name)} (${escapeHtml(variable.type)})</option>`;
    });

    if (!options) {
      options = '<option value="">Keine Variablen vorhanden</option>';
    } else if (currentValue && !filteredVariables.some(variable => variable.name === currentValue)) {
      options += `<option value="${escapeHtml(currentValue)}" selected>${escapeHtml(currentValue)} (nicht gefunden)</option>`;
    }

    return `<select data-k="${key}" class="variable-picker">${options}</select>`;
  }

  function getColorDirectValue(rawValue) {
    if (isVariableReference(rawValue)) {
      const variable = currentVariables.find(entry => entry.name === getVariableReferenceName(rawValue) && entry.type === 'color');
      return variable && variable.hex ? variable.hex : '#FF0000';
    }
    return rawValue || '#FF0000';
  }

  function buildColorField(key, value) {
    return `<div class="field-inline"><label>Farbe</label>${buildWheelControl(key, getColorDirectValue(value), '')}${buildVariablePicker(key + '_var', 'color', value)}</div>`;
  }

  function buildBrightnessField(key, value, labelText) {
    const directValue = isVariableReference(value) ? '100' : value;
    return `<div class="field-inline"><label>${labelText}</label>${buildBrightnessSelect(key, directValue)}${buildVariablePicker(key + '_var', 'brightness', value)}</div>`;
  }

  function buildDurationField(key, value, labelText) {
    const directValue = isVariableReference(value) ? '1.0' : value;
    return `<div class="field-inline"><label>${labelText}</label><div class="number-wrap"><input type="number" data-k="${key}" class="var-direct-input" value="${directValue}" min="0" max="30" step="0.5" style="width:60px"><span>s</span></div>${buildVariablePicker(key + '_var', 'duration', value)}</div>`;
  }

  function defaultValueForVariableType(type) {
    if (type === 'led_mask') {
      return '1';
    }
    if (type === 'brightness') {
      return '100';
    }
    if (type === 'duration') {
      return '1';
    }
    if (type === 'color') {
      return '#ff0000';
    }
    return '';
  }

  function placeholderForVariableType(type) {
    if (type === 'color') {
      return '#ff0000';
    }
    return defaultValueForVariableType(type);
  }

  function buildLedTargetField(value) {
    const ledVariables = getVariablesByType('led_mask');
    const isVariableMode = isVariableReference(value) && ledVariables.length > 0;
    const directValue = isVariableReference(value) ? defaultLedsCsv() : value;
    const modeDisabled = ledVariables.length === 0 ? ' disabled' : '';
    const modeOptions = `<option value="fixed"${!isVariableMode ? ' selected' : ''}>fix</option><option value="var"${isVariableMode ? ' selected' : ''}>var</option>`;
    const fixedStyle = isVariableMode ? ' style="display:none"' : '';
    const variableStyle = isVariableMode ? '' : ' style="display:none"';
    return `<div class="field-inline led-target-field"><label>LEDs</label><div class="field-stack compact" style="width:16%;"><select data-k="leds_mode" class="variable-picker"${modeDisabled}>${modeOptions}</select></div><div class="led-checklist" data-led-target="fixed"${fixedStyle}>${ledCheckboxes(directValue)}</div><div data-led-target="var"${variableStyle}>${buildVariablePicker('leds_var', 'led_mask', value)}</div></div>`;
  }

  function updateLedTargetMode(field) {
    const modeSelect = field.querySelector('[data-k="leds_mode"]');
    const fixedTarget = field.querySelector('[data-led-target="fixed"]');
    const variableTarget = field.querySelector('[data-led-target="var"]');
    if (!modeSelect || !fixedTarget || !variableTarget) {
      return;
    }
    const useVariable = modeSelect.value === 'var';
    fixedTarget.style.display = useVariable ? 'none' : '';
    variableTarget.style.display = useVariable ? '' : 'none';
  }

  function initLedTargetFields() {
    document.querySelectorAll('.led-target-field').forEach(field => {
      const modeSelect = field.querySelector('[data-k="leds_mode"]');
      if (!modeSelect || modeSelect.dataset.bound === 'true') {
        updateLedTargetMode(field);
        return;
      }
      modeSelect.dataset.bound = 'true';
      modeSelect.addEventListener('change', () => updateLedTargetMode(field));
      updateLedTargetMode(field);
    });
  }

  function initSetVariableFields() {
    document.querySelectorAll('.block-set_variable').forEach(block => {
      const typeSelect = block.querySelector('[data-k="varType"]');
      const valueInput = block.querySelector('[data-k="value"]');
      if (!typeSelect || !valueInput) {
        return;
      }

      const applyTypeDefaults = () => {
        const nextType = typeSelect.value;
        valueInput.value = defaultValueForVariableType(nextType);
        valueInput.placeholder = placeholderForVariableType(nextType);
      };

      valueInput.placeholder = placeholderForVariableType(typeSelect.value);
      if (typeSelect.dataset.bound === 'true') {
        return;
      }

      typeSelect.dataset.bound = 'true';
      typeSelect.addEventListener('change', applyTypeDefaults);
    });
  }

  function resolveBlockValue(el, key, fallback) {
    const variableField = el.querySelector(`[data-k="${key}_var"]`);
    if (variableField && variableField.value) {
      return '$' + variableField.value;
    }
    const field = el.querySelector(`[data-k="${key}"]`);
    return field ? field.value : fallback;
  }

  function buildBrightnessSelect(key, value) {
    const parsed = Number.parseInt(value, 10);
    const safe = Number.isFinite(parsed) ? parsed : 100;
    const clamped = Math.min(100, Math.max(0, safe));
    const selected = Math.round(clamped / 10) * 10;
    let options = '';
    for (let level = 0; level <= 100; level += 10) {
      options += `<option value="${level}"${level === selected ? ' selected' : ''}>${level}</option>`;
    }
    return `<div class="number-wrap"><select data-k="${key}" style="width:58px">${options}</select><span>%</span></div>`;
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
    const getLedsCsv = () => {
      const modeField = el.querySelector('[data-k="leds_mode"]');
      const variableField = el.querySelector('[data-k="leds_var"]');
      if (modeField && modeField.value === 'var' && variableField && variableField.value) {
        return '$' + variableField.value;
      }
      const selected = Array.from(el.querySelectorAll('input[type="checkbox"][data-k="leds"]:checked'))
        .map(input => input.value);
      return selected.length > 0 ? selected.join(',') : '0';
    };
    if (type === 'set_color') {
      return { leds: getLedsCsv(), color: resolveBlockValue(el, 'color', '#FF0000'), br: resolveBlockValue(el, 'br', '100') };
    }
    if (type === 'set_brightness') {
      return { leds: getLedsCsv(), br: resolveBlockValue(el, 'br', '100') };
    }
    if (type === 'fade') {
      return {
        leds: getLedsCsv(),
        from: resolveBlockValue(el, 'from', '#FF0000'),
        to: resolveBlockValue(el, 'to', '#0000FF'),
        br: resolveBlockValue(el, 'br', '100'),
        s: resolveBlockValue(el, 's', '1.0')
      };
    }
    if (type === 'wait') {
      return { s: resolveBlockValue(el, 's', '1.0') };
    }
    if (type === 'set_variable') {
      return { name: get('name'), varType: get('varType'), value: get('value') };
    }
    if (type === 'change_variable') {
      return { name: get('name'), op_type: get('op_type'), value: get('value') };
    }
    if (type === 'repeat_start') {
      return { count: get('count') };
    }
    if (type === 'all_off') {
      return { leds: getLedsCsv() };
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

    const index = steps.findIndex(step => step.id === id);
    if (index < 0) {
      renderList();
      return;
    }

    const step = steps[index];
    const idsToDelete = new Set([id]);

    if (step.type === 'repeat_start') {
      let depth = 1;
      for (let i = index + 1; i < steps.length; i += 1) {
        if (steps[i].type === 'repeat_start') {
          depth += 1;
        } else if (steps[i].type === 'repeat_end') {
          depth -= 1;
          if (depth === 0) {
            idsToDelete.add(steps[i].id);
            break;
          }
        }
      }
    } else if (step.type === 'repeat_end') {
      let depth = 1;
      for (let i = index - 1; i >= 0; i -= 1) {
        if (steps[i].type === 'repeat_end') {
          depth += 1;
        } else if (steps[i].type === 'repeat_start') {
          depth -= 1;
          if (depth === 0) {
            idsToDelete.add(steps[i].id);
            break;
          }
        }
      }
    }

    steps = steps.filter(entry => !idsToDelete.has(entry.id));
    renderList();
  }

  function duplicateBlock(id) {
    syncStepsFromDom();

    const index = steps.findIndex(step => step.id === id);
    if (index < 0) {
      return;
    }

    const source = steps[index];
    const copiedValues = JSON.parse(JSON.stringify(source.values || defaultValuesForType(source.type)));
    const clone = {
      id: Date.now() + Math.floor(Math.random() * 1000),
      type: source.type,
      values: copiedValues
    };

    steps.splice(index + 1, 0, clone);
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
    const ledSource = values && values.leds ? values.leds : '0';
    const usesLedVariable = isVariableReference(ledSource);
    const selectedLeds = usesLedVariable ? [0] : normalizeLedCsv(ledSource);
    const ledsCsv = usesLedVariable ? ledSource : selectedLeds.join(',');
    if (type === 'set_color') {
      step.op = 'set';
      step.led = selectedLeds[0];
      step.leds = ledsCsv;
      step.color = values.color;
      step.br = isVariableReference(values.br) ? values.br : parseInt(values.br, 10);
    } else if (type === 'set_brightness') {
      step.op = 'brightness';
      step.led = selectedLeds[0];
      step.leds = ledsCsv;
      step.br = isVariableReference(values.br) ? values.br : parseInt(values.br, 10);
    } else if (type === 'fade') {
      step.led = selectedLeds[0];
      step.leds = ledsCsv;
      step.from = values.from;
      step.to = values.to;
      step.br = isVariableReference(values.br) ? values.br : parseInt(values.br, 10);
      step.s = isVariableReference(values.s) ? values.s : parseFloat(values.s);
    } else if (type === 'wait') {
      step.s = isVariableReference(values.s) ? values.s : parseFloat(values.s);
    } else if (type === 'set_variable') {
      step.op = 'set_var';
      step.name = values.name;
      step.type = values.varType;
      step.value = values.value;
    } else if (type === 'change_variable') {
      step.op = 'change_var';
      step.name = values.name;
      step.op_type = values.op_type;
      step.value = parseInt(values.value, 10);
    } else if (type === 'repeat_start') {
      step.op = 'repeat_start';
      step.count = parseInt(values.count, 10);
    } else if (type === 'repeat_end') {
      step.op = 'repeat_end';
    } else if (type === 'all_off') {
      step.op = 'all_off';
      step.led = selectedLeds[0];
      step.leds = ledsCsv;
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
      if (!led.enabled) {
        dot.classList.remove('active');
        dot.style.background = '#d9e1ee';
        dot.style.boxShadow = 'inset 0 1px 2px rgba(255,255,255,0.95)';
        dot.style.opacity = '0.45';
        return;
      }
      const rr = Math.max(0, Math.min(255, Math.round(led.r)));
      const gg = Math.max(0, Math.min(255, Math.round(led.g)));
      const bb = Math.max(0, Math.min(255, Math.round(led.b)));
      dot.classList.add('active');
      dot.style.background = `rgb(${rr}, ${gg}, ${bb})`;
      dot.style.boxShadow = `0 0 0 1px rgba(20,32,51,0.14), 0 0 14px rgba(${rr}, ${gg}, ${bb}, 0.48)`;
      dot.style.opacity = '1';
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
    const variableStore = {};

    (payload.vars || []).forEach(variable => {
      variableStore[variable.name] = { ...variable };
    });

    function getVariableReference(rawValue) {
      if (typeof rawValue === 'string' && rawValue.startsWith('$')) {
        return variableStore[rawValue.slice(1)] || null;
      }
      return null;
    }

    function resolvePreviewLedTargets(rawValue, fallbackLed) {
      const variable = getVariableReference(rawValue);
      if (variable && variable.type === 'led_mask') {
        const result = [];
        for (let index = 0; index < MAX_LEDS; index += 1) {
          if ((variable.value || 0) & (1 << index)) {
            result.push(index);
          }
        }
        return result.length ? result : [0];
      }
      return normalizeLedCsv(rawValue || String(fallbackLed));
    }

    function resolvePreviewBrightness(rawValue) {
      const variable = getVariableReference(rawValue);
      if (variable && variable.type === 'brightness') {
        return variable.value;
      }
      return Number.isFinite(rawValue) ? rawValue : parseInt(rawValue || '100', 10);
    }

    function resolvePreviewDurationMs(rawValue) {
      const variable = getVariableReference(rawValue);
      if (variable && variable.type === 'duration') {
        return variable.value;
      }
      return Math.max(0, (parseFloat(rawValue || 0) || 0) * 1000);
    }

    function resolvePreviewColor(rawValue) {
      const variable = getVariableReference(rawValue);
      if (variable && variable.type === 'color') {
        return { r: variable.r, g: variable.g, b: variable.b };
      }
      return hexToRgb(rawValue || '#000000');
    }

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
      const targetLeds = resolvePreviewLedTargets(step.leds, step.led);
      if (step.op === 'set') {
        const rgb = resolvePreviewColor(step.color || '#000000');
        const brightness = resolvePreviewBrightness(step.br);
        targetLeds.forEach(ledIndex => {
          state[ledIndex] = { enabled: true, r: rgb.r, g: rgb.g, b: rgb.b, br: brightness };
        });
        applyLedPreviewState(state);
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
        return;
      }

      if (step.op === 'brightness') {
        const brightness = resolvePreviewBrightness(step.br);
        targetLeds.forEach(ledIndex => {
          const current = state[ledIndex] || { enabled: false, r: 0, g: 0, b: 0, br: 0 };
          state[ledIndex] = { ...current, enabled: true, br: Number.isFinite(brightness) ? brightness : current.br };
        });
        applyLedPreviewState(state);
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
        return;
      }

      if (step.op === 'all_off') {
        const offTargets = step.leds
          ? normalizeLedCsv(step.leds)
          : (Number.isInteger(step.led) ? [step.led] : Array.from({ length: MAX_LEDS }, (_, i) => i));
        offTargets.forEach(ledIndex => {
          state[ledIndex] = { enabled: false, r: 0, g: 0, b: 0, br: 0 };
        });
        applyLedPreviewState(state);
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
        return;
      }

      if (step.op === 'wait') {
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), resolvePreviewDurationMs(step.s));
        return;
      }

      if (step.op === 'fade') {
        const from = resolvePreviewColor(step.from || '#000000');
        const to = resolvePreviewColor(step.to || '#000000');
        const duration = resolvePreviewDurationMs(step.s);
        const brightness = resolvePreviewBrightness(step.br);
        if (duration === 0) {
          targetLeds.forEach(ledIndex => {
            state[ledIndex] = { enabled: true, r: to.r, g: to.g, b: to.b, br: brightness };
          });
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
          targetLeds.forEach(ledIndex => {
            state[ledIndex] = {
              enabled: true,
              r: Math.round(from.r + (to.r - from.r) * progress),
              g: Math.round(from.g + (to.g - from.g) * progress),
              b: Math.round(from.b + (to.b - from.b) * progress),
              br: brightness
            };
          });
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

      if (step.op === 'set_var') {
        if (step.type === 'color') {
          const rgb = resolvePreviewColor(step.value || '#000000');
          variableStore[step.name] = { name: step.name, type: 'color', value: 0, r: rgb.r, g: rgb.g, b: rgb.b, hex: rgbToHex(rgb.r, rgb.g, rgb.b) };
        } else {
          variableStore[step.name] = { name: step.name, type: step.type, value: parseInt(step.value || 0, 10) || 0, r: 0, g: 0, b: 0 };
        }
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
        return;
      }

      if (step.op === 'change_var') {
        const current = variableStore[step.name];
        if (current && current.type !== 'color') {
          const delta = parseInt(step.value || 0, 10) || 0;
          if (step.op_type === 'add') current.value += delta;
          else if (step.op_type === 'subtract') current.value -= delta;
          else if (step.op_type === 'multiply') current.value *= delta;
          else current.value = delta;
        }
        scheduleLedPreviewNext(() => runStep(stepIndex + 1), 0);
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
    // Preserve any in-flight DOM changes (e.g. LED multi-select, color wheel) before
    // clearing and rebuilding the list.  Safe even when steps is empty: readBlockValues
    // falls back to the stored step.values when the element doesn't exist yet.
    syncStepsFromDom();
    const list = document.getElementById('scriptList');
    list.innerHTML = '';

    const scriptStart = document.createElement('div');
    scriptStart.className = 'block block-script_start';
    scriptStart.innerHTML = `<span class="block-label">Script Start</span><div class="number-wrap"><input type="checkbox" id="scriptLoopChk" ${scriptLoopEnabled ? 'checked' : ''}><span>wiederholen</span></div>`;
    list.appendChild(scriptStart);

    steps.forEach(step => {
      const values = step.values || defaultValuesForType(step.type);
      const selectedLeds = values.leds || values.led || '0';
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

      // Einrückung innerhalb Script/Repeat-Strukturen
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
        inner = `<span class="block-label">Farbe</span>${buildLedTargetField(selectedLeds)}${buildColorField('color', values.color)}${buildBrightnessField('br', values.br, 'Helligkeit')}`;
      } else if (step.type === 'set_brightness') {
        inner = `<span class="block-label">Helligkeit</span>${buildLedTargetField(selectedLeds)}${buildBrightnessField('br', values.br, 'Wert')}`;
      } else if (step.type === 'fade') {
        inner = `<span class="block-label">Blend</span>${buildLedTargetField(selectedLeds)}<div class="field-inline"><label>Von</label>${buildWheelControl('from', getColorDirectValue(values.from), '')}${buildVariablePicker('from_var', 'color', values.from)}</div><div class="field-inline"><label>Nach</label>${buildWheelControl('to', getColorDirectValue(values.to), '')}${buildVariablePicker('to_var', 'color', values.to)}</div>${buildBrightnessField('br', values.br, 'Helligkeit')}${buildDurationField('s', values.s, 'Dauer')}`;
      } else if (step.type === 'wait') {
        inner = `<span class="block-label">Warten</span>${buildDurationField('s', values.s, 'Dauer')}`;
      } else if (step.type === 'set_variable') {
        inner = `<span class="block-label">Variable setzen</span><div class="field-inline"><label>Name</label><input type="text" data-k="name" value="${values.name}" maxlength="15"></div><div class="field-inline"><label>Typ</label><select data-k="varType"><option value="brightness"${values.varType === 'brightness' ? ' selected' : ''}>Helligkeit</option><option value="duration"${values.varType === 'duration' ? ' selected' : ''}>Dauer</option><option value="led_mask"${values.varType === 'led_mask' ? ' selected' : ''}>LED-Auswahl</option><option value="color"${values.varType === 'color' ? ' selected' : ''}>Farbe</option></select></div><div class="field-inline"><label>Wert</label><input type="text" data-k="value" value="${values.value}" placeholder="${placeholderForVariableType(values.varType)}"></div>`;
      } else if (step.type === 'change_variable') {
        inner = `<span class="block-label">Variable ändern</span><div class="field-inline"><label>Name</label>${buildVariableNameSelect('name', values.name, ['brightness', 'duration', 'led_mask'])}</div><div class="field-inline"><label>Aktion</label><select data-k="op_type"><option value="add"${values.op_type === 'add' ? ' selected' : ''}>Addieren</option><option value="subtract"${values.op_type === 'subtract' ? ' selected' : ''}>Subtrahieren</option><option value="multiply"${values.op_type === 'multiply' ? ' selected' : ''}>Multiplizieren</option><option value="set"${values.op_type === 'set' ? ' selected' : ''}>Setzen</option></select></div><div class="field-inline"><label>Wert</label><input type="number" data-k="value" value="${values.value}" min="0" max="65535"></div>`;
      } else if (step.type === 'repeat_start') {
        inner = `<span class="block-label">Wiederholen</span><div class="field-inline"><label>Wiederhole</label><div class="number-wrap"><input type="number" data-k="count" value="${values.count}" min="1" max="16" step="1" style="width:60px"><span>mal</span></div></div><span class="inline-note">ab hier bis Wiederholen Ende</span>`;
      } else if (step.type === 'repeat_end') {
        inner = `<span class="block-label">Wiederholen Ende</span><span class="inline-note">Ende Wiederhol-Block</span>`;
      } else if (step.type === 'all_off') {
        inner = `<span class="block-label">Ausschalten</span>${buildLedTargetField(selectedLeds)}`;
      }
      inner += `<div class="block-actions"><button class="block-mini-btn copy" onclick="duplicateBlock(${step.id})" title="Kopieren" aria-label="Kopieren"><svg viewBox="0 0 24 24" aria-hidden="true"><rect x="9" y="9" width="11" height="11" rx="2"></rect><rect x="4" y="4" width="11" height="11" rx="2"></rect></svg></button><button class="block-mini-btn remove" onclick="removeBlock(${step.id})" title="Entfernen" aria-label="Entfernen"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 6l12 12"></path><path d="M18 6 6 18"></path></svg></button></div>`;
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
        syncStepsFromDom();
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
    initLedTargetFields();
    initSetVariableFields();
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
      vars: currentVariables.map(variable => ({ name: variable.name, type: variable.type, value: variable.type === 'color' ? variable.hex : variable.value })),
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
          setStatus('Script läuft.', true);
          ledSimulatorRunning = true;
          syncToolbarStates();
          startLedPreviewPlayback();
          return;
        }
        response.text().then(text => setStatus('Fehler: ' + text, false));
      })
      .catch(() => setStatus('Verbindungsfehler.', false));
  }

  function fetchScriptNames() {
    return fetch('/scripts/list')
      .then(response => {
        if (!response.ok) {
          throw new Error('scripts/list failed');
        }
        return response.json();
      })
      .then(data => Array.isArray(data.scripts) ? data.scripts : []);
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

    fetchScriptNames()
      .then(scriptNames => {
        const existingHint = scriptNames.length > 0 ? ('\nVorhanden: ' + scriptNames.join(', ')) : '';
        const rawName = prompt('Script-Name eingeben:' + existingHint, 'Script 1');
        if (rawName === null) {
          return;
        }
        const name = rawName.trim();
        if (!name) {
          setStatus('Bitte einen gültigen Script-Namen eingeben.', false);
          return;
        }

        const lowerName = name.toLowerCase();
        const exists = scriptNames.some(entry => String(entry).toLowerCase() === lowerName);
        if (exists && !confirm('Script "' + name + '" existiert bereits. Überschreiben?')) {
          return;
        }

        fetch('/scripts/slot/save?name=' + encodeURIComponent(name), {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify(payload)
        })
          .then(response => {
            if (response.ok) {
              setStatus('Script "' + name + '" gespeichert.', false);
              if (ledSimulatorRunning) {
                startLedPreviewPlayback();
              }
              return;
            }
            response.text().then(text => setStatus('Fehler: ' + text, false));
          })
          .catch(() => setStatus('Verbindungsfehler.', false));
      })
      .catch(() => setStatus('Script-Liste konnte nicht geladen werden.', false));
  }

  function openLoadScriptPopup() {
    fetchScriptNames()
      .then(scriptNames => {
        loadOverlayScriptNames = scriptNames;
        const overlay = document.getElementById('loadScriptOverlay');
        const hint = document.getElementById('loadScriptOverlayHint');
        const select = document.getElementById('loadScriptSelect');
        if (!overlay || !hint || !select) {
          setStatus('Overlay konnte nicht geöffnet werden.', false);
          return;
        }

        select.innerHTML = '';
        scriptNames.forEach(name => {
          const option = document.createElement('option');
          option.value = String(name);
          option.textContent = String(name);
          select.appendChild(option);
        });

        if (scriptNames.length === 0) {
          hint.textContent = 'Keine gespeicherten Scripts gefunden.';
        } else {
          hint.textContent = 'Wähle ein gespeichertes Script. Doppelklick lädt sofort.';
          select.selectedIndex = 0;
        }

        overlay.classList.add('open');
        overlay.setAttribute('aria-hidden', 'false');
        select.focus();
      })
      .catch(() => setStatus('Script-Liste konnte nicht geladen werden.', false));
  }

  function closeLoadScriptPopup() {
    const overlay = document.getElementById('loadScriptOverlay');
    if (!overlay) {
      return;
    }
    overlay.classList.remove('open');
    overlay.setAttribute('aria-hidden', 'true');
  }

  function getSelectedOverlayScriptName() {
    const select = document.getElementById('loadScriptSelect');
    if (!select) {
      return '';
    }
    const selected = select.value ? String(select.value).trim() : '';
    return selected;
  }

  function loadSelectedScriptFromOverlay() {
    const name = getSelectedOverlayScriptName();
    if (!name) {
      setStatus('Bitte ein Script auswählen.', false);
      return;
    }
    closeLoadScriptPopup();
    loadScriptFromLib(name);
  }

  function deleteSelectedScriptFromOverlay() {
    const name = getSelectedOverlayScriptName();
    if (!name) {
      setStatus('Bitte ein Script auswählen.', false);
      return;
    }
    if (!confirm('Script "' + name + '" wirklich löschen?')) {
      return;
    }

    fetch('/scripts/slot/delete?name=' + encodeURIComponent(name), { method: 'POST' })
      .then(response => {
        if (!response.ok) {
          throw new Error('delete failed');
        }
        setStatus('Script "' + name + '" gelöscht.', false);
        openLoadScriptPopup();
      })
      .catch(() => setStatus('Löschen fehlgeschlagen.', false));
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
          setStatus('Gespeichertes Script gelöscht.', false);
          ledSimulatorRunning = false;
          syncToolbarStates();
          resetLedPreviewPlayback();
        }
      })
      .catch(() => setStatus('Löschen fehlgeschlagen.', false));
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
  const overlay = document.getElementById('loadScriptOverlay');
  if (overlay) {
    overlay.addEventListener('click', event => {
      if (event.target === overlay) {
        closeLoadScriptPopup();
      }
    });
  }
  const overlaySelect = document.getElementById('loadScriptSelect');
  if (overlaySelect) {
    overlaySelect.addEventListener('dblclick', () => {
      loadSelectedScriptFromOverlay();
    });
  }
  document.addEventListener('keydown', event => {
    const activeOverlay = document.getElementById('loadScriptOverlay');
    if (event.key === 'Escape' && activeOverlay && activeOverlay.classList.contains('open')) {
      closeLoadScriptPopup();
    }
  });
  function stepsFromOps(ops) {
    const result = [];
    for (let i = 0; i < ops.length; i++) {
      const op = ops[i];
      const id = Date.now() + i * 1000 + Math.floor(Math.random() * 1000);
      if (op.op === 'set') {
        result.push({ id, type: 'set_color', values: { leds: op.leds || String(op.led != null ? op.led : 0), color: op.color || '#FF0000', br: String(op.br != null ? op.br : 100) } });
      } else if (op.op === 'brightness') {
        result.push({ id, type: 'set_brightness', values: { leds: op.leds || String(op.led != null ? op.led : 0), br: String(op.br != null ? op.br : 100) } });
      } else if (op.op === 'fade') {
        result.push({ id, type: 'fade', values: { leds: op.leds || String(op.led != null ? op.led : 0), from: op.from || '#FF0000', to: op.to || '#0000FF', br: String(op.br != null ? op.br : 100), s: String(op.s != null ? op.s : 1) } });
      } else if (op.op === 'wait') {
        result.push({ id, type: 'wait', values: { s: String(op.s != null ? op.s : 1) } });
      } else if (op.op === 'set_var') {
        result.push({ id, type: 'set_variable', values: { name: op.name || 'var_name', varType: op.type || 'brightness', value: String(op.value != null ? op.value : 0) } });
      } else if (op.op === 'change_var') {
        result.push({ id, type: 'change_variable', values: { name: op.name || 'var_name', op_type: op.op_type || 'add', value: String(op.value != null ? op.value : 0) } });
      } else if (op.op === 'all_off') {
        result.push({ id, type: 'all_off', values: { leds: op.leds || String(op.led != null ? op.led : 0) } });
      }
    }
    return result;
  }

  function loadScriptFromLib(name) {
    fetch('/scripts/slot/load?name=' + encodeURIComponent(name))
      .then(r => { if (!r.ok) throw new Error(); return r.json(); })
      .then(data => {
        syncStepsFromDom();
        steps = stepsFromOps(data.ops || []);
        scriptLoopEnabled = !!data.loop;
        renderList();
        const activePayload = { loop: !!data.loop, ops: Array.isArray(data.ops) ? data.ops : [] };
        fetch('/script/save', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(activePayload)
        }).then(response => {
          if (response.ok) {
            setStatus('Script "' + name + '" geladen (Autoload nach Neustart aktiv).', false);
            return;
          }
          setStatus('Script "' + name + '" geladen (Autoload konnte nicht gespeichert werden).', false);
        }).catch(() => {
          setStatus('Script "' + name + '" geladen (Autoload konnte nicht gespeichert werden).', false);
        });
        if (ledSimulatorRunning) { startLedPreviewPlayback(); }
      })
      .catch(() => setStatus('Laden fehlgeschlagen.', false));
  }

  function loadActiveScriptFromDevice() {
    fetch('/script/current')
      .then(response => {
        if (response.status === 204) {
          return null;
        }
        if (!response.ok) {
          throw new Error('script/current failed');
        }
        return response.json();
      })
      .then(data => {
        if (!data || !Array.isArray(data.ops) || data.ops.length === 0) {
          return;
        }
        syncStepsFromDom();
        steps = stepsFromOps(data.ops);
        scriptLoopEnabled = !!data.loop;
        ledSimulatorRunning = true;
        syncToolbarStates();
        renderList();
        setStatus('Aktives Script vom Gerät geladen.', true);
      })
      .catch(() => {
        // Keep the editor usable even if the active script endpoint is unavailable.
      });
  }

  syncLedCountSelect();
  syncToolbarStates();
  setupToolboxDnD();
  renderList();
  refreshLogicVariables();
  loadActiveScriptFromDevice();
</script>
</body>
</html>)HTML");
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
    :root { --bg:#f3eeff; --panel:#fffaff; --line:#ddd0f0; --text:#1a1030; --muted:#6d5a84; --accent:#9b3db8; --danger:#ab2f2f; }
    body { margin:0; min-height:100vh; font-family:"Avenir Next","Segoe UI",sans-serif; color:var(--text); background:linear-gradient(160deg,#e8f0ff 0%,#f5e6ff 100%); padding:20px; }
    .shell { width:min(1020px,100%); margin:0 auto; display:grid; gap:16px; }
    .panel { background:var(--panel); border:1px solid var(--line); border-radius:20px; padding:20px; }
    .topbar { display:flex; align-items:center; gap:12px; flex-wrap:wrap; margin-bottom:4px; }
    .menu { position:relative; }
    .menu summary { list-style:none; }
    .menu summary::-webkit-details-marker { display:none; }
    .menu-button { border:1px solid var(--line); border-radius:12px; padding:10px 14px; background:#ffffff; font-weight:700; cursor:pointer; color:var(--text); }
    .menu-icon { font-size:1.2rem; line-height:1; }
    .menu[open] .menu-button { border-color:#9e82cb; box-shadow:0 4px 14px rgba(26,16,48,0.14); }
    .menu-list { position:absolute; top:46px; left:0; min-width:180px; display:grid; gap:2px; background:#fff; border:1px solid var(--line); border-radius:12px; padding:6px; box-shadow:0 10px 24px rgba(26,16,48,0.18); z-index:20; }
    .menu-list a { display:block; padding:10px 12px; border-radius:8px; text-decoration:none; font-weight:700; color:var(--text); }
    .menu-list a:hover { background:#f2eaff; }
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
    .config-title-icon { width:84px; height:84px; flex:0 0 auto; overflow:visible; }
    /* V2 Variables UI Styles */
    .variables-list { background: #f9f7fc; border-radius: 12px; padding: 16px; margin-bottom: 20px; }
    .variable-item { display:flex; align-items:center; justify-content:space-between; padding:12px; background:#fff; border:1px solid #ddd0f0; border-radius:8px; margin-bottom:8px; }
    .variable-item-info { flex:1; }
    .variable-name { font-weight:700; color:#1a1030; font-family:monospace; }
    .variable-type { font-size:0.9rem; color:#6d5a84; }
    .variable-value { background:#f2eaff; padding:4px 8px; border-radius:4px; font-family:monospace; font-size:0.9rem; }
    .variable-item-actions { display:flex; gap:6px; }
    .variable-item-actions button { padding:6px 10px; font-size:0.85rem; border:1px solid #ddd0f0; background:#fff; border-radius:6px; cursor:pointer; color:#6d5a84; }
    .variable-item-actions button:hover { background:#f2eaff; }
    .variable-creation-panel { background:#f9f7fc; border:2px dashed #9b3db8; border-radius:12px; padding:16px; }
    .led-strip-preview { display:flex; gap:6px; margin:10px 0; flex-wrap:wrap; }
    .led { width:32px; height:32px; border-radius:4px; cursor:pointer; border:2px solid #ddd0f0; background:#fff; transition:all 0.2s; }
    .led:hover { transform:scale(1.1); }
    .led.selected { border-color:#9b3db8; background:#9b3db8; box-shadow:0 0 12px rgba(155,61,184,0.4); }
    .led-actions { display:flex; gap:8px; margin-top:10px; }
    .led-actions button { flex:1; padding:8px 12px; font-size:0.9rem; background:linear-gradient(135deg,#8d5f18,#bd8528); color:#fff; border:0; border-radius:8px; cursor:pointer; font-weight:600; }
    .brightness-slider-container { display:flex; align-items:center; gap:10px; margin:10px 0; }
    .brightness-slider-container input[type="range"] { flex:1; width:auto; }
    .value-display { min-width:40px; text-align:right; font-weight:700; color:#9b3db8; }
    .quick-presets { display:flex; gap:6px; flex-wrap:wrap; margin:10px 0; }
    .quick-presets button { flex:0 1 auto; padding:8px 12px; font-size:0.9rem; background:#f2eaff; color:#9b3db8; border:1px solid #9b3db8; border-radius:6px; cursor:pointer; font-weight:600; }
    .quick-presets button:hover { background:#9b3db8; color:#fff; }
    .duration-input-group { display:flex; align-items:center; gap:8px; margin:10px 0; }
    .duration-input-group input { width:80px; margin:0; }
    .duration-input-group span { color:#6d5a84; font-weight:600; }
    .color-input-group { display:flex; align-items:center; gap:10px; margin:10px 0; }
    .color-input-group input[type="color"] { width:60px; height:40px; border-radius:6px; cursor:pointer; margin:0; }
    @media (max-width: 860px) { .layout { grid-template-columns:1fr; } }
  </style>
</head>
<body>
  <main class="shell">
    <section class="panel">
      <div class="topbar">
        <details class="menu">
          <summary class="menu-button" aria-label="Menü"><span class="menu-icon">&#9776;</span></summary>
          <nav class="menu-list">
            <a href="/">LED-Logic</a>
            <a href="/config">Konfiguration</a>
          </nav>
        </details>
        <div class="config-title">
          <svg class="config-title-icon" viewBox="0 0 64 64" aria-hidden="true">
            <image href=")HTML";
  page += kLedLogicIconUrl;
  page += R"HTML(" x="-2" y="-2" width="68" height="68" preserveAspectRatio="xMidYMid slice"/>
          </svg>
          <h1>Konfigurationsseite</h1>
        </div>
      </div>
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
        <select id="scan-results"><option value="">Noch kein Scan ausgeführt</option></select>

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
          <button class="secondary" type="submit">WLAN-Konfiguration löschen</button>
        </form>

        <p class="tiny">AP SSID: <strong>)HTML";
  page += htmlEscape(kAccessPointSsid);
  page += R"HTML(</strong><br>AP Passwort: <strong>)HTML";
  page += htmlEscape(kAccessPointPassword);
  page += R"HTML(</strong></p>
      </article>

      <article class="panel">
        <h2>OTA Update</h2>
        <form method="get" action="/ota/check" id="ota-check-form">
          <label for="check-url">GitHub Metadata URL (JSON oder Text)</label>
          <input id="check-url" name="url" placeholder="https://.../latest.json oder version.txt" value=")HTML";
  page += htmlEscape(kDefaultOtaCheckUrl);
  page += R"HTML(">
          <button class="neutral" type="submit">Version prüfen</button>
        </form>
        <div class="pill" id="ota-check-result">
          <div><strong>Installierte Version:</strong> <span id="ota-current-version">)HTML";
  page += htmlEscape(firmwareVersion);
  page += R"HTML(</span></div>
          <div><strong>Verfügbare Version:</strong> <span id="ota-remote-version">-</span></div>
          <div class="tiny" id="ota-check-message">Noch keine Prüfung ausgeführt.</div>
        </div>

        <form method="post" action="/ota/update_url">
          <label for="bin-url">Direkter Firmware-URL (.bin, z. B. GitHub Release Asset)</label>
          <input id="bin-url" name="url" placeholder="https://.../firmware.bin" value=")HTML";
  page += htmlEscape(kDefaultOtaBinUrl);
  page += R"HTML(">
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
      <h2>🔧 Variablen-Management (V2)</h2>
      
      <div class="variables-list">
        <h4>Definierte Variablen</h4>
        <div class="tiny" id="variableStatus">Noch keine Variablen definiert.</div>
        <div id="variablesContainer" style="display:grid; gap:10px;">
        </div>
      </div>

      <div class="variable-creation-panel">
        <h4>Neue Variable erstellen</h4>
        
        <label>Variablenname</label>
        <input type="text" id="varName" placeholder="z.B. 'my_brightness'" maxlength="15" style="margin-bottom:10px;">
        
        <label>Variable Typ</label>
        <select id="varType" onchange="updateVariableInput()" style="margin-bottom:10px;">
          <option value="brightness">Helligkeit (0-255)</option>
          <option value="duration">Dauer (ms)</option>
          <option value="led_mask">LED-Auswahl</option>
          <option value="color">Farbe (RGB)</option>
        </select>
        
        <div id="varInputContainer" style="margin:10px 0;">
          <!-- Dynamisch aktualisiert -->
        </div>
        
        <div class="led-actions">
          <button class="primary" id="variableSubmitButton" onclick="saveVariable()" type="button" style="margin-top:10px;">Variable erstellen</button>
          <button class="neutral" onclick="resetVariableForm()" type="button" style="margin-top:10px;">Zurücksetzen</button>
        </div>
      </div>
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
      select.innerHTML = '<option value="">Scan läuft...</option>';
      try {
        const response = await fetch('/scan');
        const networks = await response.json();
        if (!Array.isArray(networks) || networks.length === 0) {
          select.innerHTML = '<option value="">Keine Netzwerke gefunden</option>';
          return;
        }

        select.innerHTML = '<option value="">SSID aus Scan wählen</option>';
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
        document.getElementById('ota-current-version').textContent = data.version || '-';
        document.getElementById('logbox').innerHTML = data.logs.map((entry) => `<div>${escapeHtml(entry)}</div>`).join('');
      } catch (error) {
        console.log(error);
      }
    }

    function setupOtaCheck() {
      const form = document.getElementById('ota-check-form');
      const currentVersion = document.getElementById('ota-current-version');
      const remoteVersion = document.getElementById('ota-remote-version');
      const message = document.getElementById('ota-check-message');
      const binUrl = document.getElementById('bin-url');

      form.addEventListener('submit', async event => {
        event.preventDefault();
        const formData = new FormData(form);
        const url = new URL('/ota/check', window.location.origin);
        url.searchParams.set('url', formData.get('url'));

        remoteVersion.textContent = '-';
        message.textContent = 'Prüfung läuft...';

        try {
          const response = await fetch(url.toString());
          const data = await response.json();
          if (!response.ok) {
            message.textContent = data.error ? `Fehler: ${data.error}` : 'Prüfung fehlgeschlagen.';
            return;
          }

          currentVersion.textContent = data.currentVersion || '-';
          remoteVersion.textContent = data.remoteVersion || '-';
          if (data.binUrl) {
            binUrl.value = data.binUrl;
          }

          if (!data.remoteVersion) {
            message.textContent = 'Keine verfügbare Version gefunden.';
          } else if (data.updateAvailable) {
            message.textContent = 'Update verfügbar.';
          } else {
            message.textContent = 'Firmware ist aktuell.';
          }
        } catch (error) {
          message.textContent = 'Prüfung fehlgeschlagen.';
          console.log(error);
        }
      });
    }

    // V2 Variables UI Functions
    let editingVariableName = '';
    let currentVariables = [];

    function showVariableStatus(message, isError) {
      const target = document.getElementById('variableStatus');
      target.textContent = message;
      target.style.color = isError ? '#ab2f2f' : '#6d5a84';
    }

    function updateVariableInput(existingVariable) {
      const varType = document.getElementById('varType').value;
      const container = document.getElementById('varInputContainer');
      let html = '';

      switch (varType) {
        case 'brightness':
          html = `<label>Helligkeit (0-255)</label>
            <div class="brightness-slider-container">
              <input type="range" id="varValueSlider" min="0" max="255" value="128"
                     oninput="document.getElementById('varValueDisplay').textContent = this.value">
              <span id="varValueDisplay" class="value-display">128</span>
            </div>
            <div class="quick-presets">
              <button onclick="setVariableBrightness(0)" type="button">0%</button>
              <button onclick="setVariableBrightness(128)" type="button">50%</button>
              <button onclick="setVariableBrightness(200)" type="button">80%</button>
              <button onclick="setVariableBrightness(255)" type="button">100%</button>
            </div>`;
          break;

        case 'duration':
          html = `<label>Dauer</label>
            <div class="duration-input-group">
              <input type="number" id="varDurationSec" min="0" max="30" value="1" placeholder="0">
              <span>s</span>
              <input type="number" id="varDurationMs" min="0" max="999" value="0" placeholder="0">
              <span>ms</span>
            </div>
            <div class="quick-presets">
              <button onclick="setVariableDuration(100)" type="button">100ms</button>
              <button onclick="setVariableDuration(500)" type="button">500ms</button>
              <button onclick="setVariableDuration(1000)" type="button">1s</button>
              <button onclick="setVariableDuration(2000)" type="button">2s</button>
              <button onclick="setVariableDuration(5000)" type="button">5s</button>
            </div>`;
          break;

        case 'led_mask':
          html = `<label>LEDs auswählen</label>
            <div class="led-strip-preview" id="ledStripPreview">` +
            Array.from({ length: 12 }, (_, i) =>
              `<div class="led" data-index="${i}" onclick="toggleLED(${i})"></div>`
            ).join('') +
            `</div>
            <div class="led-actions">
              <button onclick="selectAllLEDs()" type="button">Alle</button>
              <button onclick="selectNoLEDs()" type="button">Keine</button>
            </div>`;
          break;

        case 'color':
          html = `<label>Farbe (RGB)</label>
            <div class="color-input-group">
              <input type="color" id="varColorPicker" value="#ff0000"
                     oninput="document.getElementById('varColorHex').value = this.value">
              <input type="text" id="varColorHex" value="#ff0000" maxlength="7" style="margin:0;">
            </div>`;
          break;
      }

      container.innerHTML = html;
      if (existingVariable) {
        populateVariableInputs(existingVariable);
      }
    }

    function populateVariableInputs(variable) {
      if (!variable) {
        return;
      }
      if (variable.type === 'brightness') {
        setVariableBrightness(variable.value || 0);
      } else if (variable.type === 'duration') {
        setVariableDuration(variable.value || 0);
      } else if (variable.type === 'led_mask') {
        selectNoLEDs();
        for (let index = 0; index < 12; index += 1) {
          if ((variable.value || 0) & (1 << index)) {
            toggleLED(index);
          }
        }
      } else if (variable.type === 'color') {
        const hex = variable.hex || '#ff0000';
        document.getElementById('varColorPicker').value = hex;
        document.getElementById('varColorHex').value = hex;
      }
    }

    function setVariableBrightness(value) {
      document.getElementById('varValueSlider').value = value;
      document.getElementById('varValueDisplay').textContent = value;
    }

    function setVariableDuration(ms) {
      const sec = Math.floor(ms / 1000);
      const msPart = ms % 1000;
      document.getElementById('varDurationSec').value = sec;
      document.getElementById('varDurationMs').value = msPart;
    }

    function toggleLED(index) {
      const led = document.querySelector(`[data-index="${index}"]`);
      if (led) {
        led.classList.toggle('selected');
      }
    }

    function selectAllLEDs() {
      document.querySelectorAll('[data-index]').forEach(led => led.classList.add('selected'));
    }

    function selectNoLEDs() {
      document.querySelectorAll('[data-index]').forEach(led => led.classList.remove('selected'));
    }

    function getLEDMask() {
      let mask = 0;
      document.querySelectorAll('[data-index].selected').forEach(led => {
        mask |= (1 << parseInt(led.dataset.index, 10));
      });
      return mask;
    }

    function resetVariableForm() {
      editingVariableName = '';
      document.getElementById('varName').value = '';
      document.getElementById('varType').value = 'brightness';
      document.getElementById('variableSubmitButton').textContent = 'Variable erstellen';
      updateVariableInput();
    }

    function renderVariables() {
      const container = document.getElementById('variablesContainer');
      container.innerHTML = '';

      if (!currentVariables.length) {
        showVariableStatus('Noch keine Variablen definiert.', false);
        return;
      }

      showVariableStatus(`${currentVariables.length} Variablen geladen.`, false);
      currentVariables.forEach(variable => {
        let displayValue = '';
        if (variable.type === 'brightness') {
          displayValue = `${variable.value}/255`;
        } else if (variable.type === 'duration') {
          displayValue = `${variable.value}ms`;
        } else if (variable.type === 'led_mask') {
          displayValue = `mask:${variable.value}`;
        } else {
          displayValue = variable.hex || `RGB(${variable.r},${variable.g},${variable.b})`;
        }

        const item = document.createElement('div');
        item.className = 'variable-item';
        item.innerHTML = `
          <div class="variable-item-info">
            <div class="variable-name">$${escapeHtml(variable.name)}</div>
            <div class="variable-type">${escapeHtml(variable.type)}</div>
          </div>
          <div class="variable-value">${escapeHtml(displayValue)}</div>
          <div class="variable-item-actions">
            <button onclick="editVariable('${encodeURIComponent(variable.name)}')" type="button">Bearbeiten</button>
            <button onclick="deleteVariable('${encodeURIComponent(variable.name)}')" type="button">Löschen</button>
          </div>
        `;
        container.appendChild(item);
      });
    }

    function collectVariablePayload() {
      const name = document.getElementById('varName').value.trim();
      const type = document.getElementById('varType').value;
      const payload = { name, type, value: 0 };

      if (type === 'brightness') {
        payload.value = parseInt(document.getElementById('varValueSlider').value, 10) || 0;
      } else if (type === 'duration') {
        const seconds = parseInt(document.getElementById('varDurationSec').value, 10) || 0;
        const millis = parseInt(document.getElementById('varDurationMs').value, 10) || 0;
        payload.value = seconds * 1000 + millis;
      } else if (type === 'led_mask') {
        payload.value = getLEDMask();
      } else if (type === 'color') {
        payload.hex = document.getElementById('varColorHex').value.trim();
      }

      return payload;
    }

    async function refreshVariables() {
      try {
        const response = await fetch('/variables');
        const data = await response.json();
        currentVariables = Array.isArray(data.variables) ? data.variables : [];
        renderVariables();
      } catch (error) {
        showVariableStatus('Variablen konnten nicht geladen werden.', true);
        console.log(error);
      }
    }

    async function saveVariable() {
      const payload = collectVariablePayload();
      if (!payload.name) {
        showVariableStatus('Variablenname erforderlich.', true);
        return;
      }

      try {
        const response = await fetch('/variables', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(payload)
        });
        const data = await response.json();
        if (!response.ok) {
          showVariableStatus(data.error || 'Variable konnte nicht gespeichert werden.', true);
          return;
        }
        currentVariables = Array.isArray(data.variables) ? data.variables : [];
        renderVariables();
        showVariableStatus(`Variable $${payload.name} gespeichert.`, false);
        resetVariableForm();
      } catch (error) {
        showVariableStatus('Speichern fehlgeschlagen.', true);
        console.log(error);
      }
    }

    function editVariable(encodedName) {
      const name = decodeURIComponent(encodedName);
      const variable = currentVariables.find(entry => entry.name === name);
      if (!variable) {
        showVariableStatus('Variable nicht gefunden.', true);
        return;
      }
      editingVariableName = name;
      document.getElementById('varName').value = variable.name;
      document.getElementById('varType').value = variable.type;
      document.getElementById('variableSubmitButton').textContent = 'Variable aktualisieren';
      updateVariableInput(variable);
      showVariableStatus(`Variable $${name} zur Bearbeitung geladen.`, false);
    }

    async function deleteVariable(encodedName) {
      const name = decodeURIComponent(encodedName);
      if (!confirm(`Variable "${name}" löschen?`)) {
        return;
      }

      try {
        const response = await fetch('/variables?name=' + encodeURIComponent(name), { method: 'DELETE' });
        const data = await response.json();
        if (!response.ok) {
          showVariableStatus(data.error || 'Variable konnte nicht gelöscht werden.', true);
          return;
        }
        currentVariables = Array.isArray(data.variables) ? data.variables : [];
        renderVariables();
        if (editingVariableName === name) {
          resetVariableForm();
        }
        showVariableStatus(`Variable $${name} gelöscht.`, false);
      } catch (error) {
        showVariableStatus('Löschen fehlgeschlagen.', true);
        console.log(error);
      }
    }

    document.getElementById('scan-trigger').addEventListener('click', refreshScan);
    setupPasswordToggle();
    setupOtaCheck();
    refreshStatus();
    setInterval(refreshStatus, 3000);

    // V2 Variables UI Initialization
    resetVariableForm();
    refreshVariables();
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
  webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.sendHeader("Expires", "-1");
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  sendLogicPageStreamed();
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

String sanitizeScriptName(const String& name) {
  String out;
  for (size_t i = 0; i < name.length() && out.length() < kScriptMaxNameLen; ++i) {
    const char c = name[i];
    if (isAlphaNumeric(c) || c == ' ' || c == '-' || c == '_') {
      out += c;
    }
  }
  out.trim();
  return out;
}

String scriptFilePath(const String& displayName) {
  String safe = sanitizeScriptName(displayName);
  for (size_t i = 0; i < safe.length(); ++i) {
    if (safe[i] == ' ') safe[i] = '_';
  }
  return String(kScriptDir) + "/" + safe + ".json";
}

void handleScriptsList() {
  if (!LittleFS.exists(kScriptDir)) {
    webServer.send(200, "application/json", "{\"scripts\":[]}");
    return;
  }
  File dir = LittleFS.open(kScriptDir);
  if (!dir || !dir.isDirectory()) {
    webServer.send(200, "application/json", "{\"scripts\":[]}");
    return;
  }
  String json = "{\"scripts\":[";
  bool first = true;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String fullPath = f.name();
      const int slashPos = fullPath.lastIndexOf('/');
      String fname = (slashPos >= 0) ? fullPath.substring(slashPos + 1) : fullPath;
      if (fname.endsWith(".json")) {
        String displayName = fname.substring(0, fname.length() - 5);
        for (size_t i = 0; i < displayName.length(); ++i) {
          if (displayName[i] == '_') displayName[i] = ' ';
        }
        if (!first) json += ",";
        json += "\"" + jsonEscape(displayName) + "\"";
        first = false;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  json += "]}";
  webServer.send(200, "application/json", json);
}

void handleScriptSlotSave() {
  if (!webServer.hasArg("name") || !webServer.hasArg("plain")) {
    webServer.send(400, "text/plain", "name oder Body fehlt");
    return;
  }
  const String sanitized = sanitizeScriptName(webServer.arg("name"));
  if (sanitized.isEmpty()) {
    webServer.send(400, "text/plain", "Ungültiger Name");
    return;
  }
  const String json = webServer.arg("plain");
  if (json.length() > kMaxScriptJsonLen) {
    webServer.send(413, "text/plain", "Script zu gross");
    return;
  }
  if (!validateScriptJson(json)) {
    webServer.send(400, "text/plain", "Script ungültig");
    return;
  }
  if (!LittleFS.exists(kScriptDir)) {
    LittleFS.mkdir(kScriptDir);
  }
  const String path = scriptFilePath(sanitized);
  File f = LittleFS.open(path, "w");
  if (!f) {
    webServer.send(500, "text/plain", "Speichern fehlgeschlagen");
    return;
  }
  f.print(json);
  f.close();
  appendLog("Script in Bibliothek gespeichert: " + sanitized);
  webServer.send(204, "text/plain", "");
}

void handleScriptSlotLoad() {
  if (!webServer.hasArg("name")) {
    webServer.send(400, "text/plain", "name fehlt");
    return;
  }
  const String sanitized = sanitizeScriptName(webServer.arg("name"));
  if (sanitized.isEmpty()) {
    webServer.send(400, "text/plain", "Ungültiger Name");
    return;
  }
  const String path = scriptFilePath(sanitized);
  if (!LittleFS.exists(path)) {
    webServer.send(404, "text/plain", "Script nicht gefunden");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    webServer.send(500, "text/plain", "Laden fehlgeschlagen");
    return;
  }
  webServer.streamFile(f, "application/json");
  f.close();
}

void handleScriptSlotDelete() {
  if (!webServer.hasArg("name")) {
    webServer.send(400, "text/plain", "name fehlt");
    return;
  }
  const String sanitized = sanitizeScriptName(webServer.arg("name"));
  if (sanitized.isEmpty()) {
    webServer.send(400, "text/plain", "Ungültiger Name");
    return;
  }
  const String path = scriptFilePath(sanitized);
  if (!LittleFS.exists(path)) {
    webServer.send(404, "text/plain", "Script nicht gefunden");
    return;
  }
  LittleFS.remove(path);
  appendLog("Script aus Bibliothek gelöscht: " + sanitized);
  webServer.send(204, "text/plain", "");
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
    webServer.send(400, "text/plain", "Script ungültig");
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
    webServer.send(400, "text/plain", "Script ungültig");
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
  appendLog("Script gelöscht.");
  webServer.send(204, "text/plain", "");
}

void handleScriptCurrent() {
  if (savedScriptJson.isEmpty()) {
    webServer.send(204, "application/json", "");
    return;
  }
  webServer.send(200, "application/json", savedScriptJson);
}

void handleVariablesList() {
  webServer.send(200, "application/json", "{\"variables\":" + collectVariablesAsJson() + "}");
}

void handleVariablesUpsert() {
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "application/json", "{\"error\":\"Body fehlt\"}");
    return;
  }

  const String payload = webServer.arg("plain");
  const String name = extractJsonStr(payload, "name");
  const String typeRaw = extractJsonStr(payload, "type");
  const String valueRaw = extractJsonStr(payload, "value");
  const String hexRaw = extractJsonStr(payload, "hex");

  if (!isValidVariableName(name)) {
    webServer.send(400, "application/json", "{\"error\":\"Ungültiger Variablenname\"}");
    return;
  }

  VarType varType = VarType::kDuration;
  if (!parseVarType(typeRaw, varType)) {
    webServer.send(400, "application/json", "{\"error\":\"Ungültiger Variablentyp\"}");
    return;
  }

  bool ok = false;
  if (varType == VarType::kColor) {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    ok = parseHexColor(hexRaw.isEmpty() ? valueRaw : hexRaw, r, g, b) &&
         createOrUpdateVariable(name, varType, 0, r, g, b);
  } else {
    long numericValue = valueRaw.toInt();
    if (varType == VarType::kBrightness) {
      numericValue = constrain(numericValue, 0L, 255L);
    } else if (varType == VarType::kLedMask) {
      numericValue = constrain(numericValue, 0L, static_cast<long>(allLedMask()));
    } else {
      numericValue = constrain(numericValue, 0L, 65535L);
    }
    ok = createOrUpdateVariable(name, varType, static_cast<uint16_t>(numericValue));
  }

  if (!ok) {
    webServer.send(500, "application/json", "{\"error\":\"Variable konnte nicht gespeichert werden\"}");
    return;
  }

  persistVariables();
  appendLog("Variable gespeichert: $" + name);
  webServer.send(200, "application/json", "{\"ok\":true,\"variables\":" + collectVariablesAsJson() + "}");
}

void handleVariablesDelete() {
  if (!webServer.hasArg("name")) {
    webServer.send(400, "application/json", "{\"error\":\"name fehlt\"}");
    return;
  }

  const String name = webServer.arg("name");
  if (!deleteVariable(name)) {
    webServer.send(404, "application/json", "{\"error\":\"Variable nicht gefunden\"}");
    return;
  }

  persistVariables();
  appendLog("Variable gelöscht: $" + name);
  webServer.send(200, "application/json", "{\"ok\":true,\"variables\":" + collectVariablesAsJson() + "}");
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

  const bool updateAvailable = !remoteVersion.isEmpty() &&
                               compareVersionsAlphabetical(remoteVersion, firmwareVersion) > 0;

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

  otaUploadResult = "Upload erfolgreich, Neustart wird ausgeführt.";
  appendLog("OTA Upload erfolgreich.");
  otaRebootPending = true;
  otaRebootAtMs = millis() + 1500;
  webServer.send(200, "text/plain", otaUploadResult);
}

void handleOtaUploadData() {
  HTTPUpload& upload = webServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    appendLog("OTA Upload gestartet: " + upload.filename);
    otaUploadResult = "Upload läuft";
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
  webServer.send_P(200, "image/png", reinterpret_cast<const char*>(led_icon_75_png), led_icon_75_png_len);
}

void handleFaviconIco() {
  webServer.sendHeader("Cache-Control", "public, max-age=86400");
  webServer.send_P(200, "image/x-icon", reinterpret_cast<const char*>(led_favicon_ico), led_favicon_ico_len);
}

void handleSetupSaveIconPng() {
  webServer.sendHeader("Cache-Control", "public, max-age=86400");
  webServer.send_P(200, "image/png", reinterpret_cast<const char*>(save_script_16_png), save_script_16_png_len);
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
  webServer.on(kLedLogicFaviconUrl, HTTP_GET, handleFaviconIco);
  webServer.on(kSetupSaveIconUrl, HTTP_GET, handleSetupSaveIconPng);
  webServer.on("/script/save", HTTP_POST, handleScriptSave);
  webServer.on("/script/run", HTTP_POST, handleScriptRun);
  webServer.on("/script/stop", HTTP_POST, handleScriptStop);
  webServer.on("/script/clear", HTTP_POST, handleScriptClear);
  webServer.on("/script/current", HTTP_GET, handleScriptCurrent);
  webServer.on("/variables", HTTP_GET, handleVariablesList);
  webServer.on("/variables", HTTP_POST, handleVariablesUpsert);
  webServer.on("/variables", HTTP_DELETE, handleVariablesDelete);
  webServer.on("/scripts/list", HTTP_GET, handleScriptsList);
  webServer.on("/scripts/slot/save", HTTP_POST, handleScriptSlotSave);
  webServer.on("/scripts/slot/load", HTTP_GET, handleScriptSlotLoad);
  webServer.on("/scripts/slot/delete", HTTP_POST, handleScriptSlotDelete);
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

  if (!LittleFS.begin(true)) {
    appendLog("LittleFS: Initialisierung fehlgeschlagen.");
  } else {
    LittleFS.mkdir(kScriptDir);
  }

  initWs2812Rmt();
  // Mehrfach senden, damit beim Boot sicher ein OFF-Frame anliegt.
  for (uint8_t i = 0; i < 3; ++i) {
    ws2812Show();
    delay(5);
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(kDeviceHostname);
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

  // Advance script interpreter every loop tick.
  tickScript();

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
