#include <WiFi.h>
#include <HTTPClient.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

// ==========================================
//              USER CONFIG
// ==========================================
struct WiFiCreds { const char* ssid; const char* pass; };

WiFiCreds myNetworks[] = {
  {"iPhone",       "77332608"},
  {"Hotspot 5.0",  "77332608"},
  {"Nishat",       "77332608"}
};
int totalNetworks = 3;
int currentNetworkIndex = 0;

String serverBase = "http://78.154.103.21:10976";

// ==========================================
//              HARDWARE SETUP
// ==========================================
#define SDA_PIN 8
#define SCL_PIN 9

// Display Driver
U8G2_SSD1306_128X32_UNIVISION_F_SW_I2C u8g2(U8G2_R0, SCL_PIN, SDA_PIN, U8X8_PIN_NONE);

// New 4x5 Matrix Wiring (ESP32 Pins)
int rowPins[] = {0, 1, 2, 3}; 
int colPins[] = {4, 5, 6, 7, 10}; 

// ==========================================
//              GLOBAL VARIABLES
// ==========================================
String inputBuffer = "";
unsigned long lastKeyPress = 0;
int t9CycleIndex = 0;
int lastKeyIndex = -1;

String historyBuffer[3];
int historyCount = 0;

bool isViewingAnswer = false;
bool isMathMode = false;
bool isSystemMenu = false;
bool isSleepMode = false; 

int menuIndex = 0;
int screenBrightness = 120;
int scrollY = 0;
int totalViewHeight = 0;
uint8_t viewBuffer[512]; 

const char* mathWords[] = {" equal ", " sum ", " mean ", " by ", " div ", " int "};
int mathWordIndex = 0;

const char* t9Map[] = {
  " 0", ".,-/?!1@#", "abc2", "def3", "ghi4", 
  "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9"
};

const char* mathMap[] = {
  "0 ", "1.+*/%", "2^r23", "3abdpt", "4Ssmer", 
  "5XYPQ", "6idx", "7$L", "8()[]{}", "9><gln"
};

// ==========================================
//              MAIN SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Setup Matrix
  for(int i=0; i<4; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], HIGH);
  }
  for(int j=0; j<5; j++) {
    pinMode(colPins[j], INPUT_PULLUP);
  }
  
  // Setup Display
  u8g2.begin();
  u8g2.setContrast(screenBrightness);
  u8g2.setFont(u8g2_font_6x10_tf);
  
  // Connect WiFi
  connectWiFi(currentNetworkIndex);

  // Setup OTA (Over-The-Air)
  ArduinoOTA.setHostname("Exam-Assistant-ESP32");
  ArduinoOTA.begin();
}

// ==========================================
//              MAIN LOOP
// ==========================================
void loop() {
  // OTA Handle - সবসময় ওয়্যারলেস আপলোডের জন্য কান পেতে থাকবে
  ArduinoOTA.handle();

  // --- 1. CHECK SLEEP COMBO (7 + DEL) ---
  if (checkSleepCombo()) {
    unsigned long start = millis();
    while(checkSleepCombo()) {
      if (millis() - start > 1500) { 
        isSleepMode = !isSleepMode;
        u8g2.setPowerSave(isSleepMode ? 1 : 0);
        while(checkSleepCombo()) delay(10); 
        break;
      }
      delay(10);
    }
  }

  // স্লিপ মোডে থাকলে অন্য কোনো বাটন কাজ করবে না
  if (isSleepMode) { delay(100); return; }

  // --- 2. STANDARD OPERATION ---
  int key = scanMatrix();

  if (key != -1) {
    handleInput(key);
    delay(180); 
  }

  // --- 3. DISPLAY ---
  if (isSystemMenu) drawSystemMenu();
  else if (!isViewingAnswer) drawTypingMode();
}

// ==========================================
//           SLEEP COMBO CHECKER
// ==========================================
bool checkSleepCombo() {
  digitalWrite(rowPins[0], LOW); 
  bool sevenPressed = (digitalRead(colPins[0]) == LOW); 
  bool delPressed   = (digitalRead(colPins[3]) == LOW); 
  digitalWrite(rowPins[0], HIGH); 
  return (sevenPressed && delPressed);
}

// ==========================================
//              INPUT ENGINE
// ==========================================
void handleInput(int key) {
  
  if (key == 24) { 
    unsigned long start = millis();
    while(scanMatrix() == 24) {
      if(millis() - start > 1500) { 
        isSystemMenu = !isSystemMenu; 
        menuIndex = 0; 
        while(scanMatrix() == 24) delay(10); 
        return; 
      }
      delay(10);
    }
  }

  if (isSystemMenu) {
    if (key == 12) { menuIndex--; if(menuIndex<0) menuIndex=3; } 
    else if (key == 32) { menuIndex++; if(menuIndex>3) menuIndex=0; } 
    else if (key == 21) adjustMenuValue(-1); 
    else if (key == 23) adjustMenuValue(1);  
    else if (key == 45) executeMenuAction(); 
    return;
  }

  if (isViewingAnswer) {
    if (key == 12) { // UP (8 বাটন)
      scrollY -= 32; 
      if(scrollY < 0) scrollY = 0; 
      fetchAndDrawView(); 
    } 
    else if (key == 32) { // DOWN (2 বাটন)
      scrollY += 32; 
      if(scrollY > totalViewHeight - 32) scrollY = totalViewHeight - 32; 
      if(scrollY < 0) scrollY = 0; 
      fetchAndDrawView(); 
    } 
    else if (key == 45) { isViewingAnswer = false; } 
    return;
  }

  if (key == 45) {
    unsigned long start = millis();
    bool isLong = false;
    while(scanMatrix() == 45) {
      if (millis() - start > 500) { 
        inputBuffer += " "; 
        isLong = true; 
        break; 
      }
    }
    if (!isLong) { 
      if(inputBuffer.length() > 0) {
        addToHistory(inputBuffer);
        askServer(inputBuffer);
      }
    } else {
      while(scanMatrix() == 45); 
    }
    return;
  }

  if (key == 14) {
    if(inputBuffer.length() > 0) inputBuffer.remove(inputBuffer.length()-1);
    return;
  }

  if (key == 15) {
    inputBuffer = "";
    return;
  }

  if (key == 41) {
    if (isMathMode) {
      inputBuffer += mathWords[mathWordIndex];
      mathWordIndex++;
      if(mathWordIndex > 5) mathWordIndex = 0;
    } else {
      processInput(10); 
    }
    return;
  }

  if (key == 42) {
    static int dc = 0; static unsigned long ld = 0;
    if (millis() - ld < 500) dc++; else dc = 1;
    ld = millis();
    if (dc == 3) { 
      isMathMode = !isMathMode; 
      inputBuffer.remove(inputBuffer.length()-2); 
      dc=0; 
    } else inputBuffer += ".";
    return;
  }

  int num = -1;
  if (key == 11) num = 7;      else if (key == 12) num = 8;      else if (key == 13) num = 9;
  else if (key == 21) num = 4; else if (key == 22) num = 5;      else if (key == 23) num = 6;
  else if (key == 31) num = 1; else if (key == 32) num = 2;      else if (key == 33) num = 3;
  
  if (num != -1) processInput(num);

  if (key == 34) { inputBuffer += "+"; lastKeyIndex = -1; }
  if (key == 35) { inputBuffer += "-"; lastKeyIndex = -1; }
  if (key == 25) { inputBuffer += "/"; lastKeyIndex = -1; }
  if (key == 43) { inputBuffer += "x10"; lastKeyIndex = -1; }
  if (key == 44) { inputBuffer += "Ans"; lastKeyIndex = -1; }
  
  if (key == 24 && !isSystemMenu) { inputBuffer += "*"; lastKeyIndex = -1; }
}

// ==========================================
//              T9 ENGINE
// ==========================================
void processInput(int num) {
  int mapIndex = (num == 10) ? 0 : num;
  unsigned long now = millis();
  const char** currentMap = isMathMode ? mathMap : t9Map;
  
  if (mapIndex == lastKeyIndex && (now - lastKeyPress < 900)) {
    t9CycleIndex++;
    String chars = currentMap[mapIndex];
    if (t9CycleIndex >= chars.length()) t9CycleIndex = 0;
    inputBuffer.setCharAt(inputBuffer.length()-1, chars[t9CycleIndex]);
  } else {
    t9CycleIndex = 0;
    inputBuffer += currentMap[mapIndex][0];
  }
  lastKeyIndex = mapIndex;
  lastKeyPress = now;
}

// ==========================================
//              BIOS MENU
// ==========================================
void drawSystemMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 8, "-- BIOS --");
  
  u8g2.setCursor(0, 22);
  if (menuIndex == 0) { 
    u8g2.print("Bright: "); u8g2.print(map(screenBrightness,0,255,0,100)); u8g2.print("%"); 
  }
  else if (menuIndex == 1) { 
    u8g2.print("WiFi: "); u8g2.print(myNetworks[currentNetworkIndex].ssid); 
  }
  else if (menuIndex == 2) { 
    u8g2.print("Hist: "); 
    if(historyCount>0) u8g2.print(historyBuffer[historyCount-1].substring(0,8));
    else u8g2.print("(Empty)");
  }
  else if (menuIndex == 3) { u8g2.print("Exit"); }
  
  u8g2.setCursor(90, 32);
  u8g2.print("USB");
  
  u8g2.sendBuffer();
}

void adjustMenuValue(int dir) {
  if (menuIndex == 0) { 
    screenBrightness = constrain(screenBrightness + (dir * 25), 0, 255); 
    u8g2.setContrast(screenBrightness); 
  }
  else if (menuIndex == 1) { 
    currentNetworkIndex += dir; 
    if(currentNetworkIndex >= totalNetworks) currentNetworkIndex=0; 
    if(currentNetworkIndex<0) currentNetworkIndex=totalNetworks-1; 
  }
}

void executeMenuAction() {
  if (menuIndex == 1) { connectWiFi(currentNetworkIndex); isSystemMenu = false; }
  else if (menuIndex == 2) { 
    if (historyCount > 0) { inputBuffer = historyBuffer[historyCount-1]; isSystemMenu = false; }
  }
  else if (menuIndex == 3) isSystemMenu = false;
}

// ==========================================
//              UTILITIES
// ==========================================
int scanMatrix() {
  for(int r=0; r<4; r++) {
    digitalWrite(rowPins[r], LOW); 
    for(int c=0; c<5; c++) { 
      if(digitalRead(colPins[c]) == LOW) { 
        digitalWrite(rowPins[r], HIGH); 
        return (r+1)*10 + (c+1); 
      }
    }
    digitalWrite(rowPins[r], HIGH); 
  }
  return -1;
}

void connectWiFi(int index) {
  u8g2.clearBuffer(); u8g2.setCursor(0, 15); u8g2.print("Conn: "); u8g2.print(myNetworks[index].ssid); 
  u8g2.sendBuffer();
  WiFi.disconnect();
  WiFi.begin(myNetworks[index].ssid, myNetworks[index].pass);
  int t=0; while(WiFi.status()!=WL_CONNECTED && t<15){delay(200);t++;}
}

void addToHistory(String q) {
  historyBuffer[2]=historyBuffer[1]; 
  historyBuffer[1]=historyBuffer[0]; 
  historyBuffer[0]=q; 
  if(historyCount<3) historyCount++;
}

void askServer(String q) { 
  if(WiFi.status() != WL_CONNECTED) { inputBuffer = "No WiFi"; return; }
  
  u8g2.clearBuffer(); 
  u8g2.drawStr(0, 20, "Thinking..."); 
  u8g2.sendBuffer();
  
  HTTPClient http; 
  http.setTimeout(45000); // 45 সেকেন্ড টাইমআউট
  
  String url = serverBase + "/ask?q=" + q; 
  url.replace(" ", "%20"); 
  url.replace("+", "%2B");
  
  http.begin(url); 
  int httpCode = http.GET();
  
  if(httpCode == 200) {
      String payload = http.getString(); 
      if(payload.startsWith("OK:")) {
          totalViewHeight = payload.substring(3).toInt(); 
          isViewingAnswer = true; 
          scrollY = 0; 
          fetchAndDrawView();
      } else {
          inputBuffer = "API Err";
      }
  } else { 
      inputBuffer = "Err " + String(httpCode); 
  } 
  http.end(); 
}

void fetchAndDrawView() {
  if(WiFi.status() != WL_CONNECTED) return; 
  
  u8g2.clearBuffer(); 
  u8g2.drawStr(0, 20, "Loading..."); 
  u8g2.sendBuffer();

  HTTPClient http;
  http.setTimeout(10000); 
  http.begin(serverBase + "/view?y=" + String(scrollY));
  
  if(http.GET() == 200) { 
    WiFiClient *s = http.getStreamPtr(); 
    int bytesRead = s->readBytes(viewBuffer, 512); 
    
    if(bytesRead > 0) { 
      u8g2.clearBuffer(); 
      u8g2.drawXBM(0, 0, 128, 32, viewBuffer); 
      
      // ডানপাশে স্ক্রলবার
      if (totalViewHeight > 0) {
          u8g2.drawBox(126, map(scrollY, 0, totalViewHeight, 0, 28), 2, 4);
      }
      u8g2.sendBuffer(); 
    } 
  } 
  http.end();
}

void drawTypingMode() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_micro_tr); 
  if(WiFi.status() == WL_CONNECTED) u8g2.drawBox(124, 0, 3, 3);
  if(isMathMode) u8g2.drawStr(0, 5, "M"); else u8g2.drawStr(0, 5, "T");
  
  u8g2.setFont(u8g2_font_6x10_tf);
  int len = inputBuffer.length();
  String show = inputBuffer;
  if (len > 18) show = ".." + inputBuffer.substring(len-18);
  u8g2.drawStr(0, 20, show.c_str());
  if ((millis()/500)%2==0) u8g2.drawStr(u8g2.getStrWidth(show.c_str()), 20, "_");
  u8g2.sendBuffer();
}
