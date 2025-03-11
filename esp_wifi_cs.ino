#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUDP.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

ESP8266WebServer server(80);
WiFiUDP udp;

// MAC地址存储结构
struct {
  byte mac[6];
  bool configured;
} macConfig;

const char* ssid = ""; // Wifi ssid
const char* password = ""; // Wifi password
const char* esp_id = "esp001";
const char* server_host = "";
const int server_port = -1;

const char* device_description = "Test Device 1";
const char* device_password = "123456";

WebSocketsClient webSocket;

// 网页HTML
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>远程开机控制</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 20px;
      background-color: #f0f0f0;
    }
    .card {
      background-color: white;
      padding: 20px;
      border-radius: 10px;
      box-shadow: 0 2px 5px rgba(0,0,0,0.1);
      margin: 20px auto;
      max-width: 600px;
    }
    .button {
      background-color: #4CAF50;
      border: none;
      color: white;
      padding: 15px 32px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 16px;
      margin: 4px 2px;
      cursor: pointer;
      border-radius: 4px;
      width: 100%;
    }
    .button.config {
      background-color: #2196F3;
    }
    .input-group {
      margin: 10px 0;
    }
    input[type="text"] {
      width: 100%;
      padding: 8px;
      margin: 5px 0;
      box-sizing: border-box;
      border: 1px solid #ddd;
      border-radius: 4px;
    }
    .status {
      margin: 10px 0;
      padding: 10px;
      border-radius: 4px;
      background-color: #f8f8f8;
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>远程开机控制</h1>
    <div class="status">
      状态: <span id="status">就绪</span><br>
      当前MAC: <span id="currentMac">未设置</span><br>
      上次操作: <span id="lastWake">无</span><br>
      控制板IP: <span id="boardIP">--</span>
    </div>
    
    <div class="input-group">
      <input type="text" id="macInput" placeholder="输入MAC地址 (格式: 00-11-22-33-44-55)">
      <button class="button config" onclick="saveMac()">保存MAC地址</button>
    </div>
    
    <button class="button" onclick="wakePC()">开机</button>
  </div>

  <script>
    window.onload = function() {
      fetchCurrentMac();
      updateNetworkInfo();
    };

    function updateNetworkInfo() {
      fetch('/networkinfo')
        .then(response => response.json())
        .then(data => {
          document.getElementById('boardIP').innerText = data.ip;
        });
    }

    function fetchCurrentMac() {
      fetch('/getmac')
        .then(response => response.text())
        .then(data => {
          document.getElementById('currentMac').innerText = data;
        });
    }

    function saveMac() {
      const mac = document.getElementById('macInput').value;
      if (!validateMac(mac)) {
        alert('请输入正确格式的MAC地址！');
        return;
      }

      fetch('/setmac?mac=' + mac)
        .then(response => response.text())
        .then(data => {
          alert('MAC地址已保存');
          fetchCurrentMac();
        });
    }

    function validateMac(mac) {
      return /^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/.test(mac);
    }

    function wakePC() {
      document.getElementById('status').innerText = '发送唤醒指令...';
      
      fetch('/wake')
        .then(response => response.json())
        .then(data => {
          document.getElementById('status').innerText = '唤醒指令已发送';
          document.getElementById('lastWake').innerText = new Date().toLocaleString();
          document.getElementById('boardIP').innerText = data.boardIP;
        })
        .catch(error => {
          document.getElementById('status').innerText = '发送失败';
        });
    }
  </script>
</body>
</html>
)rawliteral";

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    StaticJsonDocument<200> doc;
    String message;
    bool success;
    
    WiFiClient client;
    HTTPClient http;
    String url;
    int httpCode;
    
    switch(type) {
        case WStype_ERROR:
            Serial.println("WebSocket错误！");
            Serial.printf("错误信息: %s\n", (char*)payload);
            webSocket.disconnect();
            delay(5000);
            webSocket.begin(server_host, server_port, String("/ws?esp_id=" + String(esp_id)).c_str());
            break;
            
        case WStype_DISCONNECTED:
            Serial.println("WebSocket未连接，15秒后重试...");
            delay(15000);
            webSocket.begin(server_host, server_port, String("/ws?esp_id=" + String(esp_id)).c_str());
            break;
            
        case WStype_CONNECTED:
            Serial.print("WebSocket已连接到: ");
            Serial.println(server_host);
            
            delay(1000);
            
            url = String("http://") + server_host + ":" + String(server_port) + "/register";
            
            Serial.printf("尝试连接到: %s\n", url.c_str());
            
            if (!http.begin(client, url)) {
                Serial.println("HTTP begin失败!");
                return;
            }
            
            http.addHeader("Content-Type", "application/json");
            
            // 构建注册消息
            doc.clear();
            doc["esp_id"] = esp_id;
            
            if (macConfig.configured) {
                char macStr[18];
                sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                    macConfig.mac[0], macConfig.mac[1], macConfig.mac[2],
                    macConfig.mac[3], macConfig.mac[4], macConfig.mac[5]);
                doc["mac_address"] = String(macStr);
            } else {
                doc["mac_address"] = "未设置";
            }
            
            doc["description"] = device_description;
            doc["password"] = device_password;
            
            serializeJson(doc, message);
            
            // 发送POST请求
            Serial.printf("正在发送POST请求...\n");
            httpCode = http.POST(message);
            
            Serial.printf("发送注册请求到 %s\n", url.c_str());
            Serial.printf("注册消息: %s\n", message.c_str());
            Serial.printf("HTTP响应码: %d\n", httpCode);
            
            if (httpCode > 0) {
                String payload = http.getString();
                Serial.printf("服务器响应: %s\n", payload.c_str());
            } else {
                Serial.printf("HTTP错误: %s\n", http.errorToString(httpCode).c_str());
            }
            
            http.end();
            break;
            
        case WStype_TEXT:
            Serial.print("收到消息: ");
            Serial.println((char*)payload);
            handleMessage((char*)payload);
            break;
            
        case WStype_PING:
            Serial.println("收到Ping");
            break;
            
        case WStype_PONG:
            Serial.println("收到Pong");
            break;
    }
}

void handleMessage(char* payload) {
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
        Serial.println("解析JSON失败");
        return;
    }

    const char* type = doc["type"];
    
    if (strcmp(type, "register_ack") == 0) {
        Serial.println("注册确认已收到");
        if (doc["status"] == "success") {
            Serial.println("注册成功");
        } else {
            Serial.println("注册失败");
        }
    } else if (strcmp(type, "wake") == 0) {
        Serial.println("收到远程唤醒命令");
        const char* mac_address = doc["mac_address"];
        if (mac_address) {
            Serial.printf("目标MAC地址: %s\n", mac_address);
        }
        bool result = sendWOL();
        
        StaticJsonDocument<200> response;
        response["type"] = "wake_response";
        response["status"] = result ? "success" : "failed";
        
        String responseMsg;
        serializeJson(response, responseMsg);
        webSocket.sendTXT(responseMsg);
    } else {
        Serial.println("未知消息类型: " + String(type));
    }
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  EEPROM.get(0, macConfig);
  
  Serial.println("正在连接WiFi...");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi已连接");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());
  
  // 尝试解析域名
  Serial.print("正在解析域名: ");
  Serial.println(server_host);
  IPAddress serverIP;
  if (WiFi.hostByName(server_host, serverIP)) {
    Serial.print("域名解析成功，IP: ");
    Serial.println(serverIP);
  } else {
    Serial.println("域名解析失败！");
  }
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wake", HTTP_GET, handleWake);
  server.on("/setmac", HTTP_GET, handleSetMac);
  server.on("/getmac", HTTP_GET, handleGetMac);
  server.on("/networkinfo", HTTP_GET, handleNetworkInfo);
  
  server.begin();

  Serial.println("正在配置WebSocket...");
  String ws_url = "/ws?esp_id=";
  ws_url += esp_id;
  
  Serial.print("WebSocket URL: ws://");
  Serial.print(server_host);
  Serial.print(":");
  Serial.print(server_port);
  Serial.println(ws_url);
  
  webSocket.begin(server_host, server_port, ws_url.c_str());
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  
  Serial.println("设置完成，等待WebSocket连接...");
}

void loop() {
    server.handleClient();
    webSocket.loop();
    
    static unsigned long lastPing = 0;
    static unsigned long lastReconnectCheck = 0;
    
    // 每30秒发送一次ping
    if (millis() - lastPing > 30000) {
        lastPing = millis();
        if (webSocket.isConnected()) {
            webSocket.sendPing();
            Serial.println("发送Ping...");
        }
    }
    
    // 每60秒检查一次连接状态
    if (millis() - lastReconnectCheck > 60000) {
        lastReconnectCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi连接已断开，尝试重连...");
            WiFi.begin(ssid, password);
        }
        
        // 检查WebSocket连接状态
        if (!webSocket.isConnected()) {
            Serial.println("WebSocket未连接，尝试重连...");
            webSocket.begin(server_host, server_port, String("/ws?esp_id=" + String(esp_id)).c_str());
        }
    }
}

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleNetworkInfo() {
  StaticJsonDocument<200> doc;
  doc["ip"] = WiFi.localIP().toString();
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleWake() {
  if (!macConfig.configured) {
    server.send(400, "text/plain", "请先设置MAC地址");
    return;
  }
  
  StaticJsonDocument<200> response;
  response["boardIP"] = WiFi.localIP().toString();
  
  bool success = sendWOL();
  
  String jsonResponse;
  serializeJson(response, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

void handleSetMac() {
  String macStr = server.arg("mac");
  if (macStr.length() != 17) {
    server.send(400, "text/plain", "Invalid MAC format");
    return;
  }
  
  byte tmpMac[6];
  char *ptr = strdup(macStr.c_str());
  char *p = strtok(ptr, ":-");
  for(int i = 0; i < 6; i++) {
    if(p == NULL) {
      free(ptr);
      server.send(400, "text/plain", "Invalid MAC format");
      return;
    }
    tmpMac[i] = strtol(p, NULL, 16);
    p = strtok(NULL, ":-");
  }
  free(ptr);
  
  memcpy(macConfig.mac, tmpMac, 6);
  macConfig.configured = true;
  EEPROM.put(0, macConfig);
  EEPROM.commit();
  
  server.send(200, "text/plain", "MAC saved");
}

void handleGetMac() {
  if (!macConfig.configured) {
    server.send(200, "text/plain", "未设置");
    return;
  }
  
  char macStr[18];
  sprintf(macStr, "%02X-%02X-%02X-%02X-%02X-%02X",
          macConfig.mac[0], macConfig.mac[1], macConfig.mac[2],
          macConfig.mac[3], macConfig.mac[4], macConfig.mac[5]);
  
  server.send(200, "text/plain", macStr);
}

bool sendWOL() {
  byte magicPacket[102];
  memset(magicPacket, 0xFF, 6);
  
  // 打印目标MAC地址
  Serial.print("准备唤醒MAC地址: ");
  for(int i = 0; i < 6; i++) {
    Serial.printf("%02X", macConfig.mac[i]);
    if(i < 5) Serial.print(":");
  }
  Serial.println();
  
  // 构建魔术包
  for(int i = 1; i <= 16; i++) {
    memcpy(magicPacket + i * 6, macConfig.mac, 6);
  }
  
  // 计算广播地址
  IPAddress broadcastIP = WiFi.localIP();
  IPAddress subnetMask = WiFi.subnetMask();
  
  for (int i = 0; i < 4; i++) {
    broadcastIP[i] |= ~subnetMask[i];
  }
  
  Serial.print("广播地址: ");
  Serial.println(broadcastIP.toString());
  
  const int ports[] = {9, 7, 32767};
  bool success = false;
  
  // 发送Magic packet
  for(int retry = 0; retry < 3; retry++) {
    Serial.printf("尝试第 %d 次发送...\n", retry + 1);
    
    for(int port : ports) {
      udp.beginPacket(broadcastIP, port);
      udp.write(magicPacket, sizeof(magicPacket));
      bool sendResult = udp.endPacket();
      success |= sendResult;
      
      Serial.printf("发送到端口 %d: %s\n", port, sendResult ? "成功" : "失败");
      delay(100);
    }
    delay(100);
  }
  
  Serial.printf("唤醒包发送 %s\n", success ? "成功" : "失败");
  return success;
}