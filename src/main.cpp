#include <WiFi.h>
#include <WebServer.h>
#include <esp_camera.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "miniz.h"  // Miniz for ZIP

#define ZIP_NAME "/images.zip"
#define FLASHLIGHT 4
#define SS_DEBUG true

void handleRoot();
void handleGetData();
void handleDownload();
void handleImages();
void handleDownloadImage();
void handleDownloadImages();
void handleNotFound();
void serialTask(void* pvParameters);
void serverTask(void* pvParameters);

const char* ssid = "BMI_SYSTEM";
const char* password = "12345678";
String ipAddress = "";
int port = 80;

WebServer server(port);
String serialData = "";
const int batteryPin = 33; // GPIO33 for battery voltage
const int wakeUpInterval = 30000000; // 30 seconds in microseconds (deep sleep)

void setup() {
  Serial.begin(115200);
  analogReadResolution(12); // 12-bit ADC (0–4095)
  pinMode(FLASHLIGHT, OUTPUT);
  digitalWrite(FLASHLIGHT, LOW);
  
  if (!SD_MMC.begin()) {
    if (SS_DEBUG) Serial.println("SD Card Mount Failed");
    return;
  }
  if (!SD_MMC.mkdir("/images")) {
    if (SS_DEBUG) Serial.println("Failed to create /images folder, it may already exist");
  }
  
  File file = SD_MMC.open("/bmi_data.csv", FILE_READ);
  if (!file) {
    file = SD_MMC.open("/bmi_data.csv", FILE_WRITE);
    if (file) {
      file.println("ID,Height (cm),Weight (kg),BMI,Timestamp,Image,Battery (V)");
      file.close();
    }
  } else {
    file.close();
  }
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    if (SS_DEBUG) Serial.println("Camera init failed");
    return;
  }
  
  WiFi.softAP(ssid, password);
  if (SS_DEBUG) Serial.println("AP started: ");
  ipAddress = WiFi.softAPIP().toString();
  if (SS_DEBUG) Serial.println(ipAddress);
  
  server.on("/", handleRoot);
  server.on("/getdata", handleGetData);
  server.on("/images", HTTP_GET, handleImages);
  server.on("/download", handleDownload);
  server.on("/download-image", HTTP_GET, handleDownloadImage);
  server.on("/download-images", handleDownloadImages);
  server.onNotFound(handleNotFound);
  server.begin();
  
  xTaskCreatePinnedToCore(serialTask, "SerialTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(serverTask, "ServerTask", 10000, NULL, 1, NULL, 0);
  
  // Configure deep sleep
  // esp_sleep_enable_timer_wakeup(wakeUpInterval);
}

int getNextId() {
  File file = SD_MMC.open("/bmi_data.csv", FILE_READ);
  int id = 0;
  if (file) {
    file.readStringUntil('\n');
    while (file.available()) {
      String line = file.readStringUntil('\n');
      int firstComma = line.indexOf(',');
      if (firstComma != -1) {
        id = line.substring(0, firstComma).toInt();
      }
    }
    file.close();
  }
  return id + 1;
}

float readBatteryVoltage() {
  int adcValue = analogRead(batteryPin);
  float voltage = (adcValue / 4095.0) * 4.2 * 2; // Voltage divider: 4.2V max, 2:1 ratio
  return voltage;
}

void saveDataToCsv(float height, float weight, float bmi, String timestamp, String imageName, float batteryVoltage) {
  File file = SD_MMC.open("/bmi_data.csv", FILE_APPEND);
  if (file) {
    String csvLine = String(getNextId()) + "," + String(height) + "," + String(weight) + "," + String(bmi) + "," + timestamp + "," + imageName + "," + String(batteryVoltage, 2);
    // file.print(csvLine);
    // file.print("\n");
    file.println(csvLine);
    file.close();
    if (SS_DEBUG) Serial.println("Saved CSV entry: " + csvLine);
  } else {
    if (SS_DEBUG) Serial.println("Failed to open data.csv for writing");
  }
}

bool saveImage(String timestamp) {
  digitalWrite(FLASHLIGHT, HIGH);
  camera_fb_t* fb = esp_camera_fb_get();
  digitalWrite(FLASHLIGHT, LOW);
  if (!fb) {
    if (SS_DEBUG) Serial.println("Camera capture failed");
    return false;
  }
  
  String safeTimestamp = timestamp;
  safeTimestamp.replace(":", "-");
  String imageName = "/images/img_" + safeTimestamp + ".jpg";
  File file = SD_MMC.open(imageName, FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
    if (SS_DEBUG) Serial.println("Saved image: " + imageName);
  } else {
    if (SS_DEBUG) Serial.println("Failed to save image: " + imageName);
    esp_camera_fb_return(fb);
    return false;
  }
  
  esp_camera_fb_return(fb);
  return true;
}

void handleRoot() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html lang='en'>
    <head>
    <meta charset='utf-8' />
    <meta name='viewport' value='width=device-width, initial-scale=1' />
    <meta name='theme-color' content='#000000' />
    <title>BMI</title>
    </head>
    <body>
      <h1 style="text-align: center;">BMI System</h1>
      <p style="max-width: 100%; display: flex; justify-content: center;"><a href="/download">Download CSV</a> | <a href="/download-images" style="">Download All Images</a></p>
      <p>Filter by Date: <input type='date' id='dateFilter' onchange='applyFilter()'></p>
      <table id='dataTable' border='1'>
        <tr>
          <th>ID</th>
          <th>Height (cm)</th>
          <th>Weight (kg)</th>
          <th>BMI</th>
          <th onclick='sortTable()' style='cursor:pointer;'>Timestamp &#x2195;</th>
          <th>Image</th>
          <th>Battery (V)</th>
        </tr>
      </table>
      <style>
        table {border-collapse: collapse; width: 100%;}
        th, td {border: 1px solid black; padding: 8px; text-align: left;}
        th {background-color: #f2f2f2;}
        a {margin-right: 10px;}
        input[type=date] {padding: 5px;}
      </style>
      <script>
        let sortAscending = true;
        let currentData = [];

        function updateTable(data) {
          const table = document.getElementById('dataTable');
          while (table.rows.length > 1) table.deleteRow(1);
          const filterDate = document.getElementById('dateFilter').value;
          
          data.forEach(entry => {
            if (!filterDate || entry.timestamp.startsWith(filterDate)) {
              const row = table.insertRow();
              row.insertCell().textContent = entry.id;
              row.insertCell().textContent = entry.height;
              row.insertCell().textContent = entry.weight;
              row.insertCell().textContent = entry.bmi;
              row.insertCell().textContent = entry.timestamp;
              const cell = row.insertCell();
              cell.innerHTML = `<a href='/images?file=${entry.image}'>View</a><a href='/download-image?file=${entry.image}'>Download</a>`;
              row.insertCell().textContent = entry.battery;
            }
          });
        }

        function sortTable() {
          sortAscending = !sortAscending;
          currentData.sort((a, b) => {
            const dateA = new Date(a.timestamp);
            const dateB = new Date(b.timestamp);
            return sortAscending ? dateA - dateB : dateB - dateA;
          });
          updateTable(currentData);
        }

        function applyFilter() {
          updateTable(currentData);
        }

        function fetchData() {
          fetch('/getdata')
            .then(response => response.json())
            .then(data => {
              currentData = data;
              updateTable(data);
            })
            .catch(error => console.error('Error fetching data:', error));
        }

        fetchData();
        setInterval(fetchData, 5000);
      </script>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleGetData() {
  File file = SD_MMC.open("/bmi_data.csv", FILE_READ);
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  
  if (file) {
    file.readStringUntil('\n');
    while (file.available()) {
      String line = file.readStringUntil('\n');
      int c1 = line.indexOf(',');
      int c2 = line.indexOf(',', c1 + 1);
      int c3 = line.indexOf(',', c2 + 1);
      int c4 = line.indexOf(',', c3 + 1);
      int c5 = line.indexOf(',', c4 + 1);
      int c6 = line.indexOf(',', c5 + 1);
      if (c1 != -1 && c2 != -1 && c3 != -1 && c4 != -1 && c5 != -1) {
        JsonObject entry = array.add<JsonObject>();
        entry["id"] = line.substring(0, c1).toInt();
        entry["height"] = line.substring(c1 + 1, c2).toFloat();
        entry["weight"] = line.substring(c2 + 1, c3).toFloat();
        entry["bmi"] = line.substring(c3 + 1, c4).toFloat();
        entry["timestamp"] = line.substring(c4 + 1, c5);
        entry["image"] = line.substring(c5 + 1, c6);
        entry["battery"] = line.substring(c6 + 1).toFloat();
      }
    }
    file.close();
  }
  
  String jsonOutput;
  serializeJson(array, jsonOutput);
  server.send(200, "application/json", jsonOutput);
}

void handleDownload() {
  File file = SD_MMC.open("/bmi_data.csv", FILE_READ);
  if (file) {
    server.streamFile(file, "text/csv");
    file.close();
  } else {
    server.send(404, "text/plain", "CSV file not found");
  }
}

void handleImages() {
  if (server.hasArg("file")) {
    String filePath = server.arg("file");
    File file = SD_MMC.open("/images/" + filePath, FILE_READ);
    if (file) {
      server.streamFile(file, "image/jpeg");
      file.close();
    } else {
      server.send(404, "text/plain", "Image not found");
    }
  } else {
    server.send(400, "text/plain", "Missing file parameter");
  }
}

void handleDownloadImage() {
  if (server.hasArg("file")) {
    String filePath = server.arg("file");
    File file = SD_MMC.open("/images/" + filePath, FILE_READ);
    if (file) {
      // server.streamFile(file, "image/jpeg", filePath.substring(filePath.lastIndexOf('/') + 1));
      server.streamFile(file, "image/jpeg", 200);
      file.close();
    } else {
      server.send(404, "text/plain", "Image not found");
    }
  } else {
    server.send(400, "text/plain", "Missing file parameter");
  }
}

void handleDownloadImages() {
  File root = SD_MMC.open("/images");
  if (!root || !root.isDirectory()) {
    server.send(404, "text/plain", "Images folder not found");
    return;
  }
  
  File zipFile = SD_MMC.open(ZIP_NAME, FILE_WRITE);
  if (!zipFile) {
    server.send(500, "text/plain", "Failed to create ZIP file");
    return;
  }
  zipFile.close();
  
  // Placeholder: Replace with actual ZIP library calls
  
  // Create ZIP archive
  mz_zip_archive zip_archive;
  memset(&zip_archive, 0, sizeof(zip_archive));
  if (!mz_zip_writer_init_file(&zip_archive, ZIP_NAME, 0)) {
    if (SS_DEBUG) Serial.println("ZIP init failed");
    return;
  }
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      file = root.openNextFile();
      continue;
    }
    String filePath = "/images/" + String(file.name());
    size_t fileSize = file.size();
    uint8_t* fileData = (uint8_t*)malloc(fileSize);
    if (fileData) {
      file.read(fileData, fileSize);
      if (!mz_zip_writer_add_mem(&zip_archive, file.name(), fileData, fileSize, MZ_BEST_COMPRESSION)) {
        if (SS_DEBUG) Serial.println("Failed to add file to ZIP: " + String(file.name()));
      }
      free(fileData);
    } else {
      if (SS_DEBUG) Serial.println("Failed to allocate memory for file: " + String(file.name()));
    }
    file = root.openNextFile();
  }
  if (!mz_zip_writer_finalize_archive(&zip_archive)) {
    if (SS_DEBUG) Serial.println("ZIP finalize failed");
    return;
  }
  mz_zip_writer_end(&zip_archive);
  server.streamFile(zipFile, "application/zip");
  // zipFile.close();
  SD_MMC.remove(ZIP_NAME);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void serialTask(void* pvParameters) {
  while (1) {
    JsonDocument doc;

    if (Serial.available()) {
        DeserializationError error = deserializeJson(doc, Serial);
        if (error) {
          doc.clear();
          doc["cmd"] = "status";
          doc["msg"] = "JSON_ERR";
          serializeJson(doc, Serial);
          vTaskDelay(100 / portTICK_PERIOD_MS);
          continue;
        }
        
        if(doc["cmd"] == "bmi_data") {
          float height = doc["height"];
          float weight = doc["weight"];
          float bmi = doc["bmi"];
          String timestamp = doc["timestamp"].as<String>();
          // float batteryVoltage = readBatteryVoltage();
          float batteryVoltage = doc["battery"];
          String imageName = "img_" + timestamp + ".jpg";
          imageName.replace(":", "-");
          saveDataToCsv(height, weight, bmi, timestamp, imageName, batteryVoltage);
          if (saveImage(timestamp)) {
            doc.clear();
            doc["cmd"] = "status";
            doc["msg"] = "IMG_OK";
            serializeJson(doc, Serial);
            vTaskDelay(100 / portTICK_PERIOD_MS);
          } else {
            doc.clear();
            doc["cmd"] = "status";
            doc["msg"] = "IMG_ERR";
            serializeJson(doc, Serial);
            vTaskDelay(100 / portTICK_PERIOD_MS);
          }
          // esp_deep_sleep_start(); // Enter deep sleep after processing
        } else if(doc["cmd"] == "host"){
          doc["host"] = String(ipAddress) + ":" + String(port);
          serializeJson(doc, Serial);
          vTaskDelay(100 / portTICK_PERIOD_MS);
        } else if (doc["cmd"] == "ping") {
          doc["msg"] = "pong";
          serializeJson(doc, Serial);
          vTaskDelay(100 / portTICK_PERIOD_MS);
        }
      }

      doc.clear();
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }

void serverTask(void* pvParameters) {
  while (1) {
    server.handleClient();
    // server.
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void loop() {}