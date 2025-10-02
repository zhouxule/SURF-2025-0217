/*
 *   __   __    _ _______ _     _    _        _____ _    _ _____  ______    ___   ___ ___  _____        ___ ___  __ ______ 
 *   \ \ / /   | |__   __| |   | |  | |      / ____| |  | |  __ \|  ____|  |__ \ / _ \__ \| ____|      / _ \__ \/_ |____  |
 *    \ V /    | |  | |  | |   | |  | |_____| (___ | |  | | |__) | |__ ______ ) | | | | ) | |__ ______| | | | ) || |   / / 
 *     > < _   | |  | |  | |   | |  | |______\___ \| |  | |  _  /|  __|______/ /| | | |/ /|___ \______| | | |/ / | |  / /  
 *    / . \ |__| |  | |  | |___| |__| |      ____) | |__| | | \ \| |        / /_| |_| / /_ ___) |     | |_| / /_ | | / /   
 *   /_/ \_\____/   |_|  |______\____/      |_____/ \____/|_|  \_\_|       |____|\___/____|____/       \___/____||_|/_/    
 *                                                                                                                         
 *                                                                                                                         
 */

#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#include <AsyncMqttClient.h>
#include <esp_wifi.h>

// For AHT20 sensor
#include <Wire.h>
#include <Adafruit_AHTX0.h>

// ESP32 AP (Soft-AP) Mode
AsyncWebServer server(80);
const char* AP_SSID = "SURF-2025-0217";
const char* AP_PASS = "20250217";
const char* CONFIG_FILE = "/config.json";
const char* CSV_FILE = "/log.csv";

AsyncMqttClient mqttClient;

struct Config {
  String ssid = "";
  String password = "";
  int baud = 115200;
  String mqtt_server = "";
  int mqtt_port = 8883;
  String mqtt_user = "";
  String mqtt_pass = "";
  String mqtt_topic = "";
  String mac_address = "";
} config;

String latestLine = "";
String logs = "";
bool shouldRestart = false;
unsigned long restartAt = 0;

Adafruit_AHTX0 aht;

// Forward declarations
void connectToMqtt();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttPublish(uint16_t packetId);
void WiFiEvent(WiFiEvent_t event);


// Check input MAC address format
bool parseMacAddress(const String& macStr, uint8_t* macArray) {
    if (macStr.length() != 17) {
        return false;
    }
    int values[6];
    int count = sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    if (count == 6) {
        for (int i = 0; i < 6; ++i) {
            macArray[i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

// Load config.json from SPIFFS
void loadConfig() {
  if (!SPIFFS.exists(CONFIG_FILE)) return;
  File f = SPIFFS.open(CONFIG_FILE, "r");
  if (!f) return;
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  config.ssid = doc["ssid"] | "";
  config.password = doc["password"] | "";
  config.baud = doc["baud"] | 115200;
  config.mqtt_server = doc["mqtt_server"] | "";
  config.mqtt_port = doc["mqtt_port"] | 8883;
  config.mqtt_user = doc["mqtt_user"] | "";
  config.mqtt_pass = doc["mqtt_pass"] | "";
  config.mqtt_topic = doc["mqtt_topic"] | "";
  config.mac_address = doc["mac_address"] | "";
}

// Save config.json to SPIFFS
void saveConfig() {
  StaticJsonDocument<512> doc;
  doc["ssid"] = config.ssid;
  doc["password"] = config.password;
  doc["baud"] = config.baud;
  doc["mqtt_server"] = config.mqtt_server;
  doc["mqtt_port"] = config.mqtt_port;
  doc["mqtt_user"] = config.mqtt_user;
  doc["mqtt_pass"] = config.mqtt_pass;
  doc["mqtt_topic"] = config.mqtt_topic;
  doc["mac_address"] = config.mac_address;
  File f = SPIFFS.open(CONFIG_FILE, "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}

// NTP time sync
void setupTime() {
    // Set three servers to increase success rate
    const char* ntpServer1 = "cn.pool.ntp.org";
    const char* ntpServer2 = "ntp.aliyun.com";
    const char* ntpServer3 = "ntp.tuna.tsinghua.edu.cn";

    configTime(8 * 3600, 0, ntpServer1, ntpServer2, ntpServer3);

    struct tm timeinfo;

    if (getLocalTime(&timeinfo, 5000)) { 
        char buf[32];
        strftime(buf, sizeof(buf), "%F %T", &timeinfo);
        logs += "NTP time sync success: ";
        logs += buf;
        logs += "\n";
    } else {
        logs += "NTP time sync failed after 5s\n";
    }
}

/*
void setupTime() {
  configTime(8 * 3600, 0, "cn.pool.ntp.org");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%F %T", &timeinfo);
    logs += "NTP time sync success: ";
    logs += buf;
    logs += "\n";
  } else {
    logs += "NTP time sync failed\n";
  }
}
*/

String getNowTime() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long seconds = millis() / 1000;
    char buf[20];
    sprintf(buf, "T+%08lu", seconds);
    return String(buf);
  }

  struct tm timeinfo;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (getLocalTime(&timeinfo)) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03lu",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, tv.tv_usec / 1000);
    return String(buf);
  }
  return "1970-01-01 00:00:00.000";
}

// AsyneMQTT Handlers
void connectToMqtt() {
  if (config.mqtt_server.length() > 0) {
    logs += "Attempting MQTT connection...\n";
    mqttClient.connect();
  }
}

void onMqttConnect(bool sessionPresent) {
  logs += "MQTT connected.\n";
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  logs += "MQTT disconnected. Reason: ";
  if (reason == AsyncMqttClientDisconnectReason::TCP_DISCONNECTED) {
    logs += "TCP disconnected";
  } else if (reason == AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION) {
    logs += "Unacceptable protocol version";
  } else if (reason == AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED) {
    logs += "Identifier rejected";
  } else if (reason == AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE) {
    logs += "Server unavailable";
  } else if (reason == AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED) {
    logs += "Not authorized (check user/pass)";
  } else {
    logs += "Unknown";
  }
  logs += ". Will retry automatically.\n";
}

void onMqttPublish(uint16_t packetId) {
  logs += "MQTT publish acknowledged for packet ID: " + String(packetId) + "\n";
}

void WiFiEvent(WiFiEvent_t event) {
    switch(event) {
        case SYSTEM_EVENT_STA_GOT_IP:
            logs += "WiFi connected: " + WiFi.localIP().toString() + "\n";
            setupTime();
            connectToMqtt();
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            logs += "WiFi lost connection. Reconnecting...\n";
            break;
        default:
            break;
    }
}


void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin();

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  if (!aht.begin()) {
    logs += "Error: Could not find AHT20 sensor!\n";
  } else {
    logs += "AHT20 sensor found.\n";
  }

  loadConfig();

  if (Serial.baudRate() != config.baud) {
    Serial.end();
    Serial.begin(config.baud);
  }

  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_AP_STA);

  if (config.mac_address.length() > 0) {
    uint8_t newMac[6];
    if (parseMacAddress(config.mac_address, newMac)) {
      esp_wifi_set_mac(WIFI_IF_STA, newMac);
      logs += "Custom MAC address set for WiFi STA: " + config.mac_address + "\n";
    } else {
      logs += "Error: Invalid MAC address format in config. Using default MAC.\n";
    }
  }

  WiFi.softAP(AP_SSID, AP_PASS);
  logs += "AP started: " + WiFi.softAPIP().toString() + "\n";

  // Configure MQTT client details
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.setServer(config.mqtt_server.c_str(), config.mqtt_port);
  mqttClient.setCredentials(config.mqtt_user.c_str(), config.mqtt_pass.c_str());

  // The library automatically uses TLS for port 8883.
  // By not setting a CA certificate, it will skip server validation (insecure).

  if (config.ssid.length() > 0) {
      logs += "Connecting to WiFi: " + config.ssid + "\n";
      WiFi.begin(config.ssid.c_str(), config.password.c_str());
  }

  // Initialize csv file
  if (!SPIFFS.exists(CSV_FILE)) {
    File f = SPIFFS.open(CSV_FILE, FILE_WRITE);
    if (f) {
      f.println("Time,SerialData,Temperature,Humidity");
      f.close();
    }
  }

  // Web Server HTML
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>SURF-2025-0217</title>
<style>
body { font-family: Arial, sans-serif; margin:20px; }
textarea,input,select { width: 100%; margin: 5px 0; box-sizing: border-box; }
</style>
<script>
function fetchData() {
  fetch('/data').then(r => r.text()).then(t => { document.getElementById('dataBox').value = t; });
  fetch('/logs').then(r => r.text()).then(t => { document.getElementById('logBox').value = t; });
}
function loadConfig() {
  fetch('/getconfig').then(r => r.json()).then(cfg => {
    document.getElementById('ssid').value = cfg.ssid || '';
    document.getElementById('password').value = cfg.password || '';
    document.getElementById('mac_address').value = cfg.mac_address || '';
    document.getElementById('baud').value = cfg.baud || '115200';
    document.getElementById('mqtt_server').value = cfg.mqtt_server || '';
    document.getElementById('mqtt_port').value = cfg.mqtt_port || 8883;
    document.getElementById('mqtt_user').value = cfg.mqtt_user || '';
    document.getElementById('mqtt_pass').value = cfg.mqtt_pass || '';
    document.getElementById('mqtt_topic').value = cfg.mqtt_topic || '';
  });
}
function saveConfig() {
  const mac = document.getElementById('mac_address').value;
  if (mac && !/^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/.test(mac)) {
    alert('Error: Invalid MAC address format.\nPlease use XX:XX:XX:XX:XX:XX format.');
    return;
  }
  const cfg = {
    ssid: document.getElementById('ssid').value,
    password: document.getElementById('password').value,
    mac_address: mac,
    baud: parseInt(document.getElementById('baud').value),
    mqtt_server: document.getElementById('mqtt_server').value,
    mqtt_port: parseInt(document.getElementById('mqtt_port').value),
    mqtt_user: document.getElementById('mqtt_user').value,
    mqtt_pass: document.getElementById('mqtt_pass').value,
    mqtt_topic: document.getElementById('mqtt_topic').value
  };

  fetch('/setconfig', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify(cfg)
  }).then(response => {
    if (response.ok) {
        return response.text();
    } else {
        response.text().then(text => alert('Failed to save config: ' + text));
        throw new Error('Save failed');
    }
  }).then(html => {
    if(html) document.body.innerHTML = html;
  }).catch(error => console.error(error));
}
setInterval(fetchData, 1000);
window.onload = () => {
  loadConfig();
  fetchData();
};
</script>
</head>
<body>
<h2>串口数据 Serial Port Data</h2>
<textarea id="dataBox" rows="10" readonly></textarea>
<h2>日志 Log</h2>
<textarea id="logBox" rows="10" readonly></textarea>
<button onclick="location.href='/download'">Download CSV</button>
<button onclick="fetch('/delete').then(()=>alert('CSV deleted'));">Delete CSV</button>
<hr>
<h2>配置 Configuration</h2>
<form id="configForm" onsubmit="saveConfig();return false;">
WiFi SSID:<input type="text" id="ssid"><br>
WiFi Password:<input type="password" id="password"><br>
Custom MAC Address (Station):<input type="text" id="mac_address" placeholder="e.g., 00:11:22:33:44:55"><br>
Baud Rate:
<select id="baud">
  <option value="9600">9600</option>
  <option value="19200">19200</option>
  <option value="38400">38400</option>
  <option value="57600">57600</option>
  <option value="115200" selected>115200</option>
</select><br>
MQTT Server:<input type="text" id="mqtt_server"><br>
MQTT Port:<input type="number" id="mqtt_port" min="1" max="65535"><br>
MQTT User:<input type="text" id="mqtt_user"><br>
MQTT Password:<input type="password" id="mqtt_pass"><br>
MQTT Topic:<input type="text" id="mqtt_topic"><br>
<button type="submit">Save</button>
</form>
</body>
</html>
)rawliteral";
    request->send(200, "text/html; charset=utf-8", page);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", latestLine);
  });

  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", logs);
  });

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists(CSV_FILE)) {
      request->send(SPIFFS, CSV_FILE, "text/csv");
    } else {
      request->send(404, "text/plain", "CSV not found");
    }
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists(CSV_FILE)) SPIFFS.remove(CSV_FILE);
    File f = SPIFFS.open(CSV_FILE, FILE_WRITE);
    if (f) {
      f.println("Time,SerialData,Temperature,Humidity");
      f.close();
    }
    logs += "CSV deleted and rebuilt\n";
    request->send(200, "text/plain", "CSV deleted");
  });

  server.on("/getconfig", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<512> doc;
    doc["ssid"] = config.ssid;
    doc["password"] = config.password;
    doc["baud"] = config.baud;
    doc["mqtt_server"] = config.mqtt_server;
    doc["mqtt_port"] = config.mqtt_port;
    doc["mqtt_user"] = config.mqtt_user;
    doc["mqtt_pass"] = config.mqtt_pass;
    doc["mqtt_topic"] = config.mqtt_topic;
    
    if (config.mac_address.length() > 0) {
        doc["mac_address"] = config.mac_address;
    } else {
        doc["mac_address"] = WiFi.macAddress();
    }
    
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on(
    "/setconfig",
    HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, (const char*)data, len) == DeserializationError::Ok) {
            String mac_str = doc["mac_address"] | "";
            mac_str.trim();

            uint8_t temp_mac[6];
            if (mac_str.length() > 0 && !parseMacAddress(mac_str, temp_mac)) {
                request->send(400, "text/plain", "Invalid MAC address format on server.");
                return;
            }

            config.ssid = doc["ssid"].as<String>();
            config.password = doc["password"].as<String>();
            config.mac_address = mac_str;
            config.baud = doc["baud"].as<int>();
            config.mqtt_server = doc["mqtt_server"].as<String>();
            config.mqtt_port = doc["mqtt_port"].as<int>();
            config.mqtt_user = doc["mqtt_user"].as<String>();
            config.mqtt_pass = doc["mqtt_pass"].as<String>();
            config.mqtt_topic = doc["mqtt_topic"].as<String>();
            
            saveConfig();
            
            shouldRestart = true;
            restartAt = millis() + 3000;

            const char* savedPage = "<html><meta charset='UTF-8'><body><h2>配置保存成功，设备3秒后重启...Configuration saved successfully, reboot in 3 seconds...</h2></body></html>";
            request->send(200, "text/html; charset=utf-8", savedPage);

        } else {
            request->send(400, "text/plain", "Invalid JSON");
        }
      }
    }
  );

  server.begin();
}


void loop() {
  if (shouldRestart && millis() > restartAt) {
    ESP.restart();
  }

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length()) {
      String timeStr = getNowTime();

      sensors_event_t humidity, temp;
      float temp_val = 0.0;
      float hum_val = 0.0;
      if (aht.getEvent(&humidity, &temp)) {
        temp_val = temp.temperature;
        hum_val = humidity.relative_humidity;
      } else {
        logs += "Failed to read AHT20 sensor.\n";
      }

      String csvLine = timeStr + "," + line + "," + String(temp_val) + "," + String(hum_val);
      latestLine = csvLine;
      logs += "Recorded: " + csvLine + "\n";

      File f = SPIFFS.open(CSV_FILE, FILE_APPEND);
      if (f) {
        f.println(csvLine);
        f.close();
      } else {
        logs += "Failed to open log.csv for writing.\n";
      }

      if (mqttClient.connected()) {
        StaticJsonDocument<512> doc;
        doc["time"] = timeStr;
        doc["data"] = line;
        doc["temperature"] = temp_val;
        doc["humidity"] = hum_val;
        
        String payload;
        serializeJson(doc, payload);

        mqttClient.publish(config.mqtt_topic.c_str(), 0, false, payload.c_str());
      }
    }
  }
}