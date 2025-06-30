#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_ADS1X15.h>

// === OLED Tanımları ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1        // RST kullanılmıyorsa -1
#define OLED_ADDR     0x3C      // SSD1306 I²C adresi

// === ESP32 I²C Pinleri ===
#define I2C_SDA       21
#define I2C_SCL       22

// === ADS1115 Tanımları ===
#define ADS_ADDR      0x48      // ADDR→GND ise 0x48

// Nesne başlatma
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_ADS1115   ads(ADS_ADDR);

// Ölçeklendirme sabitleri
const float dividerRatio = 2.0f;   // 10k–10k bölücü için
const float vinMax        = 10.0f; // sensör çıkışı 0–10 V
const float pMaxBar       = 6.0f;  // ölçeklenecek 0–6 bar
const float dropThreshold = 0.1f;  // “küçük” düşüş eşiği (bar)

// Alarm yönetimi
bool    inAlert       = false;
uint32_t alertStartMs = 0;
const   uint32_t alertDuration = 3000; // ms (3 sn)

// Önceki basınç değeri
float lastPressureBar = 0;

void setup() {
  // I²C başlat (ESP32)
  Wire.begin(I2C_SDA, I2C_SCL);

  // OLED ekran başlatma
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    while (1) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.println(F("OLED BASLATILAMADI"));
      display.display();
      delay(500);
    }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // ADS1115 başlatma
  if (!ads.begin()) {
    while (1) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println(F("ADS1115 BASLATILAMADI"));
      display.display();
      delay(500);
    }
  }
  ads.setGain(GAIN_TWOTHIRDS); // ±6.144 V tam ölçek

  // İlk basınç ölçümü (referans)
  int16_t raw0   = ads.readADC_SingleEnded(0);
  float vAdc0    = raw0 * (6.144f / 32767.0f);
  float vIn0     = vAdc0 * dividerRatio;
  lastPressureBar = (vIn0 / vinMax) * pMaxBar;

  delay(500);
}

void loop() {
  // ADC ölçümü
  int16_t raw = ads.readADC_SingleEnded(0);
  float vAdc  = raw * (6.144f / 32767.0f);
  float vIn   = vAdc * dividerRatio;
  float pressureBar = (vIn / vinMax) * pMaxBar;

  // Basınç farkı
  float deltaP = lastPressureBar - pressureBar;
  float leakRate = deltaP; // yaklaşık bar/s

  // “Küçük düşüş” alarmı
  if (!inAlert && deltaP > 0.0f && deltaP < dropThreshold) {
    inAlert      = true;
    alertStartMs = millis();
  }
  // 3 sn geçtiyse alarmı sonlandır
  if (inAlert && millis() - alertStartMs > alertDuration) {
    inAlert = false;
  }

  // Ekranı güncelle
  display.clearDisplay();

  // Sol 64x64 bölgede basınç
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(pressureBar, 2);
  display.println(F(" bar"));

  // Sağ 64x64 bölge: sol yarı kaçak uyarısı, sağ yarı kaçak hızı
  display.setTextSize(2);
  if (inAlert) {
    display.setCursor(70, 0);
    display.println(F("!"));
  }
  display.setTextSize(1);
  display.setCursor(98, 28);
  display.print(leakRate, 2);
  display.println(F(" b/s"));

  display.display();

  // Bir sonraki ölçüm için kaydet
  lastPressureBar = pressureBar;

  delay(1000);
}

