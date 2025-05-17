#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEDescriptor.h> // For BLE2902
#include <BLEUUID.h>       // For BLEUUID
#include <Wire.h>
#include "UNIT_SCALES.h"

#define REED_PIN       D3       // your lid switch
#define MEASURE_MS     2000     // sample scale every 2 s
#define STABLE_MS      5000     // emit net change after 5 s quiet
#define NOISE_G        10.0f     // ignore ±2 g jitter

// --- BLE UUIDs ---
static BLEUUID SM_SERVICE_UUID( "4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID LID_CHAR_UUID(   "00002a56-0000-1000-8000-00805f9b34fb");
static BLEUUID DRINK_CHAR_UUID( "00002a57-0000-1000-8000-00805f9b34fb");

// --- Globals ---
UNIT_SCALES scale;
BLECharacteristic* lidChar;
BLECharacteristic* drinkChar;
BLEServer *pServer = NULL; // Keep a global pointer to the server

float         lastWeight    = 0.0f;
float         accDelta      = 0.0f;
unsigned long lastRead      = 0;
unsigned long lastEventTime = 0;
bool          lastLidClosed = false;
bool deviceConnected = false; // Track connection state

// Callback class for server events
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServerInstance) {
      deviceConnected = true;
      Serial.println("Device connected");
      // You could stop advertising here if you only want one connection at a time
      // pServerInstance->getAdvertising()->stop(); 
    }

    void onDisconnect(BLEServer* pServerInstance) {
      deviceConnected = false;
      Serial.println("Device disconnected, restarting advertising...");
      // It's important to use the pServerInstance passed to onDisconnect
      // or the global pServer pointer if you are sure it's the same.
      pServerInstance->getAdvertising()->start(); 
      // Or, for some ESP32 BLE libraries/versions, you might need:
      // BLEDevice::startAdvertising();
    }
};

void setup() {
  Serial.begin(115200);
  pinMode(REED_PIN, INPUT_PULLUP);

  // init scale
  Wire.begin();
  if (!scale.begin()) {
    Serial.println("❌ Scale not found!");
    while (1) delay(1000);
  }
  lastWeight    = scale.getWeight();
  lastLidClosed = (digitalRead(REED_PIN)==LOW); // Assuming LOW is closed for reed switch
  lastRead      = millis();
  lastEventTime = millis();

  // init BLE
  BLEDevice::init("SmartMug");
  pServer = BLEDevice::createServer(); 
  pServer->setCallbacks(new MyServerCallbacks()); 

  BLEService *svc = pServer->createService(SM_SERVICE_UUID);

  lidChar = svc->createCharacteristic(
    LID_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  lidChar->addDescriptor(new BLEDescriptor(BLEUUID((uint16_t)0x2902))); // Standard way to add CCCD

  drinkChar = svc->createCharacteristic(
    DRINK_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  drinkChar->addDescriptor(new BLEDescriptor(BLEUUID((uint16_t)0x2902))); // Standard way to add CCCD

  // You can add descriptors if needed, e.g., for client characteristic configuration (CCC)
  // lidChar->addDescriptor(new BLE2902());
  // drinkChar->addDescriptor(new BLE2902());


  svc->start();
  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising(); // Get the advertising object
  pAdvertising->addServiceUUID(SM_SERVICE_UUID); // Advertise your main service
  pAdvertising->setScanResponse(true);
  //pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  //pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising(); // More robust way to start advertising
  Serial.println("✅ BLE SmartMug ready and advertising");
}

void loop() {
  unsigned long now = millis();

  // Lid polling logic seems okay, but ensure digitalRead logic is correct for your switch
  // For INPUT_PULLUP, a closed switch usually pulls the pin LOW.
  // Your original code had: lastLidClosed = (digitalRead(REED_PIN)==LOW);
  // And then in loop: bool lidClosed = (digitalRead(REED_PIN)==HIGH);
  // This is inconsistent. Let's assume LOW means closed.
  bool lidClosed = (digitalRead(REED_PIN) == LOW); 
  if (lidClosed != lastLidClosed) {
    lastLidClosed = lidClosed;
    String ts = String(now);
    String msg = lidClosed ? "LID_CLOSED" : "LID_OPEN";
    // msg += "@" + ts; // The @timestamp was removed from Flutter parsing, ensure consistency
    if (deviceConnected) { // Only send if connected
        lidChar->setValue(msg.c_str());
        lidChar->notify();
    }
    Serial.println(">> " + msg + (deviceConnected ? " (Notified)" : " (Not connected)"));
  }

  // — weight sampling —
  if (now - lastRead >= MEASURE_MS) {
    lastRead = now;
    float w = scale.getWeight();
    float d = w - lastWeight;
    if (fabs(d) > NOISE_G) {
      accDelta     += d;        // accumulate net change
      lastEventTime = now;      // reset stable timer
    }
    lastWeight = w;
  }

  // — when stable, flush one net event —
  if (accDelta != 0.0f && (now - lastEventTime) >= STABLE_MS) {
    float ml  = accDelta;           // 1 g≈1 ml
    String ts = String(now);
    String tag;

    // 4 states:
    if (ml <  0 && !lastLidClosed) {
      tag = "DRINKING_" + String((int)(-ml)) + "_ml@" + ts;
    }
    else if (ml >  0 && !lastLidClosed) {
      tag = "ADDING_" + String((int)(ml)) + "_ml@" + ts;
    }
    else if (ml <  0 &&  lastLidClosed) {
      tag = "THROWING_" + String((int)(-ml)) + "_ml@" + ts;
    }
    else if (ml >  0 &&  lastLidClosed) {
      tag = "ADDING_" + String((int)(ml)) + "_ml@" + ts;
    }

    if (tag.length()) {
      if (deviceConnected) { // Only send if connected
        drinkChar->setValue(tag.c_str());
        drinkChar->notify();
      }
      Serial.println(">> " + tag + (deviceConnected ? " (Notified)" : " (Not connected)"));
    }
    accDelta      = 0.0f;
    lastEventTime = now;
  }
  delay(50);
}
