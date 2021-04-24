/*
 * BME280で5分毎に温度、湿度、気圧を測定し、Ambientに送信する
 * 測定と測定の間はDeep Sleepで待つ
 */

#include <ESP8266WiFi.h>

#include "Ambient.h"

// BME280
// 参考ページ：https://www.mgo-tec.com/blog-entry-esp32-bme280-sensor-library.html
// 参考ページ：https://qiita.com/yukitter/items/de45b8280db0f3435b4d
// ライブラリ: https://github.com/finitespace/BME280
#include <BME280I2C.h>

// I2C通信用
#include <Wire.h>

// INA266用 追加
// 参考 : https://asukiaaa.blogspot.com/2019/10/ina226arduino.html
#include <INA226_asukiaaa.h>

// もし、I2Cのポートを変更する場合は、wire2関数あたりで設定する必要あり
// I2C用のポート
// const uint8_t I2C_SDA = 25;
// const uint8_t I2C_SCL = 26;

#define SERIAL_BAUD 115200     // Serial 通信レート
#define BME280_ADDRESS 0x76    // BME280 スレーブアドレス
#define INA226_1_ADDRESS 0x40  // ソーラー側INA226 スレーブアドレス
#define INA226_2_ADDRESS 0x41  // バッテリー側INA226 スレーブアドレス

// ambient送信間隔
// 1時間
#define SEND_DATA_AMBIENT

温度、湿度、気圧
    電流、電圧、電力

    1秒間隔 3回

    // シャント抵抗の値を引数にすると、キャリブレーションの値を出力する関数
    // 2つのINA226でそれぞれシャント抵抗が違う場合は、それぞれの抵抗値で計算した値が必要
    const uint16_t ina226calib = INA226_asukiaaa::calcCalibByResistorMilliOhm(2);  // Max 5120 milli ohm

// 1つ目のINA226のスレーブアドレス
INA226_asukiaaa voltCurrMeter_1(INA226_1_ADDRESS, ina226calib);  // 1つ目のINA226

// 2つ目のセンサ
// 〇〇〇の部分に、設定したスレーブアドレスと同じ定数（INA226_asukiaaa.hのdefineで設定されている）を指定する
// ヘッダファイル INA226_asukiaaa.h の定義部分
// これらから選ぶ
/*
#define INA226_ASUKIAAA_ADDR_A0_GND_A1_GND B1000000
#define INA226_ASUKIAAA_ADDR_A0_VDD_A1_GND B1000001
#define INA226_ASUKIAAA_ADDR_A0_SDA_A1_GND B1000010
#define INA226_ASUKIAAA_ADDR_A0_SCL_A1_GND B1000011
#define INA226_ASUKIAAA_ADDR_A0_GND_A1_VDD B1000100
#define INA226_ASUKIAAA_ADDR_A0_VDD_A1_VDD B1000101
#define INA226_ASUKIAAA_ADDR_A0_SDA_A1_VDD B1000110
#define INA226_ASUKIAAA_ADDR_A0_SCL_A1_VDD B1000111
#define INA226_ASUKIAAA_ADDR_A0_GND_A1_SDA B1001000
#define INA226_ASUKIAAA_ADDR_A0_VDD_A1_SDA B1001001
#define INA226_ASUKIAAA_ADDR_A0_SDA_A1_SDA B1001010
#define INA226_ASUKIAAA_ADDR_A0_SCL_A1_SDA B1001011
#define INA226_ASUKIAAA_ADDR_A0_GND_A1_SCL B1001100
#define INA226_ASUKIAAA_ADDR_A0_VDD_A1_SCL B1001101
#define INA226_ASUKIAAA_ADDR_A0_SDA_A1_SCL B1001110
#define INA226_ASUKIAAA_ADDR_A0_SCL_A1_SCL B1001111
*/

INA226_asukiaaa voltCurrMeter_2(INA226_2_ADDRESS, ina226calib);  // 2つ目のINA226

// -----------------------------------------------------------------------

extern "C" {
#include "user_interface.h"
}

#define PERIOD 300

BME280I2C bme280;  // Default : forced mode, standby time = 1000 ms
                   // Oversampling = pressure ×1, temperature ×1, humidity ×1, filter off,

WiFiClient client;
Ambient ambient;

const char *ssid     = "...ssid...";
const char *password = "...password...";

unsigned int channelId = 100;               // AmbientのチャネルID
const char *writeKey   = "...writeKey...";  // ライトキー

void setup() {
    int t = millis();
    // wifi_set_sleep_type(LIGHT_SLEEP_T);

    Serial.begin(SERIAL_BAUD);
    delay(10);

    Serial.println("Start");

    // BME280 setup
    while (!bme280.begin()) {
        Serial.println("Could not find BME280 sensor!");
        delay(1000);
    }

    switch (bme280.chipModel()) {
        case BME280::ChipModel_BME280:
            Serial.println("Found BME280 sensor! Success.");
            break;
        case BME280::ChipModel_BMP280:
            Serial.println("Found BMP280 sensor! No Humidity available.");
            break;
        default:
            Serial.println("Found UNKNOWN sensor! Error!");
    }

    // ----------------------------------------

    WiFi.begin(ssid, password);              //  Wi-Fi APに接続
    while (WiFi.status() != WL_CONNECTED) {  //  Wi-Fi AP接続待ち
        delay(100);
    }

    Serial.print("WiFi connected\r\nIP address: ");
    Serial.println(WiFi.localIP());

    // ----------------------------------------

    // 消費電力センサの初期化

    // 別ポートのI2Cポートを使用する場合
    // Wire2.begin(I2C_SDA, I2C_SCL);
    // voltCurrMeter.setWire(&Wire2);

    // センサ1つ目 がエラー
    if (voltCurrMeter_1.begin() != 0) {
        Serial.println("Failed to begin INA226_1");
    }
    // センサ2つ目 がエラー
    if (voltCurrMeter_2.begin() != 0) {
        Serial.println("Failed to begin INA226_2");
    }

    // ----------------------------------------
    // データ取得
    // ----------------------------------------

    // 変数 初期化
    // float temp(NAN), hum(NAN), pres(NAN);
    float temp = NAN, hum = NAN, pres = NAN;
    BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);  // 温度の単位設定
    BME280::PresUnit presUnit(BME280::PresUnit_Pa);       // 気圧の単位設定

    // データ取得
    bme280.read(pres, temp, hum, tempUnit, presUnit);
    
    Serial.print("temp: ");
    Serial.print(temp);
    Serial.print(", humid: ");
    Serial.print(hum);
    Serial.print(", pressure: ");
    Serial.println(pres);

    // ----------------------------------------

    // INA226 からデータを取得する
    // センサからの値を格納する変数
    // 多分、電流（mA）、電圧（mV）、電力（mW）
    int16_t ma1, mv1, mw1;  // センサ1 用
    int16_t ma2, mv2, mw2;  // センサ2 用

    // 電圧を取得する
    // 取得できたら、mvの変数に格納される
    // 取得できなかったら、0が返ってくる（mvの変数の値は変わらず）
    if (voltCurrMeter_1.readMV(&mv1) == 0) {
        Serial.println(String(mv1) + "mV");
    } else {
        // 取得できなかった
        Serial.println("Cannot read voltage.");
    }

    // 上の電流版
    if (voltCurrMeter_1.readMA(&ma1) == 0) {
        Serial.println(String(ma1) + "mA");
    } else {
        // 取得できなかった
        Serial.println("Cannot read current.");
    }

    // 電圧を取得する
    // 取得できたら、mvの変数に格納される
    // 取得できなかったら、0が返ってくる（mvの変数の値は変わらず）
    if (voltCurrMeter_2.readMV(&mv2) == 0) {
        Serial.println(String(mv2) + "mV");
    } else {
        // 取得できなかった
        Serial.println("Cannot read voltage.");
    }

    // 上の電流版
    if (voltCurrMeter_2.readMA(&ma2) == 0) {
        Serial.println(String(ma2) + "mA");
    } else {
        // 取得できなかった
        Serial.println("Cannot read current.");
    }

    // 上の電力版
    if (voltCurrMeter_2.readMW(&mv2) == 0) {
        Serial.println(String(mv2) + "mW");
    } else {
        // 取得できなかった
        Serial.println("Cannot read watt.");
    }

    // --------------------------------------------

    ambient.begin(channelId, writeKey, &client);  // チャネルIDとライトキーを指定してAmbientの初期化

    ambient.set(1, temp);  // 温度をデータ1にセット
    ambient.set(2, hum);   // 湿度をデータ2にセット
    ambient.set(3, pres);  // 気圧をデータ3にセット

    ambient.set(4, mv1);  // センサ1の電圧をデータ4にセット
    ambient.set(5, ma1);  // センサ1の電力をデータ5にセット
    ambient.set(6, ma2);  // センサ2の電流をデータ6にセット
    ambient.set(7, mv2);  // センサ2の電圧をデータ7にセット
    ambient.set(8, mw2);  // センサ2の電力をデータ8にセット

    ambient.send();  // データをAmbientに送信

    // --------------------------------------------

    t = millis() - t;
    t = (t < PERIOD * 1000) ? (PERIOD * 1000 - t) : 1;
    ESP.deepSleep(t * 1000, RF_DEFAULT);
    delay(1000);
}