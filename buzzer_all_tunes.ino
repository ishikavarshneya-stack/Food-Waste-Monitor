/*
  ESP32-S3 Passive Buzzer
  Success -> Middle -> Disappointing
*/

#define BUZZER_PIN 21

void setup()
{
  ledcAttach(BUZZER_PIN, 2000, 8);
}

void play(int freq, int duration)
{
  ledcWriteTone(BUZZER_PIN, freq);
  delay(duration);

  ledcWriteTone(BUZZER_PIN, 0);
  delay(25);
}

//======================================
// SUCCESS
//======================================

void successTune()
{
  // Gentle opening
  play(523, 120);    // C5
  play(659, 140);    // E5
  play(784, 170);    // G5

  // Build excitement
  play(1175, 130);   // D6
  play(1319, 160);   // E6

  // Bright finish
  play(1760, 900);   // A6
}

//======================================
// MIDDLE (Neutral)
//======================================

void middleTune()
{
  play(1319, 120);   // E6
  play(1175, 140);   // D6
  play(1047, 160);   // C6
  play(988, 180);    // B5
  play(1047, 500);   // C6
}

//======================================
// DISAPPOINTING
//======================================

void disappointingTune()
{
  play(1760, 90);    // A6
  play(1568, 100);   // G6
  play(1319, 120);   // E6
  play(1175, 140);   // D6
  play(988, 180);    // B5
  play(784, 850);    // G5
}

//======================================

void loop()
{
  Serial.println("SUCCESS");
  successTune();

  delay(3000);

  Serial.println("MIDDLE");
  middleTune();

  delay(3000);

  Serial.println("DISAPPOINTING");
  disappointingTune();

  delay(5000);
}