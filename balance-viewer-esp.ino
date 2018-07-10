#define DIGITS 6
#define BTN1_PIN D5
#define BTN2_PIN D6
#define SDA_PIN D2
#define SCL_PIN D1
#define EEPROM_SELECTED_ACCOUNT_ADDR 0
#define I2C_DISPLAY_ADDRESS 1

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include "secrets.h"
#include <EEPROM.h>

// hgfedcba
const byte segments[] = {
  0x3F,
  0x06,
  0x5B,
  0x4F,
  0x66,
  0x6D,
  0x7D,
  0x07,
  0x7F,
  0x6F,
  0x77,
  0x7C,
  0x39,
  0x5E,
  0x79,
  0x71
};

byte values[DIGITS];
const byte connErr[] = { segments[0xC], segments[0], B10110111, segments[0xE], B01010000, B11010000 };
const byte inetErr[] = { B00110000, B00110111, segments[0xE], B11111000, segments[0xE], B11010000 };
const byte httpsErr[] = { B01110110, B01111000, B01111000, B01110011, B01101101, 0 };
const byte jsonErr[] = { B00001110, B01101101, segments[0], B10110111, segments[0xE], B11010000 };
// const byte good[] = { B01101111, segments[0], segments[0], segments[0xD], 0, 0 };
// const byte load[] = { B00111000, segments[0], segments[0xA], segments[0xD] | B10000000, B10000000, B10000000 };
// const byte bunq[] = { B01111100, B00011100, B01010100, B01100111, 0, 0 }; // bunq small case
const byte noAcct[] = { B00110111, segments[0] | B10000000, segments[0xA], segments[0xC], segments[0xC], B11111000 };
const byte bunq[] = { 0, B01111100, B00111110, B00110111, B01100111, 0 };
String payload;
bool interruptsAttached = false;

unsigned long prevBtn1Press = 0;

void btn1Pressed() {
  if (millis() - prevBtn1Press < 500) {
    return;
  }
  prevBtn1Press = millis();
  
  byte old = EEPROM.read(EEPROM_SELECTED_ACCOUNT_ADDR);
  EEPROM.write(EEPROM_SELECTED_ACCOUNT_ADDR, old + 1);
  EEPROM.commit();
  displayBalance();
}

void btn2Pressed() {
  
}

void setup() {
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);

  // reserve 1 byte in eeprom memory to remember which account is selected
  EEPROM.begin(1);
  
  Wire.begin(SDA_PIN, SCL_PIN); //sda, scl

  delay(750); //give mini time to boot up
  
  transmit(bunq);
  
  Serial.begin(9600);

  WiFi.begin(wifiSsid, wifiPassword);

  while (WiFi.status() != WL_CONNECTED) {
    delay(50);
  }
}

StaticJsonBuffer<4096> jsonBuffer;

const unsigned short INTERVAL = 30000;

unsigned int pow10int(byte p) {
  unsigned int n = 1;
  for (byte i = 0; i < p; i ++) {
    n *= 10;
  }
  return n;
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    if (!client.connect(apiHost, apiHttpsPort)) {
      transmit(connErr);
      delay(INTERVAL);
      return;
    }

    if (!client.verify(apiHttpsCertificateFingerprint, apiHost)) {
      transmit(httpsErr);
      delay(INTERVAL);
      return;
    }

    client.print(String("GET /get_balances HTTP/1.1\r\n") +
      "Host: " + apiHost + "\r\n" +
      "User-Agent: Balance Viewer\r\n" +
      "Cookie: secret=" + apiSecret + "\r\n" +
      "Connection: close\r\n\r\n");

    while (client.connected()) {
      String line = client.readStringUntil('\n');
      if (line == "\r") {
        break;
      }
    }

    payload = client.readStringUntil('\n');

    client.stop();

    displayBalance();
  } else {
    transmit(inetErr);
  }

  delay(INTERVAL);
}

void displayBalance() {
  if (!interruptsAttached) {
    attachInterrupt(digitalPinToInterrupt(BTN1_PIN), btn1Pressed, FALLING);
    attachInterrupt(digitalPinToInterrupt(BTN2_PIN), btn2Pressed, FALLING);
    interruptsAttached = true;
    Serial.println("Interrupts attached!");
  }
  
  JsonObject& root = jsonBuffer.parseObject(payload);

  if (!root.success()) {
    transmit(jsonErr);
    return;
  }

  byte selectedAccount = EEPROM.read(EEPROM_SELECTED_ACCOUNT_ADDR);
  byte i = 0;
  float balance = -1;
  bool accountNotFound = true;
  bool noAccountsFound = true;
  for (auto kv : root) {
    noAccountsFound = false;
    balance = kv.value.as<float>();
    
    if (selectedAccount == i) {
      accountNotFound = false;
      break;
    }
    i ++;
  }

  if (noAccountsFound) {
    transmit(noAcct);
    return;
  }

  if (selectedAccount == 255) {
    accountNotFound = false;
    EEPROM.write(EEPROM_SELECTED_ACCOUNT_ADDR, i - 1);
    EEPROM.commit();
  }

  if (accountNotFound) {
    EEPROM.write(EEPROM_SELECTED_ACCOUNT_ADDR, 0);
    EEPROM.commit();
    displayBalance();
    return;
  }

  int balanceInt = balance;
  
  jsonBuffer.clear();

  byte leadingDigits;

  if (balanceInt > 99999) {
    leadingDigits = 6;
  } else if (balanceInt > 9999) {
    leadingDigits = 5;
  } else if (balanceInt > 999) {
    leadingDigits = 4;
  } else if (balanceInt > 99) {
    leadingDigits = 3;
  } else if (balanceInt > 9) {
    leadingDigits = 2;
  } else {
    leadingDigits = 1;
  }

  byte data[DIGITS];

  for (byte i = 0; i < leadingDigits; i ++) {
    data[i] = segments[(balanceInt % pow10int(leadingDigits - i)) / pow10int(leadingDigits - i - 1)];
  }

  if (leadingDigits < 5) {
    float trailing = balance - balanceInt;
    byte digit1 = 0;
    byte digit2 = 0;
    while (trailing > (float)0.09) {
      digit1 ++;
      trailing -= (float)0.10;
    }
    while (trailing > (float)0.00) {
      digit2 ++;
      trailing -= (float)0.01;
    }
    data[leadingDigits - 1] |= B10000000;
    data[leadingDigits] = segments[digit1];
    data[leadingDigits + 1] = segments[digit2];
    for (byte i = leadingDigits + 2; i < DIGITS; i ++) {
      data[i] = 0;
    }
  } else {
    for (byte i = leadingDigits; i < DIGITS; i ++) {
      data[i] = 0;
    }
  }

  byte emptyDigits = 0;
  for (byte i = DIGITS - 1; i >= 0; i --) {
    if (data[i] == 0) {
      emptyDigits ++;
    } else {
      break;
    }
  }
  
  if (emptyDigits != 0) {
    for (byte i = DIGITS - emptyDigits - 1; i >= 0 && i != 255; i --) {
      data[i + emptyDigits] = data[i];
      data[i] = 0;
    }
  }

  transmit(data);
}

void transmit(const byte data[]) {
  bool diff = false;
  for (byte i = 0; i < DIGITS; i ++) {
    if (data[i] != values[i]) {
      diff = true;
      break;
    }
  }

  if (!diff) {
    return;
  }

  memcpy(values, data, DIGITS);

  Wire.beginTransmission(I2C_DISPLAY_ADDRESS);
  Wire.write(values, DIGITS);
  Wire.endTransmission();
}

