/*
  WiFi connected round LED Clock. It gets NTP time from the internet and translates to a 60 RGB WS2812B LED strip.

  If you have another orientation where the wire comes out then change the methods getLEDHour and getLEDMinuteOrSecond

  Happy programming, Leon van den Beukel, march 2019

  ---  
  NTP and summer time code based on:
  https://tttapa.github.io/ESP8266/Chap15%20-%20NTP.html 
  https://github.com/SensorsIot/NTPtimeESP/blob/master/NTPtimeESP.cpp (for US summer time support check this link)  
  
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#define FASTLED_ESP8266_RAW_PIN_ORDER // see https://github.com/FastLED/FastLED/wiki/ESP8266-notes
#include <FastLED.h>
//#define DEBUG_ON

WiFiManager wifiManager;
// because of callbacks, these need to be in the higher scope :(
WiFiManagerParameter* wifiStaticIP = NULL;
WiFiManagerParameter* wifiStaticIPNetmask = NULL;
WiFiManagerParameter* wifiStaticIPGateway = NULL;

// ###NTP and time config###
unsigned long timeZone = 1.0;                     // Change this value to your local timezone (in my case +1 for Amsterdam)
const char* NTPServerName = "nl.pool.ntp.org";    // Change this to a ntpserver nearby, check this site for a list of servers: https://www.pool.ntp.org/en/
unsigned long intervalNTP = 24 * 60 * 60 * 1000;      // Request a new NTP time every 24 hours
unsigned long NTPmaxWait =  60 * 1000; // Maximum Time to wait for NTP request

IPAddress timeServerIP;                         
const int NTP_PACKET_SIZE = 48;                 
byte NTPBuffer[NTP_PACKET_SIZE];                

unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();
uint32_t timeUNIX = 0;
unsigned long prevActualTime = 0;

#define LEAP_YEAR(Y) ( ((1970+Y)>0) && !((1970+Y)%4) && ( ((1970+Y)%100) || !((1970+Y)%400) ) )
static const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

struct DateTime {
  int  year;
  byte month;
  byte day;
  byte hour;
  byte minute;
  byte second;
  byte dayofweek;
};

DateTime currentDateTime;

// ###FastLED Basics###

// Change the colors here if you want.
// Check for reference: https://github.com/FastLED/FastLED/wiki/Pixel-reference#predefined-colors-list
// You can also set the colors with RGB values, for example red:
// CRGB colorHour = CRGB(255, 0, 0);
CRGB colorHour = CRGB::Red;
CRGB colorMinute = CRGB::Green;
CRGB colorSecond = CRGB::Blue;

// Set this to true if you want the hour LED to move between hours (if set to false the hour LED will only move every hour)
#define USE_LED_MOVE_BETWEEN_HOURS true

// Cutoff times for day / night brightness.
#define USE_NIGHTCUTOFF true   // Enable/Disable night brightness
#define MORNINGCUTOFF 8        // When does daybrightness begin?   8am
#define NIGHTCUTOFF 20         // When does nightbrightness begin? 10pm
#define NIGHTBRIGHTNESS 60     // Brightness level from 0 (off) to 255 (full brightness)
#define DAYBRIGHTNESS 255      // Brightness level from 0 (off) to 255 (full brightness)
          
WiFiUDP UDP;                                    

#define NUM_LEDS 60
#define OFFSET_LEDS 0 //set offset of used LEDS.
#define DATA_PIN D4 //D1, D4 tested on NodeMCU Amica Modul V2 ESP8266 ESP-12F
#define BAUD_RATE 9600 //
CRGB LEDs[NUM_LEDS];

void setup() {

  FastLED.delay(3000);
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(LEDs, NUM_LEDS);  

  Serial.begin(BAUD_RATE);
  delay(10);
  Serial.println("\r\n");

  startWiFi();
  startUDP();

  if(!WiFi.hostByName(NTPServerName, timeServerIP)) { 
    Serial.println("DNS lookup failed. Rebooting.");
    Serial.flush();
    ESP.reset();
  }
  Serial.print("Time server IP:\t");
  Serial.println(timeServerIP);
  
  Serial.println("\r\nSending NTP request ...");
  sendNTPpacket(timeServerIP);  
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - prevNTP > intervalNTP) { // If last NTP request inteval has passed
    prevNTP = currentMillis;
    Serial.println("\r\nSending NTP request ...");
    sendNTPpacket(timeServerIP);               // Send an NTP request
  }

  uint32_t time = getTime();                   // Check if an NTP response has arrived and get the (UNIX) time
  if (time) {                                  // If a new timestamp has been received
    timeUNIX = time;
    Serial.print("NTP response:\t");
    Serial.println(timeUNIX);
    lastNTPResponse = currentMillis;
  } else if ((currentMillis - lastNTPResponse) > ( intervalNTP + NTPmaxWait ) ) {
    Serial.print("More than ");
    Serial.print( ( NTPmaxWait / 1000 ) );
    Serial.print(" seconds since last NTP response. Rebooting.");
    Serial.flush();
    ESP.reset();
  }

  uint32_t actualTime = timeUNIX + (currentMillis - lastNTPResponse)/1000;
  if (actualTime != prevActualTime && timeUNIX != 0) { // If a second has passed since last update
    FastLED.clear();

    prevActualTime = actualTime;
    convertTime(actualTime);

    int second = getLEDMinuteOrSecond(currentDateTime.second);
    int minute = getLEDMinuteOrSecond(currentDateTime.minute);
    int hour = getLEDHour(currentDateTime.hour, currentDateTime.minute);

    // Set "Hands"
    LEDs[second] = colorSecond;
    LEDs[minute] += colorMinute;  
    LEDs[hour] += colorHour;

    if ( night() && USE_NIGHTCUTOFF == true )
      FastLED.setBrightness (NIGHTBRIGHTNESS);
    else
      FastLED.setBrightness (DAYBRIGHTNESS);

    FastLED.show();
    FastLED.show();
  }  
}

byte getLEDHour(byte hours, byte minutes) {
  if (hours > 12)
    //convert 24h back to 12
    hours = hours - 12;

  byte hourLED;
  if (hours <= 5)
    hourLED = (hours * ( NUM_LEDS / 12 )) + OFFSET_LEDS;
  else
    hourLED = (hours * ( NUM_LEDS / 12 )) - OFFSET_LEDS;

  if (USE_LED_MOVE_BETWEEN_HOURS == true) {
    if        (minutes >= 12 && minutes < 24) {
      hourLED += 1;
    } else if (minutes >= 24 && minutes < 36) {
      hourLED += 2;
    } else if (minutes >= 36 && minutes < 48) {
      hourLED += 3;
    } else if (minutes >= 48) {
      hourLED += 4;
    }
  }

  return hourLED;  
}

byte getLEDMinuteOrSecond(byte minuteOrSecond) {
  if (minuteOrSecond < OFFSET_LEDS)
    return minuteOrSecond + OFFSET_LEDS;
  else 
    return minuteOrSecond - OFFSET_LEDS;
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
  for (int i=0; i<NUM_LEDS; i++) {
    LEDs[i] = CRGB::Yellow;
    FastLED.show();
    FastLED.delay(25);
  }
}

void startWiFi() {
  String ssid = "RC" + String(ESP.getChipId());

  FastLED.clear();
  FastLED.show();

  //fancy boot effect
  for (int i=0; i<NUM_LEDS; i++) {
    LEDs[i] = CRGB::Blue;
    FastLED.show();
    FastLED.delay(25);
  }

  wifiManager.setAPCallback(configModeCallback);

  wifiManager.setConfigPortalTimeout(180);

  if (wifiManager.autoConnect(ssid.c_str(), "roundclock")) {
    // set LED for successful operation
    for (int i=0; i<NUM_LEDS; i++) {
      LEDs[i] = CRGB::Green;
      FastLED.show();
      FastLED.delay(25);
    }
    Serial.println(F("Wifi connected succesfully\n"));
    Serial.println("\r\n");
    Serial.print("Connected to ");
    Serial.println(WiFi.SSID());
    Serial.print("IP address:\t");
    Serial.print(WiFi.localIP());
    Serial.println("\r\n");

    // if the config portal was started, make sure to turn off the config AP
    WiFi.mode(WIFI_STA);
  } else {
    // set LED for Wifi failed
    fill_solid(LEDs,NUM_LEDS, CRGB::Red);
    Serial.println(F("Wifi failed.  Restarting in 10 seconds.\n"));

    delay(10000);
    ESP.restart();
  }
}

void startUDP() {
  Serial.println("Starting UDP");
  UDP.begin(123);                          // Start listening for UDP messages on port 123
  Serial.print("Local port:\t");
  Serial.println(UDP.localPort());
  Serial.println();
}

uint32_t getTime() {
  if (UDP.parsePacket() == 0) { // If there's no response (yet)
    return 0;
  }
  UDP.read(NTPBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
  // Combine the 4 timestamp bytes into one 32-bit number
  uint32_t NTPTime = (NTPBuffer[40] << 24) | (NTPBuffer[41] << 16) | (NTPBuffer[42] << 8) | NTPBuffer[43];
  // Convert NTP time to a UNIX timestamp:
  // Unix time starts on Jan 1 1970. That's 2208988800 seconds in NTP time:
  const uint32_t seventyYears = 2208988800UL;
  // subtract seventy years:
  uint32_t UNIXTime = NTPTime - seventyYears;
  return UNIXTime;
}

void sendNTPpacket(IPAddress& address) {
  memset(NTPBuffer, 0, NTP_PACKET_SIZE);  // set all bytes in the buffer to 0
  // Initialize values needed to form NTP request
  NTPBuffer[0] = 0b11100011;   // LI, Version, Mode
  // send a packet requesting a timestamp:
  UDP.beginPacket(address, 123); // NTP requests are to port 123
  UDP.write(NTPBuffer, NTP_PACKET_SIZE);
  UDP.endPacket();
}

void convertTime(uint32_t time) {
  // Correct time zone
  time += (3600 * timeZone);
  currentDateTime.second = time % 60;
  currentDateTime.minute = time / 60 % 60;
  currentDateTime.hour   = time / 3600 % 24;
  time  /= 60;  // To minutes
  time  /= 60;  // To hours
  time  /= 24;  // To days
  currentDateTime.dayofweek = ((time + 4) % 7) + 1;
  int year = 0;
  int days = 0;
  while ((unsigned)(days += (LEAP_YEAR(year) ? 366 : 365)) <= time) {
    year++;
  }
  days -= LEAP_YEAR(year) ? 366 : 365;
  time  -= days; // To days in this year, starting at 0  
  days = 0;
  byte month = 0;
  byte monthLength = 0;
  for (month = 0; month < 12; month++) {
    if (month == 1) { // February
      if (LEAP_YEAR(year)) {
        monthLength = 29;
      } else {
        monthLength = 28;
      }
    } else {
      monthLength = monthDays[month];
    }
  
    if (time >= monthLength) {
      time -= monthLength;
    } else {
      break;
    }
  }
 
  currentDateTime.day = time + 1;
  currentDateTime.year = year + 1970;
  currentDateTime.month = month + 1;  

  // Correct European Summer time
  if (summerTime()) {
    currentDateTime.hour += 1;
  }

#ifdef DEBUG_ON
  Serial.print(currentDateTime.year);
  Serial.print(" ");
  Serial.print(currentDateTime.month);
  Serial.print(" ");
  Serial.print(currentDateTime.day);
  Serial.print(" ");
  Serial.print(currentDateTime.hour);
  Serial.print(" ");
  Serial.print(currentDateTime.minute);
  Serial.print(" ");
  Serial.print(currentDateTime.second);
  Serial.print(" day of week: ");
  Serial.print(currentDateTime.dayofweek);
  Serial.print(" summer time: ");
  Serial.print(summerTime());
  Serial.print(" night time: ");
  Serial.print(night());  
  Serial.println();
#endif
}

boolean summerTime() {

  if (currentDateTime.month < 3 || currentDateTime.month > 10) return false;  // No summer time in Jan, Feb, Nov, Dec
  if (currentDateTime.month > 3 && currentDateTime.month < 10) return true;   // Summer time in Apr, May, Jun, Jul, Aug, Sep
  if (currentDateTime.month == 3 && (currentDateTime.hour + 24 * currentDateTime.day) >= (3 +  24 * (31 - (5 * currentDateTime.year / 4 + 4) % 7)) || currentDateTime.month == 10 && (currentDateTime.hour + 24 * currentDateTime.day) < (3 +  24 * (31 - (5 * currentDateTime.year / 4 + 1) % 7)))
    return true;
  else
    return false;
}

boolean night() {
  return ( currentDateTime.hour >= NIGHTCUTOFF ) || ( currentDateTime.hour < MORNINGCUTOFF );
}
