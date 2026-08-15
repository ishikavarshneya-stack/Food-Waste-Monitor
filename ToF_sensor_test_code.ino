#include <Wire.h>
#include <Adafruit_VL53L0X.h>

Adafruit_VL53L0X lox;

void setup() {

  Serial.begin(115200);

  Wire.begin(8, 9);      // Change if required

  Serial.println("Initializing...");

  if (!lox.begin())
  {
    Serial.println("TOF unavailable");
    while(1);
  }

  Serial.println("TOF detected!");
}

void loop()
{
  VL53L0X_RangingMeasurementData_t measure;

  lox.rangingTest(&measure, false);

  if(measure.RangeStatus == 4)
  {
    Serial.println("Out of range");
  }
  else
  {
    Serial.print("Distance: ");
    Serial.print(measure.RangeMilliMeter);
    Serial.println(" mm");
  }

  delay(100);
}