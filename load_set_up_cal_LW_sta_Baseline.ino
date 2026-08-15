#include <HX711.h>
#include <math.h>

// =====================================================
// HX711 OBJECTS
// =====================================================

HX711 lc1;
HX711 lc2;
HX711 lc3;
HX711 lc4;

// =====================================================
// ESP32-S3 PINS
// =====================================================

#define DT1   4
#define SCK1  5

#define DT2   6
#define SCK2  7

#define DT3   15
#define SCK3  16

#define DT4   17
#define SCK4  18

// =====================================================
// PLATFORM CALIBRATION
// =====================================================

const long OFFSET1 = -408892;
const long OFFSET2 = -459848;
const long OFFSET3 = -67721;
const long OFFSET4 = 40680;

const float PLATFORM_FACTOR = 446.60;

// Your last raw event was approximately:
// 331.7 - 34 = 297.7 g for an actual 300 g object.
//
// 300 / 297.7 = 1.0077
const float EVENT_GAIN = 1.0077;
const float EVENT_OFFSET = 0.0;

// =====================================================
// FILTER SETTINGS
// =====================================================

const int NUM_SAMPLES = 25;
const int TRIM_COUNT = 5;

// =====================================================
// STABILITY SETTINGS
// =====================================================

const float STABLE_THRESHOLD = 2.0;
const unsigned long STABLE_TIME = 500;

// =====================================================
// EVENT SETTINGS
// =====================================================

const float EVENT_START_THRESHOLD = 10.0;
const float MINIMUM_EVENT_WEIGHT = 10.0;

const float MOTION_THRESHOLD = 2.0;
const unsigned long EVENT_SETTLE_TIME = 2000;

// Cancel an event if the weight returns near its baseline
const float EVENT_CANCEL_RANGE = 5.0;

// Stable decrease needed to recognize weight removal
const float REMOVAL_THRESHOLD = 10.0;

// How close the platform must be to its original startup
// weight to be considered empty again
const float EMPTY_TOLERANCE = 40.0;

// =====================================================
// STATES
// =====================================================

enum SystemState
{
  FINDING_BASELINE,
  IDLE,
  EVENT_IN_PROGRESS
};

SystemState systemState = FINDING_BASELINE;

// =====================================================
// VARIABLES
// =====================================================

float startupWeight = 0.0;
float baselineWeight = 0.0;
float currentWeight = 0.0;

float stabilityReference = 0.0;
float lastMotionWeight = 0.0;

float accumulatedFoodWeight = 0.0;

unsigned long stabilityStartTime = 0;
unsigned long lastEventMovementTime = 0;

bool stabilityTimerRunning = false;
bool currentReadingStable = false;

unsigned long eventNumber = 0;

// =====================================================
// SORT
// =====================================================

void sortArray(long values[], int count)
{
  for (int i = 0; i < count - 1; i++)
  {
    for (int j = i + 1; j < count; j++)
    {
      if (values[i] > values[j])
      {
        long temp = values[i];
        values[i] = values[j];
        values[j] = temp;
      }
    }
  }
}

// =====================================================
// TRIMMED MEAN
// =====================================================

long trimmedMean(long values[])
{
  sortArray(values, NUM_SAMPLES);

  long long sum = 0;

  for (int i = TRIM_COUNT;
       i < NUM_SAMPLES - TRIM_COUNT;
       i++)
  {
    sum += values[i];
  }

  const int keptSamples =
      NUM_SAMPLES - (2 * TRIM_COUNT);

  return (long)(sum / keptSamples);
}

// =====================================================
// READ ALL FOUR HX711 MODULES
//
// Samples are collected in rounds so the four HX711
// modules are measured at approximately the same time.
// =====================================================

bool readPlatformWeight(float &weight)
{
  long samples1[NUM_SAMPLES];
  long samples2[NUM_SAMPLES];
  long samples3[NUM_SAMPLES];
  long samples4[NUM_SAMPLES];

  for (int i = 0; i < NUM_SAMPLES; i++)
  {
    unsigned long waitStart = millis();

    while (!(lc1.is_ready() &&
             lc2.is_ready() &&
             lc3.is_ready() &&
             lc4.is_ready()))
    {
      if (millis() - waitStart > 2000)
      {
        Serial.println("ERROR: One or more HX711 modules are not ready.");
        return false;
      }

      delay(1);
    }

    samples1[i] = lc1.read();
    samples2[i] = lc2.read();
    samples3[i] = lc3.read();
    samples4[i] = lc4.read();
  }

  long raw1 = trimmedMean(samples1);
  long raw2 = trimmedMean(samples2);
  long raw3 = trimmedMean(samples3);
  long raw4 = trimmedMean(samples4);

  long long totalCounts = 0;

  totalCounts += (long long)raw1 - OFFSET1;
  totalCounts += (long long)raw2 - OFFSET2;
  totalCounts += (long long)raw3 - OFFSET3;
  totalCounts += (long long)raw4 - OFFSET4;

  weight = (float)totalCounts / PLATFORM_FACTOR;

  return true;
}

// =====================================================
// EVENT-WEIGHT CORRECTION
// =====================================================

float correctEventWeight(float measuredFood)
{
  float correctedFood =
      (measuredFood * EVENT_GAIN) + EVENT_OFFSET;

  if (correctedFood < 0)
  {
    correctedFood = 0;
  }

  return correctedFood;
}

// =====================================================
// STABILITY DETECTOR
// =====================================================

bool updateStability(float weight)
{
  float difference =
      fabs(weight - stabilityReference);

  if (difference <= STABLE_THRESHOLD)
  {
    if (!stabilityTimerRunning)
    {
      stabilityStartTime = millis();
      stabilityTimerRunning = true;
    }

    if (millis() - stabilityStartTime >= STABLE_TIME)
    {
      currentReadingStable = true;
    }
  }
  else
  {
    stabilityReference = weight;
    stabilityStartTime = millis();
    stabilityTimerRunning = true;
    currentReadingStable = false;
  }

  return currentReadingStable;
}

// =====================================================
// PRINT EVENT
// =====================================================

void printFoodEvent(float foodAdded)
{
  eventNumber++;
  accumulatedFoodWeight += foodAdded;

  Serial.println();
  Serial.println("====================================");
  Serial.println("       FOOD DISPOSAL DETECTED");
  Serial.println("====================================");

  Serial.print("Event Number       : ");
  Serial.println(eventNumber);

  Serial.print("Food Added         : ");
  Serial.print(foodAdded, 1);
  Serial.println(" g");

  Serial.print("Session Waste Total: ");
  Serial.print(accumulatedFoodWeight, 1);
  Serial.println(" g");

  Serial.println("====================================");
  Serial.println();
}

// =====================================================
// RESET SESSION
// =====================================================

void resetSession(float newBaseline)
{
  baselineWeight = newBaseline;
  accumulatedFoodWeight = 0.0;
  eventNumber = 0;

  lastMotionWeight = newBaseline;

  Serial.println();
  Serial.println("====================================");
  Serial.println("Platform returned to empty state.");
  Serial.println("Session waste total reset to 0 g.");
  Serial.println("Event counter reset.");
  Serial.println("====================================");
  Serial.println();
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  lc1.begin(DT1, SCK1);
  lc2.begin(DT2, SCK2);
  lc3.begin(DT3, SCK3);
  lc4.begin(DT4, SCK4);

  Serial.println();
  Serial.println("====================================");
  Serial.println(" FOOD WASTE MONITOR V2.3.1");
  Serial.println(" Corrected Event and Running Total");
  Serial.println("====================================");
  Serial.println();
  Serial.println("Keep the platform untouched.");
  Serial.println("Finding initial baseline...");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
  if (!readPlatformWeight(currentWeight))
  {
    delay(1000);
    return;
  }

  bool stable = updateStability(currentWeight);

  // ---------------------------------------------------
  // FIND INITIAL EMPTY/BIN BASELINE
  // ---------------------------------------------------

  if (systemState == FINDING_BASELINE)
  {
    if (stable)
    {
      startupWeight = currentWeight;
      baselineWeight = currentWeight;
      lastMotionWeight = currentWeight;

      accumulatedFoodWeight = 0.0;
      eventNumber = 0;

      systemState = IDLE;

      Serial.println();
      Serial.println("Initial baseline established.");
      Serial.println("Session Waste Total: 0.0 g");
      Serial.println("System ready.");
      Serial.println();
    }

    delay(20);
    return;
  }

  // ---------------------------------------------------
  // IDLE
  // ---------------------------------------------------

  if (systemState == IDLE)
  {
    float changeFromBaseline =
        currentWeight - baselineWeight;

    // Positive change: food is being added
    if (changeFromBaseline >= EVENT_START_THRESHOLD)
    {
      systemState = EVENT_IN_PROGRESS;

      lastMotionWeight = currentWeight;
      lastEventMovementTime = millis();

      Serial.println("Food detected...");
      Serial.println("Waiting for the weight to settle.");
    }

    // Negative change: something has been removed
    else if (stable &&
             changeFromBaseline <= -REMOVAL_THRESHOLD)
    {
      /*
         If the platform is back near its original startup
         weight, assume the test object/bin contents were
         removed and start a new session.
      */

      if (fabs(currentWeight - startupWeight)
          <= EMPTY_TOLERANCE)
      {
        resetSession(currentWeight);

        startupWeight = currentWeight;
      }
      else
      {
        /*
           A partial removal happened, but the bin is not
           empty. Update only the physical baseline.
           Do not count it as food added.
        */

        baselineWeight = currentWeight;
        lastMotionWeight = currentWeight;

        Serial.println();
        Serial.println("Weight removal detected.");
        Serial.println("Physical baseline updated.");
        Serial.println("Session waste total unchanged.");
        Serial.println();
      }
    }
  }

  // ---------------------------------------------------
  // EVENT IN PROGRESS
  // ---------------------------------------------------

  else if (systemState == EVENT_IN_PROGRESS)
  {
    float movement =
        fabs(currentWeight - lastMotionWeight);

    if (movement >= MOTION_THRESHOLD)
    {
      lastMotionWeight = currentWeight;
      lastEventMovementTime = millis();
    }

    float measuredIncrease =
        currentWeight - baselineWeight;

    // Object was removed before the event finished
    if (stable &&
        measuredIncrease <= EVENT_CANCEL_RANGE)
    {
      Serial.println();
      Serial.println("Event cancelled.");
      Serial.println("Weight returned to baseline.");
      Serial.println();

      systemState = IDLE;
      lastMotionWeight = currentWeight;
    }

    // Weight has stopped changing
    else if (stable &&
             millis() - lastEventMovementTime
             >= EVENT_SETTLE_TIME)
    {
      float correctedFoodAdded =
          correctEventWeight(measuredIncrease);

      if (correctedFoodAdded >= MINIMUM_EVENT_WEIGHT)
      {
        printFoodEvent(correctedFoodAdded);
      }

      // New physical sensor baseline
      baselineWeight = currentWeight;
      lastMotionWeight = currentWeight;

      systemState = IDLE;

      Serial.println("System ready for the next disposal.");
      Serial.println();
    }
  }

  delay(20);
}