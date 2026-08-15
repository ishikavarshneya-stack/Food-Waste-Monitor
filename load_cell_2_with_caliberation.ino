#include <HX711.h>

#define DT_PIN 6
#define SCK_PIN 5

HX711 scale;

// ===== Calibration Values =====
const long ZERO_OFFSET = -436555;
const float CALIBRATION_FACTOR = 427.37;   // counts per gram

void setup() {
  Serial.begin(115200);
  delay(1000);

  scale.begin(DT_PIN, SCK_PIN);

  Serial.println("Load Cell Ready");
}

void loop() {

  long sum = 0;

  // Average 20 readings
  for (int i = 0; i < 20; i++) {
    sum += scale.read();
    delay(10);
  }

  long raw = sum / 20;

  // Convert raw reading to grams
  float weight = (raw - ZERO_OFFSET) / CALIBRATION_FACTOR;

  // Remove small fluctuations around zero
  if (abs(weight) < 2)
    weight = 0;

  Serial.print("Raw: ");
  Serial.print(raw);

  Serial.print("    Weight: ");
  Serial.print(weight, 1);
  Serial.println(" g");

  delay(300);
}