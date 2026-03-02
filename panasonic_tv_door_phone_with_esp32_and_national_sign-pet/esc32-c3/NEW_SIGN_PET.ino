#include <WiFi.h>              //Wi-Fi
#include <WiFiClientSecure.h>  //Wi-Fi
#include <ArduinoJson.h>

#define A_SW 4            //C3 ドアホンA接点
#define DOOR_SW 3         //C3 ドアオープン検出マイクロスイッチ
#define relayPin 10       //C3 ATQ209
#define LED_BUILTIN 8     //C3 内蔵LED

//#define A_SW 25             //ドアホンA接点
//#define DOOR_SW 35          //ドアオープン検出マイクロスイッチ
//#define relayPin 26         //ATQ209
//#define LED_BUILTIN 2       //内蔵LED

#define JST 3600 * 9
#define PUSH_SHORT 50     // チャタリング防止用時間(ms). 50〜100 を推奨
#define RESTART_INTERVAL (24UL * 60UL * 60UL * 1000UL)  // 24時間

// 動作時間定数
#define RELAY_ON_TIME_CALL   500   // 呼出時ON時間
#define RELAY_OFF_TIME_CALL 1000   // 呼出時OFF時間
#define RELAY_REPEAT_CALL      3   // 呼出繰り返し回数
#define RELAY_ON_TIME_DOOR  1500   // ドア開時ON時間
#define BLOCK_TIME          3000   // 動作後ブロック時間

// --- Slack Webhook 設定 ---
const char* slackHost = "hooks.slack.com";
const char* slackPath = "/services/T5AKCRBM4/B09A9V0UHBL/kVXG92xfO8kXJCslkxcqs1Lf";

// WiFi設定
const char* ssid = "mochan-s2";
const char* password = "minakawa";
String hostname = "INTPHONE";
//IPAddress ip(192, 168, 11, 240);     //ESP32のIPアドレスを固定する場合のアドレス
//IPAddress subnet(255, 255, 255, 0);  //ESP32のIPアドレス固定する場合のサブネット
//IPAddress gateway(192, 168, 11, 1);  //ルーターのゲートウェイを入れる
//IPAddress DNS(192, 168, 11, 1);
bool done = true;

RTC_DATA_ATTR int bootCount = 0;
unsigned long lastRestart = 0;

//----------------------------------
// デバウンス用構造体（ピンごとに保持）
//----------------------------------
struct Debounce {
  uint8_t pin;
  int lastStable;           // 最後に確定した安定状態 (HIGH/LOW)
  unsigned long lastChange; // millis() を記録
  bool reported;            // その LOW 確定を報告済みか (立下がりを一度だけ返すため)
};

Debounce dbA = { A_SW, HIGH, 0UL, false };
Debounce dbDoor = { DOOR_SW, HIGH, 0UL, false };

//----------------------------------
// リレー制御関数
//----------------------------------

// リレーをON/OFFする基本動作
void relayOn(int onTime) {
  digitalWrite(relayPin, HIGH);
  digitalWrite(LED_BUILTIN, LOW);//C3
  //digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("リレーON");
  delay(onTime);
  digitalWrite(relayPin, LOW);
  digitalWrite(LED_BUILTIN, HIGH);//C3
  //digitalWrite(LED_BUILTIN, LOW);
  Serial.println("リレーOFF");
}

// ドアホン用（3回繰り返し）
void interphoneCall() {
  Serial.println("ドアホン呼出検知");
  for (int i = 0; i < RELAY_REPEAT_CALL; i++) {
    relayOn(RELAY_ON_TIME_CALL);
    delay(RELAY_OFF_TIME_CALL);
  }
  delay(BLOCK_TIME);
}

// ドアオープン用（1回だけ）
void doorOpen() {
  Serial.println("ドアオープン検知");
  relayOn(RELAY_ON_TIME_DOOR);
  delay(BLOCK_TIME);
}

//----------------------------------
// デバウンス検出関数（立下がりを1回だけtrueで返す）
// 使い方: if (debounceFalling(&dbA)) { ... }
//----------------------------------
bool debounceFalling(Debounce *db) {
  int reading = digitalRead(db->pin);

  if (reading != db->lastStable) {
    // 状態が変化した（暫定） -> タイマーをリセット
    db->lastChange = millis();
    db->lastStable = reading; // update the last observed level for timing
    db->reported = false;     // 未報告に戻す
    return false;
  } else {
    // 状態が変わらない -> 経過時間が閾値を超えたら確定
    if ((millis() - db->lastChange) > PUSH_SHORT) {
      // LOWが確定していて、まだ報告していなければ立下がりを返す
      if (reading == LOW && db->reported == false) {
        db->reported = true;  // 1回だけ報告する
        return true;
      }
    }
  }
  return false;
}

//----------------------------------
void setup() {
  Serial.begin(115200);
  lastRestart = millis();

  pinMode(A_SW, INPUT_PULLUP);    //ドアホンA接点短絡でON
  pinMode(DOOR_SW, INPUT_PULLUP); //ドアオープン短絡でON
  pinMode(relayPin, OUTPUT);      //リレー出力
  pinMode(LED_BUILTIN, OUTPUT);   //LED

  digitalWrite(relayPin, LOW);    //リレー初期状態OFF
  digitalWrite(LED_BUILTIN, HIGH);//LED初期状態OFF（C3SuperMiniは極性反転）
  //digitalWrite(LED_BUILTIN, LOW);//LED初期状態OFF

  // WiFi接続
WiFi.disconnect(true, true);
WiFi.mode(WIFI_STA);
WiFi.begin(ssid, password);

while (done) {
  Serial.print("Wi-Fi connecting");
  unsigned long start = millis();

  // 15秒待って接続確認
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    done = false;
    Serial.println(" Connected!");
  } else {
    Serial.println(" Retry...");

    // C3向け安全処理
    WiFi.disconnect(true, true);
    delay(5000);              // 完全に切断されるまで待つ
    WiFi.begin(ssid, password);
    delay(15000);             // 接続試行に十分時間を与える
  }
}


  Serial.println("Wi-Fi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  configTime(JST, 0, "pool.ntp.org", "jp.pool.ntp.org");

  delay(5000);
  sendSlackMessage("🤖 起動OK!");
  Serial.println("起動OK!");
}

// Slack にメッセージを送信する関数
void sendSlackMessage(const char* message) {
  WiFiClientSecure client;
  client.setInsecure(); // 簡易的に証明書検証を無効化

  if (!client.connect(slackHost, 443)) {
    Serial.println("Slack connection failed");
    return;
  }

  String payload = String("{\"text\":\"") + message + "\"}";

  String request = String("POST ") + slackPath + " HTTP/1.1\r\n" +
                   "Host: " + slackHost + "\r\n" +
                   "Content-Type: application/json\r\n" +
                   "Content-Length: " + payload.length() + "\r\n\r\n" +
                   payload;

  client.print(request);

  // レスポンスを読み出して確認
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break; // ヘッダ終了
    Serial.println(line);    // デバッグ出力
  }
  String response = client.readString();
  Serial.println("Response: " + response);
}

//----------------------------------
void loop() {

  ///////////////////////
  time_t t;
  struct tm* tm;
  static const char* wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thr", "Fri", "Sat" };
  t = time(NULL);
  tm = localtime(&t);
  
  char timeStr[16];
  sprintf(timeStr,"%02d:%02d:%02d",tm->tm_hour, tm->tm_min, tm->tm_sec);

  /*
  Serial.printf("%04d/%02d/%02d(%s) %02d:%02d:%02d\n",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                wd[tm->tm_wday],
                tm->tm_hour, tm->tm_min, tm->tm_sec);
  */

  // 毎日06:05にRestart(ハングアップ対策)
  if ((tm->tm_hour == 06) && (tm->tm_min == 05) && (tm->tm_sec == 00)) {
    ESP.restart();
  }

//  // ハングアップ対策で24時間ごとに再起動
//  if (millis() - lastRestart >= RESTART_INTERVAL) {
//    ESP.restart();
//  }

  // ドアホン呼出 (立下がり検出)
  if (debounceFalling(&dbA)) {
    interphoneCall();
    sendSlackMessage(String("🚪 おや?誰か来たようだ! (" + String(timeStr) + ")").c_str());
  }

  // ドアオープン (立下がり検出)
  if (debounceFalling(&dbDoor)) {
    doorOpen();
    sendSlackMessage(String("🔑 ドアが開きました! (" + String(timeStr) + ")").c_str());
  }
}
