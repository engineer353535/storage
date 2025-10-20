#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// === OLED Tanımları ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1        // RST kullanılmıyorsa -1
#define OLED_ADDR     0x3C      // SSD1306 I²C adresi

// === STM32 I²C Pinleri ===
#define I2C_SDA       PB7
#define I2C_SCL       PB6

// Ölçüm yapılacak ADC pini
#define ADC_PIN       PA0

// Nesne başlatma
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Ölçeklendirme sabitleri
const float dividerRatio = 3.0f;   // 0–10 V'u 0–3.3 V'a düşüren bölücü
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
  // I²C başlat (STM32)
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

  // ADC konfigürasyon (STM32)
  analogReadResolution(12);

  // İlk basınç ölçümü (referans)
  uint16_t raw0  = analogRead(ADC_PIN);
  float vAdc0    = (raw0 / 4095.0f) * 3.3f;
  float vIn0     = vAdc0 * dividerRatio;
  lastPressureBar = (vIn0 / vinMax) * pMaxBar;

  delay(500);
}

void loop() {
  // ADC ölçümü
  uint16_t raw = analogRead(ADC_PIN);
  float vAdc  = (raw / 4095.0f) * 3.3f;
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

