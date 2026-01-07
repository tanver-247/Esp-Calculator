#include <WiFi.h>
#include <HTTPClient.h>
#include <U8g2lib.h>
#include <Wire.h>

// ==========================================
//              USER CONFIG
// ==========================================
struct WiFiCreds { const char* ssid; const char* pass; };

WiFiCreds myNetworks[] = {
  {"TP-Link_AD7C", "17082006"},
  {"iPhone",       "77332608"},
  {"Khadija",      "77332608"}
};
int totalNetworks = 3;
int currentNetworkIndex = 0;

String serverBase = "http://217.154.161.167:10976";

// ==========================================
//              HARDWARE SETUP
// ==========================================
#define SDA_PIN 8
#define SCL_PIN 9
#define BAT_PIN 10 

// Display Driver
U8G2_SSD1306_128X32_UNIVISION_F_SW_I2C u8g2(U8G2_R0, SCL_PIN, SDA_PIN, U8X8_PIN_NONE);

// Matrix Wiring (ESP32 Pins)
int rowPins[] = {4, 5, 6, 7}; 
int colPins[] = {0, 1, 2, 3}; 

// ==========================================
//              GLOBAL VARIABLES
// ==========================================
String inputBuffer = "";
unsigned long lastKeyPress = 0;
int t9CycleIndex = 0;
int lastKeyIndex = -1;

// History System
String historyBuffer[3];
int historyCount = 0;

// States
bool isViewingAnswer = false;
bool isMathMode = false;
bool isSystemMenu = false;
bool isSleepMode = false; 

// Navigation
int menuIndex = 0;
int screenBrightness = 120;
int scrollY = 0;
int totalViewHeight = 0;
uint8_t viewBuffer[512]; 

// Math Text Words for Key 0 in Math Mode
const char* mathWords[] = {" equal ", " sum ", " mean ", " by ", " div ", " int "};
int mathWordIndex = 0;

// ==========================================
//              KEY MAPS
// ==========================================
const char* t9Map[] = {
  " 0",         // 0
  ".,-/?!1@#",  // 1
  "abc2",       // 2
  "def3",       // 3
  "ghi4",       // 4
  "jkl5",       // 5
  "mno6",       // 6
  "pqrs7",      // 7
  "tuv8",       // 8
  "wxyz9"       // 9
};

const char* mathMap[] = {
  "0 ",               // 0
  "1.+*/%",           // 1
  "2^r23",            // 2
  "3abdpt",           // 3
  "4Ssmer",           // 4
  "5XYPQ",            // 5
  "6idx",             // 6
  "7$L",              // 7
  "8()[]{}",          // 8
  "9><gln"            // 9
};

// ==========================================
//              MAIN SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // 1. Setup Matrix
  for(int i=0; i<4; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], HIGH);
    pinMode(colPins[i], INPUT_PULLUP);
  }
  pinMode(BAT_PIN, INPUT);

  // 2. Setup Display
  u8g2.begin();
  u8g2.setContrast(screenBrightness);
  u8g2.setFont(u8g2_font_6x10_tf);
  
  // 3. Connect to First WiFi
  connectWiFi(currentNetworkIndex);
}

// ==========================================
//              MAIN LOOP
// ==========================================
void loop() {
  
  // --- 1. CHECK SLEEP COMBO (7 + DEL) ---
  if (checkSleepCombo()) {
    unsigned long start = millis();
    while(checkSleepCombo()) {
      if (millis() - start > 1500) { // 1.5 Sec Hold
        isSleepMode = !isSleepMode;
        if (isSleepMode) u8g2.setPowerSave(1); // Screen OFF
        else u8g2.setPowerSave(0); // Screen ON
        
        while(checkSleepCombo()) delay(10); // Wait Release
        break;
      }
      delay(10);
    }
  }

  if (isSleepMode) { delay(100); return; }

  // --- 2. STANDARD OPERATION ---
  int key = scanMatrix();

  if (key != -1) {
    handleInput(key);
    delay(180); // Debounce
  }

  // --- 3. DISPLAY ---
  if (isSystemMenu) drawSystemMenu();
  else if (!isViewingAnswer) drawTypingMode();
}

// ==========================================
//           SLEEP COMBO CHECKER
// ==========================================
bool checkSleepCombo() {
  // Key 7 is R3/C1. Key DEL is R3/C4.
  digitalWrite(6, LOW); // Activate Row 3
  bool sevenPressed = (digitalRead(0) == LOW); // Col 1
  bool delPressed   = (digitalRead(3) == LOW); // Col 4
  digitalWrite(6, HIGH); // Reset Row 3
  return (sevenPressed && delPressed);
}

// ==========================================
//              INPUT ENGINE
// ==========================================
void handleInput(int key) {
  
  // --- MENU TRIGGER (Hold *) ---
  // Key 24 is '*'
  if (key == 24) { 
    unsigned long start = millis();
    // Keep checking as long as * is pressed
    while(scanMatrix() == 24) {
      if(millis() - start > 1500) { // 1.5 Sec Hold
        isSystemMenu = !isSystemMenu; 
        menuIndex = 0; 
        // Wait for release so we don't type '*' immediately after menu
        while(scanMatrix() == 24) delay(10); 
        return; 
      }
      delay(10);
    }
    // If we reach here, it was a short press.
    // Let it fall through to "SHORTCUTS" section below to print '*'
  }

  // --- SYSTEM BIOS CONTROLS ---
  if (isSystemMenu) {
    if (key == 12) { menuIndex--; if(menuIndex<0) menuIndex=3; } // UP (2)
    else if (key == 32) { menuIndex++; if(menuIndex>3) menuIndex=0; } // DOWN (8)
    else if (key == 21) adjustMenuValue(-1); // LEFT (4)
    else if (key == 23) adjustMenuValue(1);  // RIGHT (6)
    else if (key == 43) executeMenuAction(); // SELECT (x10)
    return;
  }

  // --- VIEW MODE NAV ---
  if (isViewingAnswer) {
    if (key == 12) { scrollY -= 16; if(scrollY < 0) scrollY = 0; fetchAndDrawView(); }
    else if (key == 32) { scrollY += 16; if(scrollY > totalViewHeight) scrollY = totalViewHeight; fetchAndDrawView(); }
    else if (key == 43) { isViewingAnswer = false; } // Exit
    return;
  }

  // --- TYPING MODE ---
  
  // 1. SEND / SPACE (Key 43 - x10)
  if (key == 43) {
    unsigned long start = millis();
    bool isLong = false;
    while(scanMatrix() == 43) {
      if (millis() - start > 500) { // Long Press -> SPACEBAR
        inputBuffer += " "; 
        isLong = true; 
        break; 
      }
    }
    if (!isLong) { // Short Press -> SEND
      if(inputBuffer.length() > 0) {
        addToHistory(inputBuffer);
        askServer(inputBuffer);
      }
    } else {
      while(scanMatrix() == 43); 
    }
    return;
  }

  // 2. BACKSPACE (Key 34 - DEL)
  if (key == 34) {
    unsigned long start = millis();
    while(scanMatrix() == 34) {
      if(millis() - start > 800) { inputBuffer=""; return; } // Long Press Clear
    }
    if(inputBuffer.length() > 0) inputBuffer.remove(inputBuffer.length()-1);
    return;
  }

  // 3. ZERO / MATH WORDS (Key 41 - 0)
  if (key == 41) {
    if (isMathMode) {
      inputBuffer += mathWords[mathWordIndex];
      mathWordIndex++;
      if(mathWordIndex > 5) mathWordIndex = 0;
    } else {
      processInput(10); // 10 maps to 0
    }
    return;
  }

  // 4. MATH TOGGLE (Key 42 - Dot)
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

  // 5. NUMBERS (1-9)
  int num = -1;
  if (key == 11) num = 1; else if (key == 12) num = 2; else if (key == 13) num = 3;
  else if (key == 21) num = 4; else if (key == 22) num = 5; else if (key == 23) num = 6;
  else if (key == 31) num = 7; else if (key == 32) num = 8; else if (key == 33) num = 9;
  
  if (num != -1) processInput(num);

  // 6. SHORTCUTS
  if (key == 14) { inputBuffer += "+"; lastKeyIndex = -1; }
  // Only add * if NOT opening menu
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
  
  int batRaw = analogRead(BAT_PIN);
  u8g2.setCursor(90, 32);
  if (batRaw > 500) { u8g2.print(map(batRaw,1800,2600,0,100)); u8g2.print("%"); }
  else u8g2.print("USB");
  
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
    for(int c=0; c<4; c++) {
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
  if(WiFi.status()!=WL_CONNECTED){inputBuffer="No WiFi";return;}
  u8g2.clearBuffer(); u8g2.drawStr(0,20,"Thinking..."); u8g2.sendBuffer();
  HTTPClient http; 
  String url = serverBase + "/ask?q=" + q; 
  url.replace(" ", "%20"); url.replace("+", "%2B");
  http.begin(url); 
  if(http.GET()==200 && http.getString().startsWith("OK:")) {
      totalViewHeight=http.getString().substring(3).toInt(); 
      isViewingAnswer=true; scrollY=0; fetchAndDrawView();
  } else inputBuffer="Err"; 
  http.end(); 
}

void fetchAndDrawView() {
  if(WiFi.status()!=WL_CONNECTED) return; 
  HTTPClient http;
  http.begin(serverBase + "/view?y=" + String(scrollY));
  if(http.GET()==200) { 
    WiFiClient *s = http.getStreamPtr(); 
    if(s->available()) { 
      s->readBytes(viewBuffer,512); 
      u8g2.clearBuffer(); 
      u8g2.drawXBM(0,0,128,32,viewBuffer); 
      u8g2.drawBox(127, map(scrollY,0,totalViewHeight,0,28), 1, 4);
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
