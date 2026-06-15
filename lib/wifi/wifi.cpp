#include <WiFi.h>
#include <WebServer.h> // Uses the advanced, stable built-in server

#define LED_PIN D10

const char* ssid = "TP-Link_E3AC";
const char* password = "I&Mmansion2021";

WebServer server(80);

unsigned long previousBlinkTime = 0;
const long blinkInterval = 500;
int ledState = LOW;
bool manualMode = false; // Tracks if the user took manual control

// 1. Sends raw data back to the browser JavaScript
void handleData() {
  String data = String(ledState == HIGH ? "ON" : "OFF") + "," + 
                String(millis() / 1000) + "," + 
                String(manualMode ? "Manual Control" : "Auto Blinking");
  server.send(200, "text/plain", data);
}

// 2. Handles the button press request from the browser
void handleToggle() {
  manualMode = true; // Freeze the automatic blinker
  ledState = (ledState == LOW) ? HIGH : LOW; // Toggle the LED state
  digitalWrite(LED_PIN, ledState);
  server.send(200, "text/plain", "OK");
}

// 3. Serves the main UI with buttons and background script
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>html { font-family: Helvetica; text-align: center; background-color: #f4f4f9; padding-top: 50px;}";
  html += ".box { display: inline-block; padding: 20px; background: white; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); width: 280px; }";
  html += ".btn { display: block; width: 100%; padding: 15px; margin: 15px 0; font-size: 18px; border: none; border-radius: 5px; cursor: pointer; color: white; background: #007bff; }";
  html += ".btn:active { background: #0056b3; }</style>";
  
  // JavaScript to handle background data updates and button clicks
  html += "<script>";
  html += "setInterval(function() {";
  html += "  fetch('/data').then(r => r.text()).then(text => {";
  html += "    const p = text.split(',');";
  html += "    document.getElementById('status').innerText = p[0];";
  html += "    document.getElementById('status').style.color = p[0] === 'ON' ? 'green' : 'red';";
  html += "    document.getElementById('uptime').innerText = p[1] + 's';";
  html += "    document.getElementById('mode').innerText = p[2];";
  html += "  });";
  html += "}, 200);";
  
  html += "function toggleLED() { fetch('/toggle'); }"; // Trigger hidden URL
  html += "</script>";
  
  html += "</head><body><div class='box'><h1>XIAO ESP32C3</h1>";
  html += "<p><b>Mode:</b> <span id='mode'>Loading...</span></p>";
  html += "<p><b>LED Status:</b> <span id='status' style='font-weight:bold;'>Loading...</span></p>";
  html += "<p><b>Uptime:</b> <span id='uptime'>Loading...</span></p>";
  html += "<button class='btn' onclick='toggleLED()'>Toggle LED Pin</button>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  pinMode(LED_PIN, OUTPUT);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected! IP Address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/toggle", handleToggle); // New action route

  server.begin();
}

void loop() {
  server.handleClient();

  // Only blink automatically if the user hasn't pressed the web button yet
  if (!manualMode) {
    unsigned long currentTime = millis();
    if (currentTime - previousBlinkTime >= blinkInterval) {
      previousBlinkTime = currentTime;
      ledState = (ledState == LOW) ? HIGH : LOW;
      digitalWrite(LED_PIN, ledState);
    }
  }
}
