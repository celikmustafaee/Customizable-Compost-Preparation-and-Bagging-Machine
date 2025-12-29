/*
  MUSTAFA ÇELİK 
  Project: Mycelium Biocomposite Machine

  impprtant values:
  - Load cell calibration factor: 108.28
  - Adaptive stepper speed is used for finer dosing near the target.
  - Relays are ACTIVE LOW (HIGH=OFF, LOW=ON)
*/

#include <HX711_ADC.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <EEPROM.h>

// -------------------- Hardware --------------------
LiquidCrystal_I2C lcd(0x27, 20, 4);
HX711_ADC LoadCell(A0, A1);

const int stepPins[] = {2, 5, 8, 11};
const int dirPins[]  = {3, 6, 9, 12};
const int enPins[]   = {4, 7, 10, 13};

#define pumpPin  23
#define mixerPin 25

// -------------------- Keypad --------------------
const byte ROWS = 4, COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'}, // A: menu up
  {'4','5','6','B'}, // B: menu down
  {'7','8','9','C'}, // C: emergency stop
  {'*','0','#','D'}  // D: start (from summary), #: confirm/OK
};

byte rowPins[4] = {36, 34, 32, 30};
byte colPins[4] = {28, 26, 24, 22};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// -------------------- Data --------------------
struct Recipe {
  int m1, water, m2, m3, m4, duration;
} activeRecipe;

enum MenuState { MAIN_MENU, EDIT_VALUES, SUMMARY, RUNNING };
MenuState currentMode = MAIN_MENU;

int menuIndex = 0;
int editStep = 0;
int tempValue = 0;

float targets[6];
int processStep = -1;

unsigned long mixStartTime = 0;
unsigned long lastLCDUpdate = 0;

float calibrationValue = 108.28;
String inputString = "";

void setup() {
  Serial.begin(9600);

  // Relay outputs (ACTIVE LOW)
  pinMode(pumpPin, OUTPUT);
  digitalWrite(pumpPin, HIGH);

  pinMode(mixerPin, OUTPUT);
  digitalWrite(mixerPin, HIGH);

  // Stepper driver pins
  for (int i = 0; i < 4; i++) {
    pinMode(stepPins[i], OUTPUT);
    pinMode(dirPins[i], OUTPUT);
    pinMode(enPins[i], OUTPUT);
    digitalWrite(enPins[i], HIGH);  // disable drivers at startup
  }

  // LCD + Load Cell init
  lcd.init();
  lcd.backlight();

  LoadCell.begin();
  LoadCell.start(2000, true);
  LoadCell.setCalFactor(calibrationValue);

  lcd.setCursor(0,0); lcd.print("SYSTEM READY");
  lcd.setCursor(0,1); lcd.print("MYC-COMP V15.0-PR");
  delay(1500);

  displayMainMenu();
}

void loop() {
  LoadCell.update();
  float currentWeight = LoadCell.getData();

  char key = keypad.getKey();

  // Emergency stop has priority
  if (key == 'C') {
    emergencyStop();
    return;
  }

  switch (currentMode) {
    case MAIN_MENU:   handleMainMenu(key); break;
    case EDIT_VALUES: handleValueInput(key); break;
    case SUMMARY:     handleSummary(key); break;
    case RUNNING:     handleAutomation(currentWeight, key); break;
  }
}

// -------------------- Automation (dosing + mixing) --------------------
void handleAutomation(float weight, char key) {

  // Step 0: tare
  if (processStep == 0) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("TARE IN PROGRESS...");
    LoadCell.tare();
    delay(1000);
    processStep = 1;
    lcd.clear();
  }
  // Steps 1..4: silo dosing
  else if (processStep >= 1 && processStep <= 4) {
    int idx = processStep - 1;
    float remaining = targets[idx] - weight;

    if (weight >= targets[idx]) {
      digitalWrite(enPins[idx], HIGH); // stop current motor
      processStep++;
      lcd.clear();
    } else {
      digitalWrite(enPins[idx], LOW);  // enable motor

      // Adaptive stepping: slow down close to target
      int stepDelay = 800;             // normal speed
      if (remaining < 30.0) stepDelay = 2500;
      if (remaining < 10.0) stepDelay = 4000;

      for (int i = 0; i < 30; i++) {
        digitalWrite(stepPins[idx], HIGH); delayMicroseconds(stepDelay);
        digitalWrite(stepPins[idx], LOW);  delayMicroseconds(stepDelay);
      }
    }
  }
  // Step 5: water pump until target is reached
  else if (processStep == 5) {
    if (weight >= targets[4]) {
      digitalWrite(pumpPin, HIGH);
      processStep = 6;
      lcd.clear();
    } else {
      digitalWrite(pumpPin, LOW);
    }
  }
  // Step 6: wait for confirmation to start mixer
  else if (processStep == 6) {
    lcd.setCursor(0,0); lcd.print("DOSING COMPLETE");
    lcd.setCursor(0,1); lcd.print("#: START MIXER");
    if (key == '#') {
      mixStartTime = millis();
      processStep = 7;
      lcd.clear();
    }
  }
  // Step 7: mixing timer
  else if (processStep == 7) {
    unsigned long elapsed = (millis() - mixStartTime) / 1000;

    if (elapsed < (unsigned long)activeRecipe.duration) {
      digitalWrite(mixerPin, LOW);
      lcd.setCursor(0,0); lcd.print("MIXING...");
      lcd.setCursor(0,1);
      lcd.print("REM TIME: " + String(activeRecipe.duration - elapsed) + "s");
    } else {
      digitalWrite(mixerPin, HIGH);
      lcd.clear();
      lcd.setCursor(0,1); lcd.print("PROCESS DONE!");
      delay(3000);
      currentMode = MAIN_MENU;
      displayMainMenu();
    }
  }

  // Status refresh on LCD during dosing/pumping
  if (millis() - lastLCDUpdate > 350 && processStep >= 1 && processStep <= 5) {
    lcd.setCursor(0,0); lcd.print("STEP: SILO " + String(processStep));
    lcd.setCursor(0,2); lcd.print("WGT: " + String(weight, 1) + "g    ");
    lcd.setCursor(0,3); lcd.print("TGT: " + String(targets[processStep-1], 0) + "g    ");
    lastLCDUpdate = millis();
  }
}

// --------------------User Menu / UI --------------------
void handleMainMenu(char key) {
  if (key == 'A') { menuIndex = (menuIndex + 1) % 3; displayMainMenu(); }
  if (key == 'B') { menuIndex = (menuIndex - 1 + 3) % 3; displayMainMenu(); }

  if (key == '#') {
    if (menuIndex == 0) {
      currentMode = EDIT_VALUES;
      editStep = 0;
      tempValue = 0;
      inputString = "";
      displayValueInput();
    }
    else if (menuIndex == 1) {
      loadRecipe(0);
      currentMode = SUMMARY;
      displaySummary();
    }
    else if (menuIndex == 2) {
      loadRecipe(60);
      currentMode = SUMMARY;
      displaySummary();
    }
  }
}

void handleValueInput(char key) {
  if (key >= '0' && key <= '9') {
    inputString += key;
    tempValue = inputString.toInt();
    displayValueInput();
  }
  else if (key == '*') {
    inputString = "";
    tempValue = 0;
    displayValueInput();
  }
  else if (key == '#') {
    int* refs[] = {&activeRecipe.m1, &activeRecipe.water, &activeRecipe.m2, &activeRecipe.m3, &activeRecipe.m4, &activeRecipe.duration};
    *refs[editStep] = tempValue;

    editStep++;
    tempValue = 0;
    inputString = "";

    if (editStep > 5) {
      currentMode = SUMMARY;
      displaySummary();
    } else {
      displayValueInput();
    }
  }
}

void handleSummary(char key) {
  if (key == 'D') {
    targets[0] = (float)activeRecipe.m1;
    targets[1] = targets[0] + (float)activeRecipe.water;
    targets[2] = targets[1] + (float)activeRecipe.m2;
    targets[3] = targets[2] + (float)activeRecipe.m3;
    targets[4] = targets[3] + (float)activeRecipe.m4;

    currentMode = RUNNING;
    processStep = 0;
    lcd.clear();
  }

  if (key == '1') {
    saveRecipe(0);
    lcd.setCursor(0,3); lcd.print("RECIPE A SAVED");
    delay(800);
    displaySummary();
  }

  if (key == '2') {
    saveRecipe(60);
    lcd.setCursor(0,3); lcd.print("RECIPE B SAVED");
    delay(800);
    displaySummary();
  }
}

void displayMainMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("== MAIN MENU ==");
  lcd.setCursor(0,1); lcd.print(menuIndex == 0 ? "> NEW PROCESS" : "  NEW PROCESS");
  lcd.setCursor(0,2); lcd.print(menuIndex == 1 ? "> LOAD RECIPE A" : "  LOAD RECIPE A");
  lcd.setCursor(0,3); lcd.print(menuIndex == 2 ? "> LOAD RECIPE B" : "  LOAD RECIPE B");
}

void displayValueInput() {
  lcd.clear();
  String labels[] = {"M1 (grams)", "WATER (grams)", "M2 (grams)", "M3 (grams)", "M4 (grams)", "MIX (seconds)"};
  lcd.setCursor(0,0); lcd.print(labels[editStep]);
  lcd.setCursor(0,2); lcd.print("VAL: " + String(tempValue));
  lcd.setCursor(0,3); lcd.print("#:OK  *:CLR");
}

void displaySummary() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("M1:" + String(activeRecipe.m1) + " W:" + String(activeRecipe.water));
  lcd.setCursor(0,1); lcd.print("M2:" + String(activeRecipe.m2) + " M3:" + String(activeRecipe.m3));
  lcd.setCursor(0,2); lcd.print("M4:" + String(activeRecipe.m4) + " T:" + String(activeRecipe.duration));
  lcd.setCursor(0,3); lcd.print("D:START 1:A 2:B");
}

void saveRecipe(int address) {
  EEPROM.put(address, activeRecipe);
}

void loadRecipe(int address) {
  EEPROM.get(address, activeRecipe);
}

void emergencyStop() {
  // Stop outputs (ACTIVE LOW -> HIGH means OFF)
  digitalWrite(pumpPin, HIGH);
  digitalWrite(mixerPin, HIGH);

  // Disable all stepper drivers
  for (int i = 0; i < 4; i++) {
    digitalWrite(enPins[i], HIGH);
  }

  lcd.clear();
  lcd.setCursor(0,1); lcd.print("!!! EMERGENCY !!!");
  delay(2000);

  currentMode = MAIN_MENU;
  displayMainMenu();
}
