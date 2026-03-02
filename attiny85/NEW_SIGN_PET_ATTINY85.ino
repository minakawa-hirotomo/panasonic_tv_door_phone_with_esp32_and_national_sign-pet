
// ATtiny85 用スケッチ（ドアホン A 接点 & ドアオープン検出 & リレー制御）
// GPIO LOWでリレーON（シンク駆動版）

#include <Arduino.h>
#include <avr/wdt.h>

// Digispark/ATtiny85 基準：D0=PB0, D1=PB1(LED), D2=PB2, D3=PB3, D4=PB4
#define relayPin    0   // リレー出力（ATQ209コイルをシンク駆動）
#define LED_PIN     1   // 内蔵LEDピン（一般に Digispark は 1）
#define A_SW        2   // ドアホンA接点（短絡でON：GNDへ落ちる）
#define DOOR_SW     3   // ドアオープン検出マイクロスイッチ（短絡でON：GNDへ落ちる）

// 極性（実機に合わせて切り替え）
#define LED_ACTIVE_LOW      false  // true: LOWで点灯, false: HIGHで点灯
#define RELAY_ACTIVE_HIGH   false  // ★GPIO LOWでリレーON（シンク駆動）

// ==== 動作定数 ====
#define PUSH_SHORT           50    // チャタリング防止用時間(ms)：50〜100推奨
#define RESTART_INTERVAL (24UL * 60UL * 60UL * 1000UL)  // 24時間ごとに再起動

// 動作時間定数
#define RELAY_ON_TIME_CALL    500   // 呼出時ON時間(0.5秒)
#define RELAY_OFF_TIME_CALL  1000   // 呼出時OFF時間(1秒)
#define RELAY_REPEAT_CALL       2   // 呼出繰り返し回数（2回）
#define RELAY_ON_TIME_DOOR   1000   // ドア開時ON時間(1秒)
#define BLOCK_TIME           3000   // 動作後ブロック時間(3秒)

// ==== 再起動管理 ====
unsigned long lastRestart = 0;

// ==== デバウンス用構造体（ピンごとに保持） ====
struct Debounce {
  uint8_t pin;
  int lastStable;           // 最後に確定した安定状態 (HIGH/LOW)
  unsigned long lastChange; // millis() を記録
  bool reported;            // その LOW 確定を報告済みか (立下がりを1回だけ返すため)
};

// 初期状態はプルアップ入力なので HIGH を安定状態として開始
Debounce dbA    = { A_SW,    HIGH, 0UL, false };
Debounce dbDoor = { DOOR_SW, HIGH, 0UL, false };

// ==== ユーティリティ ====
inline void setLed(bool on) {
  if (LED_ACTIVE_LOW) {
    digitalWrite(LED_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, on ? HIGH : LOW);
  }
}

inline void setRelay(bool on) {
  if (RELAY_ACTIVE_HIGH) {
    digitalWrite(relayPin, on ? HIGH : LOW);  // HIGHでONの場合
  } else {
    digitalWrite(relayPin, on ? LOW : HIGH);  // ★LOWでON（シンク駆動）
  }
}

void forceRestart() {
  // ウォッチドッグで強制リセット
  wdt_enable(WDTO_15MS);
  while (true) { /* WDT リセット待ち */ }
}

// ==== リレー制御関数 ====

// リレーをON/OFFする基本動作
void relayOn(int onTimeMs) {
  setRelay(true);   // リレーON（GPIO LOWでONに設定済み）
  setLed(true);
  delay(onTimeMs);
  setRelay(false);  // リレーOFF
  setLed(false);
}

// ドアホン呼出（2回繰り返し）
void interphoneCall() {
  for (int i = 0; i < RELAY_REPEAT_CALL; i++) {
    relayOn(RELAY_ON_TIME_CALL);
    delay(RELAY_OFF_TIME_CALL);
  }
  delay(BLOCK_TIME);
}

// ドアオープン（1回だけ）
void doorOpen() {
  relayOn(RELAY_ON_TIME_DOOR);
  delay(BLOCK_TIME);
}

// ==== デバウンス検出関数（立下がりを1回だけ true で返す） ====
// 使い方: if (debounceFalling(&dbA)) { ... }
bool debounceFalling(Debounce *db) {
  int reading = digitalRead(db->pin);

  if (reading != db->lastStable) {
    // 状態が変化した（暫定） -> タイマーをリセット
    db->lastChange = millis();
    db->lastStable = reading; // 直近の観測レベルを更新
    db->reported = false;     // 未報告に戻す
    return false;
  } else {
    // 状態が変わらない -> 経過時間が閾値を超えたら確定
    if ((millis() - db->lastChange) > PUSH_SHORT) {
      // LOW が確定していて、まだ報告していなければ立下がりを返す
      if (reading == LOW && db->reported == false) {
        db->reported = true;  // 1回だけ報告
        return true;
      }
    }
  }
  return false;
}

// ==== セットアップ ====
void setup() {
  lastRestart = millis();

  // デバウンスの初期タイムスタンプ（起動直後の誤検知防止）
  dbA.lastChange    = lastRestart;
  dbDoor.lastChange = lastRestart;

  pinMode(A_SW, INPUT_PULLUP);    // A接点短絡でLOWになる
  pinMode(DOOR_SW, INPUT_PULLUP); // ドアスイッチ短絡でLOWになる
  pinMode(relayPin, OUTPUT);      // リレー出力
  pinMode(LED_PIN, OUTPUT);       // LED

  setRelay(false);                // リレー初期状態OFF
  setLed(false);                  // LED初期状態OFF
}

// ==== ループ ====
void loop() {
  // ハングアップ対策で24時間ごとに再起動
  if (millis() - lastRestart >= RESTART_INTERVAL) {
    forceRestart();
  }

  // ドアホン呼出 (立下がり検出)
  if (debounceFalling(&dbA)) {
    interphoneCall();
  }

  // ドアオープン (立下がり検出)
  if (debounceFalling(&dbDoor)) {
       doorOpen();
  }
}
