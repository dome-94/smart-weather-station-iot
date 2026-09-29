#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include "LoRa.h"
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>   // ★ MỚI: gửi HTTP lên ThingSpeak
#include <WiFiClient.h>          // ★ MỚI
#include <DNSServer.h>           // ★ MỚI: Thư viện chạy Captive Portal
#include "weather_html.h"        // ★ MỚI: Lưu toàn bộ HTML/CSS/JS ở tab này

// =====================
// Chân kết nối LoRa
// =====================
#define SS    D8
#define RST   D0
#define DIO0  D1
#define BUZZER_PIN D2 // Chân còi báo động (Active High)




// =====================
// Cấu hình WiFi Access Point (giữ nguyên — user cục bộ)
// =====================
const char* ssid = "WeatherStation";
const char* password = "12345678";
ESP8266WebServer server(80);

// =====================
// ★ MỚI: Cấu hình Captive Portal DNS Server
// =====================
const byte DNS_PORT = 53;
DNSServer dnsServer;

// =====================
// ★ MỚI: Cấu hình WiFi Station (để có Internet đẩy ThingSpeak)
// =====================
const char* staSsid     = "huynhnhat";      // ← ĐIỀN WiFi nhà bạn
const char* staPassword = "huynhnhat"; // ← ĐIỀN mật khẩu

// =====================
// ★ MỚI: Cấu hình ThingSpeak
// =====================
const char* THINGSPEAK_API_KEY = "IWSWJL8BLKHHGSG2"; // ← ĐIỀN Write API Key
const char* THINGSPEAK_HOST    = "api.thingspeak.com";
unsigned long lastTSsend = 0;
const unsigned long TS_MIN_INTERVAL_MS = 20000; // Đồng bộ tối thiểu 1 phút (60 giây) gửi 1 lần

// =====================
// Biến lưu trữ Dữ liệu thời tiết
// =====================
float temperature = 0;
float humidity = 0;
float pressure = 0;
int rain = 0;
uint8_t batteryPct = 0;   // ★ MỚI: lưu battery để đẩy lên ThingSpeak
int rssi = -72;           // ★ MỚI: lưu RSSI của sóng LoRa
uint8_t seqNum = 0;       // ★ MỚI: lưu Mã Gói (Sequence Number) nhận từ LoRa
bool alarmActive = false; // Trạng thái còi báo động (Hysteresis)
bool loraOnline = false;  // Trạng thái hoạt động của module LoRa (SX1278)
int offlineSentCount = 0; // Số dòng dữ liệu offline đã gửi bù thành công

// Biến quản lý WiFi tránh nghẽn sóng AP nội bộ
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 10000; // Kiểm tra trạng thái WiFi mỗi 10 giây
unsigned long disconnectTime = 0;
bool wasConnected = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 300000; // Thử kết nối lại sau mỗi 5 phút nếu mất kết nối

// ============================================================
// KHAI BÁO CẤU TRÚC GÓI TIN NHỊ PHÂN (Phải khớp 100% với Trạm Phát)
// ============================================================
#pragma pack(push, 1)
typedef struct {
  uint16_t node_id;
  uint8_t  seq_num;
  uint8_t  flags;
  int16_t  temperature;
  uint16_t humidity;
  uint16_t pressure;
  uint16_t rain_analog;  
  uint8_t  battery_pct;  
  uint16_t crc16;
} WeatherPkt_t;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
  uint16_t node_id;
  uint8_t  seq_num;
  uint8_t  ack_magic; // Phải bằng 0xAC
  uint16_t crc16;
} AckPkt_t;
#pragma pack(pop)

uint16_t crc16_ccitt(const uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}


// =====================
// Xử lý Web Root (Giao diện Dashboard Pro)
// =====================
void handleRoot() { 
  server.send_P(200, "text/html; charset=utf-8", htmlPage); 
}

void handleData() {
  server.sendHeader("Access-Control-Allow-Origin", "*"); // Cho phép CORS khi test Web từ laptop
  String json = "{";
  json += "\"temperature\":"; json += String(temperature,1);
  json += ",\"humidity\":"; json += String(humidity,1);
  json += ",\"pressure\":"; json += String(pressure,1);
  json += ",\"rain\":"; json += String(rain);
  json += ",\"battery_pct\":"; json += String(batteryPct); // Bổ sung thông tin pin gửi từ LoRa
  json += ",\"rssi\":"; json += String(rssi);               // ★ MỚI: gửi RSSI thực tế lên Dashboard Local
  json += ",\"seq_num\":"; json += String(seqNum);          // ★ MỚI: gửi SeqNum thực tế lên Dashboard Local
  json += ",\"lora_online\":"; json += loraOnline ? "true" : "false";
  json += "}";
  server.send(200,"application/json",json);
}

// ============================================================
// ★ MỚI: Đẩy dữ liệu lên ThingSpeak (field1-5), field6-8 do AI ghi
// ============================================================
void sendToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ThingSpeak] Bỏ qua: WiFi STA chưa có Internet");
    return;
  }
  if (lastTSsend != 0 && (millis() - lastTSsend < TS_MIN_INTERVAL_MS)) {
    return; // Chưa đủ 15s từ lần gửi trước — bỏ qua, không log để khỏi rác Serial
  }

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + THINGSPEAK_HOST + "/update?api_key=" + THINGSPEAK_API_KEY;
  url += "&field1=" + String(temperature, 2);
  url += "&field2=" + String(humidity, 2);
  url += "&field3=" + String(pressure, 2);
  url += "&field4=" + String(rain);
  url += "&field5=" + String(batteryPct);

  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String response = http.getString(); // entry_id nếu thành công, "0" nếu lỗi
    Serial.printf("[ThingSpeak] HTTP %d - entry_id=%s\n", httpCode, response.c_str());
  } else {
    Serial.printf("[ThingSpeak] Gửi thất bại: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  lastTSsend = millis();
}



// ============================================================
// ★ MỚI: Ghi log offline khi không có kết nối WiFi
// ============================================================
void logOfflineData() {
  File file = LittleFS.open("/offline.csv", "a");
  if (!file) {
    Serial.println("[LittleFS] Không mở được file để lưu offline");
    return;
  }
  file.printf("%.2f,%.2f,%.2f,%d,%d\n", temperature, humidity, pressure, rain, batteryPct);
  file.close();
  Serial.println("[LittleFS] Đã ghi dữ liệu cảm biến vào offline.csv");
}

// ============================================================
// ★ MỚI: Tự động gửi bù dữ liệu offline khi có mạng trở lại (Non-blocking)
// ============================================================
unsigned long lastSyncTime = 0;
const unsigned long SYNC_INTERVAL = 16000; // Giãn cách 16s để tránh bị ThingSpeak chặn rate limit
bool isSyncing = false;

void handleAutoSync() {
  if (WiFi.status() != WL_CONNECTED) {
    isSyncing = false;
    return;
  }
  
  if (millis() - lastSyncTime < SYNC_INTERVAL) {
    return;
  }

  if (!LittleFS.exists("/offline.csv")) {
    isSyncing = false;
    offlineSentCount = 0;
    return;
  }

  File file = LittleFS.open("/offline.csv", "r");
  if (!file) {
    isSyncing = false;
    return;
  }

  isSyncing = true;
  
  // Đọc đến dòng cần gửi (skip offlineSentCount dòng đã gửi)
  int currentLine = 0;
  String line = "";
  while (file.available() && currentLine <= offlineSentCount) {
    line = file.readStringUntil('\n');
    currentLine++;
  }
  file.close();

  // Kiểm tra xem đã đọc hết file chưa
  if (line.length() == 0 || currentLine < offlineSentCount + 1) {
    // Đã gửi hết dữ liệu hoặc file trống -> Xóa file offline.csv
    LittleFS.remove("/offline.csv");
    Serial.println("[Sync] Đã gửi hết dữ liệu offline, xóa file thành công.");
    isSyncing = false;
    offlineSentCount = 0;
    return;
  }

  line.trim();
  if (line.length() == 0) {
    // Dòng trống, tự động bỏ qua để tiến tới dòng kế tiếp
    offlineSentCount++;
    lastSyncTime = millis();
    return;
  }

  // Tách dữ liệu: temp,hum,pres,rain,batteryPct
  int comma1 = line.indexOf(',');
  int comma2 = line.indexOf(',', comma1 + 1);
  int comma3 = line.indexOf(',', comma2 + 1);
  int comma4 = line.indexOf(',', comma3 + 1);

  if (comma1 == -1 || comma2 == -1 || comma3 == -1 || comma4 == -1) {
    Serial.printf("[Sync] Định dạng dòng lỗi: %s. Đã bỏ qua.\n", line.c_str());
    offlineSentCount++; // Bỏ qua dòng lỗi này
    lastSyncTime = millis();
    return;
  }

  float offline_temp = line.substring(0, comma1).toFloat();
  float offline_hum = line.substring(comma1 + 1, comma2).toFloat();
  float offline_pres = line.substring(comma2 + 1, comma3).toFloat();
  int offline_rain = line.substring(comma3 + 1, comma4).toInt();
  int offline_bat = line.substring(comma4 + 1).toInt();

  Serial.printf("[Sync] Đang gửi bù: Temp=%.2f, Hum=%.2f, Pres=%.2f, Rain=%d, Bat=%d%%\n", 
                offline_temp, offline_hum, offline_pres, offline_rain, offline_bat);

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + THINGSPEAK_HOST + "/update?api_key=" + THINGSPEAK_API_KEY;
  url += "&field1=" + String(offline_temp, 2);
  url += "&field2=" + String(offline_hum, 2);
  url += "&field3=" + String(offline_pres, 2);
  url += "&field4=" + String(offline_rain);
  url += "&field5=" + String(offline_bat);

  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[Sync] Gửi bù thành công: HTTP %d - entry_id=%s\n", httpCode, response.c_str());
    offlineSentCount++; // Tăng chỉ số dòng đã gửi thành công
  } else {
    Serial.printf("[Sync] Gửi bù thất bại: %s (Sẽ thử lại lần sau)\n", http.errorToString(httpCode).c_str());
  }
  http.end();

  lastSyncTime = millis();
}

// ============================================================
// Khởi tạo mạng WiFi AP nội bộ an toàn và có IP tĩnh cố định
// ============================================================
void startSoftAP() {
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  
  WiFi.softAPConfig(local_IP, gateway, subnet);
  
  bool result;
  if (password == NULL || strlen(password) == 0) {
    result = WiFi.softAP(ssid);
  } else {
    result = WiFi.softAP(ssid, password);
  }
  
  if (result) {
    Serial.println("\nAccess Point Started: " + String(ssid));
    Serial.print("IP Web Dashboard: "); Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("\nAccess Point Failed to Start!");
  }
}

// =====================
// Setup Hệ thống
// =====================
void setup() {
  Serial.begin(115200);

  // Khởi tạo còi báo động
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ★ MỚI: Thử còi khi cấp nguồn (Power-On Self-Test)
  Serial.println("[SYSTEM] Đang thử còi báo động vật lý...");
  digitalWrite(BUZZER_PIN, HIGH); // Bật còi
  delay(500);                     // Kêu trong 0.5 giây
  digitalWrite(BUZZER_PIN, LOW);  // Tắt còi
  Serial.println("[SYSTEM] Kết thúc thử còi.");

  // Bật LittleFS
  if(!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed");
  } else {
    Serial.println("LittleFS Mounted Successfully");
  }

  // ★ SỬA: chuyển sang AP_STA — vừa phát AP nội bộ, vừa kết nối Internet
  WiFi.mode(WIFI_AP_STA);

  // Phát WiFi AP (giữ nguyên — user cục bộ vẫn dùng được dù không có Internet)
  startSoftAP();

  // ★ MỚI: Khởi chạy DNS Server cho Captive Portal (Chuyển hướng mọi tên miền về ESP8266)
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
  Serial.println("DNS Server Started for Captive Portal redirection");

  // ★ MỚI: Kết nối STA vào WiFi nhà để có Internet
  Serial.printf("Đang kết nối STA tới '%s'", staSsid);
  WiFi.begin(staSsid, staPassword);
  unsigned long staStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - staStart < 15000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nSTA kết nối OK — IP: " + WiFi.localIP().toString());
    wasConnected = true;
  } else {
    Serial.println("\nSTA timeout — chạy OFFLINE (Chuyển sang WIFI_AP để ổn định sóng nội bộ)");
    WiFi.mode(WIFI_AP);
    startSoftAP();
    wasConnected = false;
    lastReconnectAttempt = millis();
  }

  // Bật Web Server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  
  // ★ MỚI: Chuyển hướng Captive Portal cho các thiết bị di động
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", ""); // Tự động chuyển hướng về trang chủ
  });
  
  server.begin();

  // Bật LoRa
  LoRa.setPins(SS, RST, DIO0);
  if(!LoRa.begin(433E6)) {
    Serial.println("LoRa Init Failed! Running in web-only mode without LoRa.");
    loraOnline = false;
  } else {
    LoRa.setSyncWord(0xAB); // Mật khẩu đồng bộ
    LoRa.enableCrc();      // Kích hoạt CRC để khớp cấu hình với Sensor Node
    Serial.println("LoRa Ready - Đang chờ gói tin nhị phân...");
    loraOnline = true;
  }
}

// ============================================================
// Quản lý trạng thái WiFi tự động tránh đơ sóng AP nội bộ
// ============================================================
void manageWiFi() {
  unsigned long now = millis();
  if (now - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      Serial.println("[WiFi] Đã kết nối lại Internet!");
      wasConnected = true;
    }
  } else {
    // Nếu mất kết nối
    if (wasConnected) {
      wasConnected = false;
      disconnectTime = now;
      Serial.println("[WiFi] Bị mất kết nối Internet.");
    }
    
    // Nếu mất kết nối quá 30 giây mà vẫn đang ở chế độ AP_STA, chuyển sang AP duy nhất
    if (WiFi.getMode() == WIFI_AP_STA && (now - disconnectTime > 30000)) {
      Serial.println("[WiFi] Mất mạng quá lâu, chuyển sang WIFI_AP để ổn định sóng nội bộ.");
      WiFi.mode(WIFI_AP);
      startSoftAP();
    }
    
    // Nếu đang ở chế độ WIFI_AP, định kỳ 5 phút thử quét lại mạng STA một lần
    if (WiFi.getMode() == WIFI_AP && (now - lastReconnectAttempt > RECONNECT_INTERVAL)) {
      lastReconnectAttempt = now;
      Serial.println("[WiFi] Thử tìm cách kết nối lại Internet (Chuyển sang AP_STA)...");
      WiFi.mode(WIFI_AP_STA);
      startSoftAP();
      WiFi.begin(staSsid, staPassword);
      disconnectTime = now; // Reset bộ đếm thời gian chờ kết nối
    }
  }
}

// =====================
// Vòng lặp chính (Loop)
// =====================
void loop() {
  manageWiFi(); // Quản lý WiFi chống nghẽn AP
  dnsServer.processNextRequest(); // Duy trì Captive Portal DNS
  server.handleClient(); // Duy trì Web Server

  // Tự động kiểm tra và gửi bù dữ liệu ngoại tuyến khi có mạng
  handleAutoSync();

  if (loraOnline) {
    int packetSize = LoRa.parsePacket();
    if (packetSize == sizeof(WeatherPkt_t)) { // Kiểm tra gói tin có đúng kích thước không
      WeatherPkt_t pkt;
      LoRa.readBytes((uint8_t*)&pkt, sizeof(pkt)); // Đọc toàn bộ cục Binary vào Struct

      // ★ Lọc Node ID của trạm phát (Tránh nhận nhầm dữ liệu trạm khác)
      if (pkt.node_id != 0x0001) {
        Serial.printf("[LoRa] Bỏ qua gói tin từ Node lạ: 0x%04X\n", pkt.node_id);
        return;
      }

      // Giải mã: Phục hồi số thập phân (chia lại cho 100 và 10 như lúc Sensor Node nhân lên)
      temperature = pkt.temperature / 100.0f;
      humidity = pkt.humidity / 100.0f;
      pressure = (pkt.pressure / 10.0f) + 800.0f;
      rain = pkt.rain_analog;
      batteryPct = pkt.battery_pct;   
      rssi = LoRa.packetRssi();       
      seqNum = pkt.seq_num;           

      // Hệ thống cảnh báo song song sử dụng thuật toán trễ Hysteresis
      static bool alarmHotActive = false;
      static bool alarmRainActive = false;
      static bool alarmExtremeActive = false; // ★ MỚI: Báo động đỏ cực đoan (Nhiệt độ cao kèm mưa dông)

      // Kịch bản 1: Nắng nóng cực hạn (nhiệt độ > 40°C và trời khô ráo)
      if (!alarmHotActive && (temperature > 40.0f && rain > 800)) {
        alarmHotActive = true;
        Serial.println("[CẢNH BÁO] Phát hiện NẮNG NÓNG cực hạn (>40°C) | Nguy cơ cháy rừng cao!");
      } else if (alarmHotActive && (temperature < 38.5f || rain < 500)) {
        alarmHotActive = false;
        Serial.println("[HỆ THỐNG] Nhiệt độ nắng nóng đã hạ xuống ngưỡng an toàn.");
      }

      // Kịch bản 2: Mưa bão / Ngập úng (lượng mưa cực đoan)
      if (!alarmRainActive && (rain < 450)) {
        alarmRainActive = true;
        Serial.println("[CẢNH BÁO] Phát hiện MƯA LỚN cực đoan | Nguy cơ ngập úng!");
      } else if (alarmRainActive && (rain > 550)) {
        alarmRainActive = false;
        Serial.println("[HỆ THỐNG] Mưa lớn đã ngớt, lượng mưa về ngưỡng an toàn.");
      }

      // Kịch bản 3: Báo động đỏ cực đoan (Nhiệt độ > 40°C và Mưa dông < 500)
      if (!alarmExtremeActive && (temperature > 40.0f && rain < 500)) {
        alarmExtremeActive = true;
        Serial.println("[CẢNH BÁO] BÁO ĐỘNG ĐỎ: Nhiệt độ cực cao (>40°C) kèm theo mưa dông!");
      } else if (alarmExtremeActive && (temperature < 38.5f || rain > 550)) {
        alarmExtremeActive = false;
        Serial.println("[HỆ THỐNG] Báo động đỏ cực đoan đã được giải tỏa.");
      }

      // Kết hợp trạng thái để điều khiển còi báo động
      bool alarmActiveNew = (alarmHotActive || alarmRainActive || alarmExtremeActive);
      if (alarmActiveNew != alarmActive) {
        alarmActive = alarmActiveNew;
        digitalWrite(BUZZER_PIN, alarmActive ? HIGH : LOW);
      }

      Serial.println("--- GÓI TIN MỚI ---");
      Serial.print("Nhiệt độ: "); Serial.print(temperature); Serial.println(" C");
      Serial.print("Độ ẩm: "); Serial.print(humidity); Serial.println(" %");
      Serial.print("Áp suất: "); Serial.print(pressure); Serial.println(" hPa");
      Serial.print("Mưa: "); Serial.println(rain);
      Serial.printf("RSSI: %d dBm | Seq: %d\n", rssi, seqNum);

      // --- BẮT ĐẦU GỬI PHẢN HỒI ACK ---
      delay(50); // Delay 50ms để Sensor Node kịp chuyển sang chế độ RX Single
      
      AckPkt_t ack;
      ack.node_id = pkt.node_id;
      ack.seq_num = pkt.seq_num;
      ack.ack_magic = 0xAC; // ACK_MAGIC
      ack.crc16 = crc16_ccitt((const uint8_t*)&ack, sizeof(ack) - 2);

      LoRa.beginPacket();
      LoRa.write((const uint8_t*)&ack, sizeof(ack));
      LoRa.endPacket();
      
      Serial.printf("[LoRa] Đã gửi phản hồi ACK cho Node %d | Seq %d\n", ack.node_id, ack.seq_num);
      // --- KẾT THÚC GỬI PHẢN HỒI ACK ---

      // Gửi lên ThingSpeak nếu có WiFi và không bận gửi bù, ngược lại thì lưu offline
      if (WiFi.status() == WL_CONNECTED && !isSyncing) {
        sendToThingSpeak();   
      } else {
        logOfflineData();
      }
    } else if (packetSize > 0) {
      Serial.println("Nhận được gói tin sai kích thước (Rác).");
    }
  }
}
