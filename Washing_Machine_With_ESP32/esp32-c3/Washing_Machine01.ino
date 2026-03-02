#include <pitches.h>
#include <Arduino.h>

#define SPEAKER 10
#define SWITCH_PIN 2
#define CURRENT_PIN 3
#define LED_BUILTIN 8

#define RESTART_INTERVAL (24UL*60UL*60UL*1000UL)
#define CALIBRATION_TIME 10000      // 10秒キャリブレーション
#define WAIT_TIME 240000            // 4分
#define MEASURE_INTERVAL_MS 20000   // 20秒
#define CURRENT_THRESHOLD 0.1

const float ADC_COUNTS_PER_V = 4095.0f / 3.3f;
const float VRMS_1V_COUNTS = ADC_COUNTS_PER_V * 1.0f;
const float COUNTS_PER_A_RMS = VRMS_1V_COUNTS / 5.0f;
const float CAL_FACTOR = 0.7;
const float DEAD_BAND = 0.05;

RTC_DATA_ATTR int bootCount = 0;
unsigned long lastRestart = 0;

bool buzzerOn = false;
bool lastSwitchState = HIGH;
bool alarmTriggered = false;
bool rawHist[4] = {false,false,false,false};//4回連続判定
bool lastCurrentState = false;

static const int tempo = 60;
static const float wholeNoteDuration = 60000.0 / tempo * 4;

int melody[] = {
  NOTE_G4,NOTE_C5,NOTE_C5,NOTE_D5,
  NOTE_E5,NOTE_G5,NOTE_G5,
  NOTE_A5,NOTE_F5,NOTE_C6,NOTE_A5,
  NOTE_G5,NOTE_A5,NOTE_G5,NOTE_G5
};

int notedurations[] = {
  8,8,8,8,
  8,8,4,
  8,8,8,8,
  8,8,8,8
};

int currentNote = 0;
unsigned long lastNoteTime = 0;
long midpoint = 0;
unsigned long lastMeasureTime = 0;
bool forceMeasure = true;
bool prevBuzzerOn = false;
unsigned long offStartTime = 0;

// ----- キャリブレーション安全版 -----
long calibrateMidpoint(int samples = 2000) {
  uint64_t sum = 0;
  uint64_t sumsq = 0;
  int minVal = 4095, maxVal = 0;
  unsigned long start = millis();
  int count = 0;

  while(millis() - start < CALIBRATION_TIME && count < samples) {
    int v = analogRead(CURRENT_PIN);
    sum += (uint64_t)v;
    sumsq += (uint64_t)v * (uint64_t)v;
    if(v < minVal) minVal = v;
    if(v > maxVal) maxVal = v;
    count++;
    delayMicroseconds(100);
  }

  double avg = (double)sum / count;
  double variance = ((double)sumsq / count) - (avg*avg);
  double stddev = sqrt(fabs(variance));

  Serial.println("=== Calibration Report ===");
  Serial.print("Samples: "); Serial.println(count);
  Serial.print("Average midpoint: "); Serial.println(avg,3);
  Serial.print("StdDev: "); Serial.println(stddev,3);
  Serial.print("Min: "); Serial.print(minVal);
  Serial.print("  Max: "); Serial.println(maxVal);
  Serial.println("==========================");

  return (long)(avg + 0.5);
}

// ----- 平均値簡易版 -----
long sampleAverageQuick(int samples=500,int pauseUs=200) {
  unsigned long sum = 0;
  for(int i=0;i<samples;i++){
    int v = analogRead(CURRENT_PIN);
    sum += (unsigned long)v;
    if(pauseUs) delayMicroseconds(pauseUs);
  }
  return (long)(sum / samples);
}

// ----- RMS計算 -----
float readCurrentRMS_counts(int samples=500,int pauseUs=200) {
  unsigned long long sumsq = 0ULL;
  for(int i=0;i<samples;i++){
    long v = analogRead(CURRENT_PIN);
    long diff = v - midpoint;
    sumsq += (unsigned long long)(diff*diff);
    if(pauseUs) delayMicroseconds(pauseUs);
  }
  float meanSq = (float)sumsq / (float)samples;
  return sqrtf(meanSq);
}

// ----- 4回連続判定 -----
bool decideCurrentState(bool raw, bool lastStable){
  rawHist[3]=rawHist[2];
  rawHist[2]=rawHist[1];
  rawHist[1]=rawHist[0];
  rawHist[0]=raw;

  if(rawHist[0] && rawHist[1] && rawHist[2] && rawHist[3]) return true;
  if(!rawHist[0] && !rawHist[1] && !rawHist[2] && rawHist[3]) return false;
  return lastStable;
}

// ----- メロディ再生 -----
void playMelody(unsigned long now){
  if(currentNote >= (int)(sizeof(melody)/sizeof(melody[0]))) return;
  unsigned long noteDur = (unsigned long)(wholeNoteDuration / notedurations[currentNote]);
  if(now - lastNoteTime >= noteDur){
    lastNoteTime = now;
    if(melody[currentNote]>0){
      ledcWriteTone(SPEAKER, melody[currentNote]);
      digitalWrite(LED_BUILTIN, LOW);
    } else {
      ledcWriteTone(SPEAKER,0);
      digitalWrite(LED_BUILTIN,HIGH);
    }
    currentNote++;
    if(currentNote >= (int)(sizeof(melody)/sizeof(melody[0]))){
      buzzerOn=false;
      ledcWriteTone(SPEAKER,0);
      digitalWrite(LED_BUILTIN,HIGH);
      Serial.println("Buzzer FINISHED");
      forceMeasure=true;
    }
  }
}

void setup(){
  pinMode(SWITCH_PIN,INPUT_PULLUP);
  pinMode(CURRENT_PIN,INPUT);
  ledcAttach(SPEAKER,5000,10);
  pinMode(LED_BUILTIN,OUTPUT);
  digitalWrite(LED_BUILTIN,HIGH);
  Serial.begin(115200);

  Serial.println("Calibration start...");
  midpoint = calibrateMidpoint();
  lastMeasureTime=0;
  forceMeasure=true;
}

void loop(){
  unsigned long now=millis();
  if(now-lastRestart >= RESTART_INTERVAL) ESP.restart();

  // スイッチ処理
  bool switchState = digitalRead(SWITCH_PIN);
  if(lastSwitchState==HIGH && switchState==LOW){
    if(buzzerOn){
      buzzerOn=false;
      ledcWriteTone(SPEAKER,0);
      digitalWrite(LED_BUILTIN,HIGH);
      Serial.println("Buzzer STOPPED (switch)");
      forceMeasure=true;
    } else {
      buzzerOn=true;
      currentNote=0;
      lastNoteTime=now;
      Serial.println("Buzzer START (switch)");
    }
    delay(50);
  }
  lastSwitchState = switchState;

  if(buzzerOn){
    playMelody(now);
    prevBuzzerOn=true;
    return;
  }
  if(prevBuzzerOn && !buzzerOn) forceMeasure=true;
  prevBuzzerOn=buzzerOn;

  // 測定処理
  if(forceMeasure || (now-lastMeasureTime>=MEASURE_INTERVAL_MS)){
    lastMeasureTime=now;
    forceMeasure=false;

    long rawAvg = sampleAverageQuick(200,100);
    float rms_counts = readCurrentRMS_counts(500,200);
    float currentA = (rms_counts / COUNTS_PER_A_RMS) * CAL_FACTOR;

    if(currentA < DEAD_BAND) currentA = 0.0; // DEAD_BAND未満の電流は0扱いとする

    bool rawCurrentNow = (currentA > CURRENT_THRESHOLD);  // しきい値超えたらON
    bool currentNow = decideCurrentState(rawCurrentNow,lastCurrentState);

    Serial.print("rawAvg="); Serial.print(rawAvg);
    Serial.print(" I_rms(A)="); Serial.print(currentA,3);
    Serial.print(" currentNow="); Serial.println(currentNow?"ON":"OFF");

    if(lastCurrentState && !currentNow){
      offStartTime=now;
      Serial.println("Detected OFF, timer started");
    }

    if(!currentNow && offStartTime>0 && (now-offStartTime>=WAIT_TIME) && !alarmTriggered){
      buzzerOn=true;
      currentNote=0;
      lastNoteTime=now;
      alarmTriggered=true;
      Serial.println("Buzzer START (auto after stop)");
    }

    if(currentNow){
      offStartTime=0;
      alarmTriggered=false;
    }

    lastCurrentState=currentNow;
  }

  ledcWriteTone(SPEAKER,0);
  digitalWrite(LED_BUILTIN,HIGH);
}
