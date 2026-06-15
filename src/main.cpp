#include <Arduino.h>


const int SENSOR_PIN = D2;
const int LED_PIN = D7;

bool lastDoorOpen = true;
int ledState = LOW;

// Variables for non-blocking blinking
unsigned long lastBlinkTime = 0;
const long blinkInterval = 500; // Blink rate in milliseconds (500ms ON, 500ms OFF)

// Returns true if door is open
bool readDoorOpen() {
  return digitalRead(SENSOR_PIN) == HIGH;
}

void printDoorState(bool open) {
  if (open) {
    Serial.println("DOOR: OPEN");
  } else {
    Serial.println("DOOR: CLOSED");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  // Read initial state on boot
  bool currentDoorOpen = readDoorOpen();
  lastDoorOpen = currentDoorOpen;

  Serial.println("Boot");
  printDoorState(currentDoorOpen);
  
  // Set initial LED state (OFF if closed)
  if (!currentDoorOpen) {
    digitalWrite(LED_PIN, LOW);
  }
}

void loop() {
  unsigned long currentMillis = millis();
  bool currentDoorOpen = readDoorOpen();

  // 1. Check for Door State Changes (Debounced)
  if (currentDoorOpen != lastDoorOpen) {
    delay(20); // Debounce delay
    currentDoorOpen = readDoorOpen(); // Re-read to confirm

    if (currentDoorOpen != lastDoorOpen) {
      lastDoorOpen = currentDoorOpen;
      printDoorState(currentDoorOpen);
      
      // If the door just closed, force the LED OFF immediately
      if (!currentDoorOpen) {
        digitalWrite(LED_PIN, LOW);
        ledState = LOW;
      }
    }
  }

  // 2. Handle LED Blinking (Only runs if the door is open)
  if (currentDoorOpen) {
    if (currentMillis - lastBlinkTime >= blinkInterval) {
      lastBlinkTime = currentMillis; // Save the last time the LED blinked

      // Toggle the LED state
      ledState = (ledState == LOW) ? HIGH : LOW;
      digitalWrite(LED_PIN, ledState);
    }
  }

  delay(10); // Small loop delay for stability
}
