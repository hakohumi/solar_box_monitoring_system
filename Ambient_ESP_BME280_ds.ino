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
// https://github.com/asukiaaa/INA226_asukiaaa
#include <INA226_asukiaaa.h>

// もし、I2Cのポートを変更する場合は、wire2関数あたりで設定する必要あり
// I2C用のポート
const uint8_t I2C_PORT_SDA = 4;
const uint8_t I2C_PORT_SCL = 5;

#define SERIAL_BAUD 115200     // Serial 通信レート
#define BME280_ADDRESS 0x76    // BME280 スレーブアドレス
#define INA226_1_ADDRESS 0x40  // ソーラー側INA226 スレーブアドレス
#define INA226_2_ADDRESS 0x41  // バッテリー側INA226 スレーブアドレス

// センサからデータを取得して、データをambientに送信する間隔 単位は[s]
// 1時間 = 60分 * 60秒 = 3600秒
#define SEND_DATA_AMBIENT_INTERVAL_SEC 3600

// 平均化のために取得するデータの個数
#define SENSING_DATA_COUNT 3

// シャント抵抗の値を引数にすると、キャリブレーションの値を出力する関数
// 2つのINA226でそれぞれシャント抵抗が違う場合は、それぞれの抵抗値で計算した値が必要
const uint16_t ina226calib = INA226_asukiaaa::calcCalibByResistorMilliOhm(2);  // Max 5120 milli ohm

// 1つ目のINA226のスレーブアドレス
INA226_asukiaaa voltCurrMeter_1(INA226_1_ADDRESS, ina226calib);
// 2つ目のINA226のスレーブアドレス
INA226_asukiaaa voltCurrMeter_2(INA226_2_ADDRESS, ina226calib);

// -----------------------------------------------------------------------

extern "C" {
#include "user_interface.h"
}

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

    Serial.println("Wake up now");

    // I2Cの設定
    Wire.begin(I2C_PORT_SDA, I2C_PORT_SCL);
    voltCurrMeter.setWire(&Wire);

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
    /*
    温度、湿度、気圧
    電流、電圧、電力

    1秒間隔 3回 平均
    */
    float temp_total = 0.0f, hum_total = 0.0f, pres_total = 0.0f;  // 平均するようの変数
    uint8_t BME280_read_count = 0;

    int16_t ma1_total, mv1_total;             // ina226_1 の平均値合計用の変数
    int16_t ma2_total, mv2_total, mw2_total;  // ina226_2 の平均値合計用の変数
    uint8_t INA226_1_read_count = 0;
    uint8_t INA226_2_read_count = 0;

    // ----------------------------------------
    // 3回データを1秒間隔で取得する
    for (int c = 0; c < SENSING_DATA_COUNT; c++) {
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

        // 平均計算用
        temp_total += temp;
        hum_total += hum;
        pres_total += pres;
        BME280_read_count++;

        // INA226 からデータを取得する
        // センサからの値を格納する変数
        // 多分、電流（mA）、電圧（mV）、電力（mW）
        int16_t ma1, mv1;       // センサ1 用
        int16_t ma2, mv2, mw2;  // センサ2 用

        // 電圧・電流を取得する
        // 取得できたら、mvの変数に格納される
        // 取得できなかったら、0が返ってくる（mvの変数の値は変わらず）
        if ((voltCurrMeter_1.readMV(&mv1) + voltCurrMeter_1.readMA(&ma1)) == 0) {
            Serial.println(String(mv1) + "mV");
            Serial.println(String(ma1) + "mA");
            mv1_total += mv1;       // 平均計算用
            ma1_total += ma1;       // 平均計算用
            INA226_1_read_count++;  // 平均計算用
        } else {
            // 取得できなかった
            Serial.println("Cannot read INA226_1.");
        }

        // 電圧・電流・電力を取得する
        // 取得できたら、mvの変数に格納される
        // 取得できなかったら、0が返ってくる（mvの変数の値は変わらず）
        if ((voltCurrMeter_2.readMV(&mv2) + voltCurrMeter_2.readMA(&ma2) + voltCurrMeter_2.readMW(&mv2)) == 0) {
            Serial.println(String(mv2) + "mV");
            Serial.println(String(ma2) + "mA");
            Serial.println(String(mv2) + "mW");
            mv2_total += mv2;       // 平均計算用
            ma2_total += ma2;       // 平均計算用
            mw2_total += mw2;       // 平均計算用
            INA226_2_read_count++;  // 平均計算用
        } else {
            // 取得できなかった
            Serial.println("Cannot read watt.");
        }

        delay(1000);  // 平均用にセンサ値を取得する間隔
    }
    // --------------------------------------------

    float average_temp = temp_total / (float)BME280_read_count;
    float average_hum  = hum_total / (float)BME280_read_count;
    float average_pres = pres_total / (float)BME280_read_count;
    float average_mv1  = (float)mv1_total / (float)INA226_1_read_count;
    float average_ma1  = (float)ma1_total / (float)INA226_1_read_count;
    float average_mv2  = (float)mv2_total / (float)INA226_2_read_count;
    float average_ma2  = (float)ma2_total / (float)INA226_2_read_count;
    float average_mw2  = (float)mw2_total / (float)INA226_2_read_count;

    ambient.begin(channelId, writeKey, &client);  // チャネルIDとライトキーを指定してAmbientの初期化

    ambient.set(1, average_temp);  // 温度をデータ1にセット
    ambient.set(2, average_hum);   // 湿度をデータ2にセット
    ambient.set(3, average_pres);  // 気圧をデータ3にセット

    ambient.set(4, average_mv1);  // センサ1の電圧をデータ4にセット
    ambient.set(5, average_ma1);  // センサ1の電流をデータ5にセット
    ambient.set(6, average_mv2);  // センサ2の電圧をデータ6にセット
    ambient.set(7, average_ma2);  // センサ2の電流をデータ7にセット
    ambient.set(8, average_mw2);  // センサ2の電力をデータ8にセット

    ambient.send();  // データをAmbientに送信

    // --------------------------------------------
    // プログラムの動作にかかった時間分
    t = millis() - t;
    t = (t < SEND_DATA_AMBIENT_INTERVAL_SEC * 1000) ? (SEND_DATA_AMBIENT_INTERVAL_SEC * 1000 - t) : 1;

    ESP.deepSleep(t * 1000, RF_DEFAULT);  // DeepSleepで復帰する時間[us]
    delay(1000);                          // DeepSleepに入るための準備時間
}

void loop() {}