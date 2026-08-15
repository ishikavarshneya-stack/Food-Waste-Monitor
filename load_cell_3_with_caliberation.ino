#include <HX711.h>

#define DT_PIN 6
#define SCK_PIN 5

HX711 scale;

// ===== Load Cell 3 Calibration =====
const long ZERO_OFFSET = 3231;
const float COUNTS_PER_GRAM = 437.6;

void setup() {
  Serial.begin(115200);
  delay(1000);

  scale.begin(DT_PIN, SCK_PIN);

  Serial.println("--------------------------------");
  Serial.println("Load Cell 3 - Weight in Grams");
  Serial.println("--------------------------------");
}

void loop() {

  // Average 20 readings
  long sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += scale.read();
    delay(10);
  }

  long raw = sum / 20;

  // Convert to grams
  float weight = (raw - ZERO_OFFSET) / COUNTS_PER_GRAM;

  // Remove tiny fluctuations around zero
  if (weight > -2 && weight < 2)
    weight = 0;

  Serial.print("Raw: ");
  Serial.print(raw);

  Serial.print(" | Weight: ");
  Serial.print(weight, 1);
  Serial.println(" g");

  delay(300);
}