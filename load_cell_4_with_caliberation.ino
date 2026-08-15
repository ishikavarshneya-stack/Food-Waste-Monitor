#include <HX711.h>

#define DT_PIN 6
#define SCK_PIN 5

HX711 scale;

// ===== Load Cell 4 Calibration =====
const long ZERO_OFFSET = -21421;
const float COUNTS_PER_GRAM = 409.42;

void setup() {
  Serial.begin(115200);
  delay(1000);

  scale.begin(DT_PIN, SCK_PIN);

  Serial.println("Load Cell 4 Ready");
}

void loop() {

  long sum = 0;

  // Average 20 readings
  for (int i = 0; i < 20; i++) {
    sum += scale.read();
    delay(10);
  }

  long raw = sum / 20;

  float weight = (raw - ZERO_OFFSET) / COUNTS_PER_GRAM;

  // Ignore tiny fluctuations
  if (abs(weight) < 2)
    weight = 0;

  Serial.print("Weight: ");
  Serial.print(weight, 1);
  Serial.println(" g");

  delay(300);
}