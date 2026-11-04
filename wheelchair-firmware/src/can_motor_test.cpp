#include <Arduino.h>
#include <ezButton.h>

const int BUTTON_PIN = 4;
ezButton button(BUTTON_PIN);

void setup() {
    Serial.begin(115200);
    button.setDebounceTime(50);
    delay(500);
    Serial.println("Button isolation test started");
}

void loop() {
    button.loop();

    if (button.isPressed()) {
        Serial.println("PRESSED");
    }

    if (button.isReleased()) {
        Serial.println("RELEASED");
    }

    static uint32_t lastPrint = 0;

    if (millis() - lastPrint >= 200) {
        lastPrint = millis();

        Serial.printf("RAW=%d | STABLE=%d\n",
                      digitalRead(BUTTON_PIN),
                      button.getState());
    }
}