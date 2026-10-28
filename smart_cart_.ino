#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "HX711.h"
#include <string.h>
#include <math.h>

// ---------------- PIN DEFINITIONS ----------------
#define RSTPIN 9
#define SSPIN  10

#define DT  3
#define SCK 2

#define BUZZER 4

#define BTN_START  6
#define BTN_REMOVE 7
#define BTN_FINAL  8

// ---------------- MODULES ----------------
MFRC522 rc(SSPIN, RSTPIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
HX711 scale;

// ---------------- HX711 CALIBRATION ----------------
float calibration_factor = 25.0;      // <-- CALIBRATE FOR YOUR LOAD CELL
const float WEIGHT_TOLERANCE = 200.0; // grams

// ---------------- PRODUCT DATABASE ----------------
struct Product {
  byte uid[4];
  const char *name;
  float price;
  float weight;
};

Product products[] = {
  {{0xD7, 0xC7, 0xA7, 0x64}, "1CELL",  30.0, 746.0},
  {{0x31, 0xFD, 0x58, 0x66}, "2CELL", 40.0, 1892.0}
};
const int PRODUCT_COUNT = sizeof(products) / sizeof(products[0]);

// ---------------- CART DATA ----------------
int cartQty[PRODUCT_COUNT] = {0};
float totalAmount    = 0.0;
float expectedWeight = 0.0;

bool cartActive   = false;
bool removingMode = false;
bool weightReady  = false;
bool weightAlarm  = false;

unsigned long lastScanTime    = 0;
unsigned long lastWeightCheck = 0;
unsigned long lastAlarmBeep   = 0;

const unsigned long SCAN_GRACE_MS          = 3000;
const unsigned long CHECK_INTERVAL_MS      = 300;
const unsigned long ALARM_BEEP_MS          = 500;
const unsigned long CARD_REMOVE_TIMEOUT_MS = 2000;

// Button debounce
unsigned long lastBtnPress[3] = {0, 0, 0};
const unsigned long DEBOUNCE_MS = 250;

// ---------------- BUZZER ----------------
void beepWarning() { tone(BUZZER, 2000, 250); }
void beepOk()      { tone(BUZZER, 1200, 120); }
void beepDouble()  { tone(BUZZER, 1500, 100); delay(120); tone(BUZZER, 1500, 100); }

// ---------------- LCD HELPERS ----------------
void showMessage(const char *line1, const char *line2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
}

void showTotals() {
  lcd.clear();
  int itemCount = 0;
  for (int i = 0; i < PRODUCT_COUNT; i++) itemCount += cartQty[i];

  lcd.setCursor(0, 0);
  lcd.print("Items:");
  lcd.print(itemCount);
  lcd.print(" Total:");

  lcd.setCursor(0, 1);
  lcd.print("Rs ");
  lcd.print(totalAmount, 2);
}

// ---------------- RFID LOOKUP ----------------
int findProduct() {
  if (rc.uid.size != 4) return -1;
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    if (memcmp(rc.uid.uidByte, products[i].uid, 4) == 0) return i;
  }
  return -1;
}

// ---------------- BUTTON HELPER (ACTIVE-LOW w/ INPUT_PULLUP) ----------------
// Wiring: pin -- button -- GND.  Internal pull-up holds pin HIGH when idle.
// Pressed -> pin reads LOW.
// Returns true ONCE per press (falling edge HIGH -> LOW), debounced.
bool buttonPressed(uint8_t pin, int idx) {
  static bool lastState[3] = {HIGH, HIGH, HIGH};   // idle = HIGH

  bool state = digitalRead(pin);
  bool pressed = false;

  if (state == LOW && lastState[idx] == HIGH &&
      millis() - lastBtnPress[idx] > DEBOUNCE_MS) {
    lastBtnPress[idx] = millis();
    pressed = true;
  }
  lastState[idx] = state;
  return pressed;
}

// ---------------- CART HELPERS ----------------
int cartItemCount() {
  int n = 0;
  for (int i = 0; i < PRODUCT_COUNT; i++) n += cartQty[i];
  return n;
}

void resetCart() {
  for (int i = 0; i < PRODUCT_COUNT; i++) cartQty[i] = 0;
  totalAmount    = 0.0;
  expectedWeight = 0.0;
  cartActive     = false;
  removingMode   = false;
  weightAlarm    = false;
  noTone(BUZZER);
  digitalWrite(BUZZER, LOW);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(9600);

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  // Buttons wired pin -> button -> GND : use internal pull-ups.
  pinMode(BTN_START,  INPUT_PULLUP);
  pinMode(BTN_REMOVE, INPUT_PULLUP);
  pinMode(BTN_FINAL,  INPUT_PULLUP);

  Wire.begin();
  lcd.init();
  lcd.backlight();

  showMessage("Smart Shopping", "Cart Starting");
  delay(1500);

  SPI.begin();
  rc.PCD_Init();
  Serial.println("Smart Shopping Cart");

  scale.begin(DT, SCK);
  showMessage("Initializing", "Load Cell...");

  if (scale.wait_ready_timeout(3000)) {
    scale.set_scale(calibration_factor);
    scale.tare();
    weightReady = true;
    Serial.println("HX711 ready.");
  } else {
    Serial.println("HX711 not detected!");
    showMessage("HX711 ERROR", "Check wiring");
    delay(2000);
  }

  showMessage("Press D6", "to START cart");
}

// ---------------- MAIN LOOP ----------------
void loop() {
  // ============ D6 : START CART ============
  if (buttonPressed(BTN_START, 0)) {
    if (!cartActive) {
      resetCart();
      cartActive = true;
      if (weightReady) scale.tare();
      lastScanTime = millis();

      beepOk();
      showMessage("Cart STARTED", "Scan items...");
      Serial.println("Cart started.");
      delay(900);
      showTotals();
    } else {
      showMessage("Cart already", "started");
      beepWarning();
      delay(900);
      showTotals();
    }
  }

  // ============ D7 : TOGGLE REMOVE MODE ============
  if (buttonPressed(BTN_REMOVE, 1)) {
    if (!cartActive) {
      showMessage("Press D6", "to START first");
      beepWarning();
      delay(900);
      showMessage("Press D6", "to START cart");
    } else if (!removingMode) {
      removingMode = true;
      beepDouble();
      showMessage("REMOVE MODE ON", "Scan items to rm");
      Serial.println("Remove mode ON.");
      delay(900);
      showTotals();
    } else {
      removingMode = false;
      beepOk();
      showMessage("REMOVE MODE OFF", "Scan to add");
      Serial.println("Remove mode OFF.");
      delay(900);
      showTotals();
    }
  }

  // ============ D8 : FINAL TOTAL & RESET ============
  if (buttonPressed(BTN_FINAL, 2)) {
    noTone(BUZZER);
    digitalWrite(BUZZER, LOW);
    weightAlarm = false;

    if (!cartActive) {
      showMessage("Cart not", "started");
      beepWarning();
      delay(900);
      showMessage("Press D6", "to START cart");
    } else if (cartItemCount() == 0) {
      showMessage("Cart is empty", "Nothing to pay");
      beepWarning();
      delay(1200);
      resetCart();
      showMessage("Press D6", "to START cart");
    } else {
      beepDouble();
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("FINAL TOTAL");
      lcd.setCursor(0, 1); lcd.print("Rs "); lcd.print(totalAmount, 2);

      Serial.print("FINAL TOTAL: Rs "); Serial.println(totalAmount, 2);
      Serial.print("Items: "); Serial.println(cartItemCount());

      delay(3000);

      resetCart();
      if (weightReady) scale.tare();

      showMessage("Thank you!", "Cart reset");
      delay(1500);
      showMessage("Press D6", "to START cart");
      Serial.println("Cart reset. Ready for next customer.");
    }
  }

  // ============ CONTINUOUS WEIGHT READOUT (serial monitor) ============
  static unsigned long lastReadout = 0;
  if (weightReady && scale.is_ready() && millis() - lastReadout >= 500) {
    lastReadout = millis();

    float measured = scale.get_units(3);
    if (measured < 0 && measured > -WEIGHT_TOLERANCE) measured = 0;

    float diff = measured - expectedWeight;

    Serial.print("Expected: ");
    Serial.print(expectedWeight, 1);
    Serial.print(" g  |  Measured: ");
    Serial.print(measured, 1);
    Serial.print(" g  |  Diff: ");
    Serial.print(diff, 1);
    Serial.print(" g  |  Cart: ");
    Serial.print(cartActive ? "ACTIVE" : "IDLE");
    Serial.print("  |  Mode: ");
    Serial.print(removingMode ? "REMOVE" : "ADD");
    Serial.print("  |  Items: ");
    Serial.println(cartItemCount());
  }

  // ============ WEIGHT MONITORING (continuous beep on mismatch) ============
  if (cartActive && weightReady && scale.is_ready()) {
    if (millis() - lastWeightCheck >= CHECK_INTERVAL_MS) {
      lastWeightCheck = millis();

      float measuredWeight = scale.get_units(3);
      if (measuredWeight < 0 && measuredWeight > -WEIGHT_TOLERANCE) measuredWeight = 0;

      float difference = measuredWeight - expectedWeight;
      bool graceOver = millis() - lastScanTime >= SCAN_GRACE_MS;

      if (graceOver && fabs(difference) > WEIGHT_TOLERANCE) {
        if (!weightAlarm) {
          weightAlarm = true;
          showMessage("UNSCANNED ITEM!", "Scan or remove");
          Serial.print("Mismatch! Measured=");
          Serial.print(measuredWeight, 1);
          Serial.print(" g  Expected=");
          Serial.print(expectedWeight, 1);
          Serial.print(" g  Diff=");
          Serial.println(difference, 1);
        }
        if (millis() - lastAlarmBeep >= ALARM_BEEP_MS) {
          lastAlarmBeep = millis();
          tone(BUZZER, 2000, 250);
        }
      } else {
        if (weightAlarm) {
          weightAlarm = false;
          noTone(BUZZER);
          digitalWrite(BUZZER, LOW);
          showTotals();
          Serial.println("Weight OK - alarm cleared.");
        }
      }
    }
  }

  // ============ RFID SCANNING ============
  if (rc.PICC_IsNewCardPresent() && rc.PICC_ReadCardSerial()) {

    if (!cartActive) {
      showMessage("Press D6", "to START cart");
      beepWarning();
      rc.PICC_HaltA();
      rc.PCD_StopCrypto1();
      unsigned long t0 = millis();
      while (rc.PICC_IsNewCardPresent() && millis() - t0 < CARD_REMOVE_TIMEOUT_MS) delay(20);
      return;
    }

    int index = findProduct();

    if (index >= 0 && !removingMode) {
      // ---------- ADD ----------
      cartQty[index]++;
      totalAmount    += products[index].price;
      expectedWeight += products[index].weight;
      lastScanTime = millis();

      beepOk();
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Added: "); lcd.print(products[index].name);
      lcd.setCursor(0, 1); lcd.print("Rs ");     lcd.print(products[index].price, 2);
      Serial.print("Added: "); Serial.println(products[index].name);
      delay(1200);
      if (!weightAlarm) showTotals();

    } else if (index >= 0 && removingMode) {
      // ---------- REMOVE (mode stays ON) ----------
      if (cartQty[index] > 0) {
        cartQty[index]--;
        totalAmount    -= products[index].price;
        if (totalAmount < 0) totalAmount = 0;
        expectedWeight -= products[index].weight;
        if (expectedWeight < 0) expectedWeight = 0;
        lastScanTime = millis();

        beepOk();
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Removed:"); lcd.print(products[index].name);
        lcd.setCursor(0, 1); lcd.print("Rs ");      lcd.print(products[index].price, 2);
        Serial.print("Removed: "); Serial.println(products[index].name);
      } else {
        showMessage("Not in cart!", products[index].name);
        beepWarning();
      }
      delay(1200);
      if (!weightAlarm) showTotals();

    } else {
      // ---------- UNKNOWN ----------
      showMessage("Unknown RFID", "Item not added");
      beepWarning();
      Serial.println("Unknown RFID UID!");
      delay(1200);
      if (!weightAlarm) showTotals();
    }

    rc.PICC_HaltA();
    rc.PCD_StopCrypto1();

    unsigned long t0 = millis();
    while (rc.PICC_IsNewCardPresent() && millis() - t0 < CARD_REMOVE_TIMEOUT_MS) delay(20);
  }
}
