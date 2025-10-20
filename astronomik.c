/*
 * ATtiny814 Astronomik Saat – Auto/Manual + EEPROM + Offset + Twilight
 * Tuna için hazırlanmıştır.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* -------------------- Kullanıcı Ayarları -------------------- */
// Konum (Yeni Foça, İzmir)
#define LAT_DEG      38.67f
#define LON_DEG      26.75f
// Zaman dilimi (Türkiye sabit UTC+3)
#define TZ_HOURS     3.0f

// Donanım pinleri (istersen değiştir)
#define PIN_RELAY_PORT  PORTA
#define PIN_RELAY_bm    PIN3_bm   // PA3
#define PIN_LED_PORT    PORTA
#define PIN_LED_bm      PIN1_bm   // PA1
#define PIN_BTN_PORT    PORTA
#define PIN_BTN_PINCTRL PORTA.PIN2CTRL
#define PIN_BTN_bm      PIN2_bm   // PA2 (buton, GND'ye basıyor)

// Röle/SSR çıkışının lojik polaritesi (true: HIGH=aktif)
#define RELAY_ACTIVE_HIGH  true

// Gün doğumu/gün batımı yerine "civil twilight" (−6°) kullanılacaksa ayar EEPROM'dan gelir.
// İlk açılışta default "resmi" (−0.833°) seçili.

/* -------------------- Zaman / Takvim -------------------- */
volatile uint32_t g_unix = 0;
volatile uint8_t  g_sec=0, g_min=0, g_hour=12;
volatile uint8_t  g_day=1, g_month=1;   // 1..31, 1..12
volatile uint16_t g_year=2025;

static inline bool is_leap(uint16_t y){
    return ( (y%4==0 && y%100!=0) || (y%400==0) );
}

static uint8_t days_in_month(uint16_t y, uint8_t m){
    static const uint8_t d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return (m==2) ? (d[1] + (is_leap(y)?1:0)) : d[m-1];
}

static uint16_t day_of_year(uint16_t y, uint8_t m, uint8_t d){
    static const uint16_t pref[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint16_t doy = pref[m-1] + d;
    if (is_leap(y) && m>2) doy++;
    return doy; // 1..365/366
}

/* -------------------- EEPROM Ayarları -------------------- */
typedef struct {
    uint8_t  magic;           // 0x42 ise geçerli
    int8_t   offset_sunrise;  // dakika (+ sonra kapatmak için +15 gibi)
    int8_t   offset_sunset;   // dakika (önce açmak için -10 gibi)
    uint8_t  twilight_sel;    // 0: resmi (-0.833°), 1: civil (-6°)
    uint8_t  mode;            // 0: AUTO, 1: MANUAL
    uint8_t  manual_state;    // 0: OFF, 1: ON
} settings_t;

settings_t EEMEM ee_settings = {
    .magic=0x42,
    .offset_sunrise=+15,  // gün doğumundan 15 dk sonra kapat
    .offset_sunset =-10,  // gün batımından 10 dk önce aç
    .twilight_sel  =0,    // resmi
    .mode          =0,    // AUTO
    .manual_state  =0
};
settings_t S; // RAM kopya

/* -------------------- Çıkış ve LED -------------------- */
static inline void relay_set(bool on){
#if RELAY_ACTIVE_HIGH
    if(on) PIN_RELAY_PORT.OUTSET = PIN_RELAY_bm; else PIN_RELAY_PORT.OUTCLR = PIN_RELAY_bm;
#else
    if(!on) PIN_RELAY_PORT.OUTSET = PIN_RELAY_bm; else PIN_RELAY_PORT.OUTCLR = PIN_RELAY_bm;
#endif
}

static inline void led_set(bool on){
    if(on) PIN_LED_PORT.OUTSET = PIN_LED_bm; else PIN_LED_PORT.OUTCLR = PIN_LED_bm;
}

static inline void led_toggle(void){ PIN_LED_PORT.OUTTGL = PIN_LED_bm; }

/* -------------------- Buton (debounce + kısayollar) -------------------- */
static uint16_t btn_cnt=0;       // ms sayacı
static bool     btn_last=false;
static uint16_t ms_tick=0;       // 1 ms zamanlayıcı (RTC’den türetilecek)

static bool button_read_raw(void){
    return !(PIN_BTN_PORT.IN & PIN_BTN_bm); // pull-up aktif, basılıysa 1 döndür
}

static void button_poll_1ms(void){
    bool cur = button_read_raw();
    if (cur==btn_last){
        if (btn_cnt<0xFFFF) btn_cnt++;
    } else {
        btn_cnt=0; btn_last=cur;
    }
}

/* -------------------- Astronomik Hesap -------------------- */

typedef struct { uint16_t rise_min; uint16_t set_min; } sun_t;

static inline uint16_t minutes_mod(int32_t m){
    while(m<0) m+=1440; while(m>=1440) m-=1440; return (uint16_t)m;
}

// doy: 1..365/366 -> ertesi gün için de aynı fonksiyonu çağıracağız
static sun_t astro_times_local(uint16_t doy){
    const float lat = LAT_DEG*(float)M_PI/180.0f;
    const float lon = LON_DEG;
    const float tz  = TZ_HOURS;

    float h0_deg = (S.twilight_sel ? -6.0f : -0.833f);
    const float h0 = h0_deg*(float)M_PI/180.0f;

    float g = 2.0f*(float)M_PI*((float)doy - 1.0f)/365.0f;
    float EoT = 229.18f*(0.000075f + 0.001868f*cosf(g) - 0.032077f*sinf(g)
                        - 0.014615f*cosf(2*g) - 0.040849f*sinf(2*g));
    float dec = 0.006918f - 0.399912f*cosf(g) + 0.070257f*sinf(g)
               -0.006758f*cosf(2*g) + 0.000907f*sinf(2*g)
               -0.002697f*cosf(3*g) + 0.001480f*sinf(3*g);

    float cosH0 = (cosf(h0) - sinf(lat)*sinf(dec)) / (cosf(lat)*cosf(dec));
    if (cosH0>1.0f)  cosH0=1.0f;
    if (cosH0<-1.0f) cosH0=-1.0f;

    float H0_deg = acosf(cosH0) * 180.0f/(float)M_PI;
    float solar_noon = 720.0f - 4.0f*lon - EoT + 60.0f*tz; // dakika

    float sunset  = solar_noon + 4.0f*H0_deg;
    float sunrise = solar_noon - 4.0f*H0_deg;

    sun_t s;
    s.rise_min = minutes_mod((int32_t)(sunrise + S.offset_sunrise + 0.5f));
    s.set_min  = minutes_mod((int32_t)(sunset  + S.offset_sunset  + 0.5f));
    return s;
}

/* -------------------- RTC: 32.768 kHz XOSC32K -> 1 Hz -------------------- */
static void rtc_init_xosc32k_1Hz(void){
    // 32k kristali enable (Run in Standby açık)
    CLKCTRL.XOSC32KCTRLA = CLKCTRL_RUNSTDBY_bm | CLKCTRL_CSUT_1K_gc | CLKCTRL_ENABLE_bm;

    while (RTC.STATUS & RTC_CTRLABUSY_bm);
    RTC.CLKSEL = RTC_CLKSEL_XOSC32K_gc;

    while (RTC.STATUS & RTC_CTRLABUSY_bm);
    RTC.CTRLA = RTC_PRESCALER_DIV32_gc; // 32768/32=1024
    RTC.PER   = 1023;                    // 1 Hz overflow
    RTC.INTCTRL = RTC_OVF_bm;
    RTC.CTRLA |= RTC_RTCEN_bm;
}

// Her saniye: zaman ilerlet, buton ms sayaçlarını çıkar
ISR(RTC_CNT_vect){
    RTC.INTFLAGS = RTC_OVF_bm;

    // 1 Hz "ms_tick": yaklaşık 1 ms çözünürlük elde etmek için 1 Hz'den yazılım sayacı yapmak yerine
    // basitçe 10 ms tempo kullanalım: saniyeyi 100'e bölecek sayaç:
    static uint8_t sub = 0;
    if (++sub >= 100){
        sub = 0;
    }
    ms_tick += 10;                    // ~10 ms

    // Zaman sayacı
    if (++g_sec == 60){ g_sec=0;
        if (++g_min == 60){ g_min=0;
            if (++g_hour == 24){ g_hour=0;
                // gün ilerlet
                if (++g_day > days_in_month(g_year, g_month)){
                    g_day = 1;
                    if (++g_month > 12){ g_month=1; g_year++; }
                }
            }
        }
    }
    g_unix++;  // kaba sayaç (epoch doğru değil ama göreli kullanıma yeter)

    led_toggle(); // heartbeat
}

/* -------------------- PLANLAYICI -------------------- */
typedef enum { MODE_AUTO=0, MODE_MANUAL=1 } runmode_t;
typedef struct {
    uint16_t sunrise_min_today;
    uint16_t sunset_min_today;
    bool     relay_on;
} plan_t;

static plan_t P;

static void compute_today_plan(void){
    uint16_t doy = day_of_year(g_year, g_month, g_day);
    sun_t s = astro_times_local(doy);
    P.sunrise_min_today = s.rise_min;
    P.sunset_min_today  = s.set_min;
}

static inline uint16_t minutes_now(void){
    return (uint16_t)(g_hour*60 + g_min);
}

static void apply_auto_logic(void){
    uint16_t now = minutes_now();
    bool target_on = false;

    // Basit mantık: gün batımında AÇ, gün doğumunda KAPAT
    if (P.sunset_min_today <= P.sunrise_min_today){
        // Normal durum (Foça): sunset akşam, sunrise ertesi sabah
        if (now >= P.sunset_min_today || now < P.sunrise_min_today) target_on = true;
    } else {
        // Teorik kutupsal durumlar için koruma (Foça’da olmaz)
        if (now >= P.sunset_min_today && now < P.sunrise_min_today) target_on = true;
    }

    if (target_on != P.relay_on){
        P.relay_on = target_on;
        relay_set(P.relay_on);
    }
}

/* -------------------- Başlatma -------------------- */
static void gpio_init(void){
    // Röle çıkışı
    PIN_RELAY_PORT.DIRSET = PIN_RELAY_bm;
#if RELAY_ACTIVE_HIGH
    PIN_RELAY_PORT.OUTCLR = PIN_RELAY_bm; // başlangıçta kapalı
#else
    PIN_RELAY_PORT.OUTSET = PIN_RELAY_bm;
#endif
    // LED
    PIN_LED_PORT.DIRSET = PIN_LED_bm;
    PIN_LED_PORT.OUTCLR = PIN_LED_bm;
    // Buton (pull-up, invert yok; GND'ye basıyor)
    PIN_BTN_PORT.DIRCLR = PIN_BTN_bm;
    PIN_BTN_PINCTRL = PORT_PULLUPEN_bm;
}

static void load_settings(void){
    eeprom_read_block(&S, &ee_settings, sizeof(S));
    if (S.magic != 0x42){
        // EEPROM boşsa fabrika varsayılanını yaz
        settings_t def;
        eeprom_read_block(&def, &ee_settings, sizeof(def)); // yukarıda tanımlı default değerleri çek
        S = def;
    }
}

/* -------------------- Buton İşlevleri -------------------- */
// Kısa bas ( <500 ms ): MANUAL modda röleyi toggle
// Uzun bas ( >2 s )   : AUTO <-> MANUAL geçiş ve EEPROM'a kaydet
// Çift kısa bas       : Twilight türünü değiştir (resmi <-> civil), EEPROM'a kaydet
static void button_service(void){
    static uint8_t  click_cnt = 0;
    static uint16_t click_timer = 0;
    static bool     handled_long = false;

    // 10 ms çözünürlüklü işleyici
    static uint16_t last_ms = 0;
    if (ms_tick == last_ms) return;
    last_ms = ms_tick;

    button_poll_1ms();

    // basılı kalma süresi
    static uint16_t hold_ms = 0;
    if (btn_last){
        if (hold_ms < 10000) hold_ms += 10;
        if (hold_ms >= 2000 && !handled_long){
            // uzun bas: mod değiştir
            handled_long = true;
            S.mode = (S.mode==MODE_AUTO) ? MODE_MANUAL : MODE_AUTO;
            eeprom_write_byte((uint8_t*)&ee_settings.mode, S.mode);
            // LED hızlı üç kez
            for(int i=0;i<3;i++){ led_set(1); _delay_ms(120); led_set(0); _delay_ms(120); }
        }
    } else {
        if (hold_ms > 20 && hold_ms < 500 && !handled_long){
            // kısa bas
            click_cnt++;
            click_timer = 300; // 300 ms içinde ikinci klik gelirse "çift tık"
        }
        hold_ms = 0;
        handled_long = false;
    }

    // çift tık zamanlaması
    if (click_timer){
        click_timer -= 10;
        if (click_timer==0){
            if (click_cnt==1){
                // tek kısa bas
                if (S.mode==MODE_MANUAL){
                    S.manual_state = !S.manual_state;
                    relay_set(S.manual_state);
                    eeprom_write_byte((uint8_t*)&ee_settings.manual_state, S.manual_state);
                }
            } else if (click_cnt>=2){
                // çift tık: twilight değiştir
                S.twilight_sel ^= 1;
                eeprom_write_byte((uint8_t*)&ee_settings.twilight_sel, S.twilight_sel);
                compute_today_plan(); // eşikleri güncelle
                // LED iki kez uzun
                for(int i=0;i<2;i++){ led_set(1); _delay_ms(200); led_set(0); _delay_ms(200); }
            }
            click_cnt=0;
        }
    }
}

/* -------------------- ANA -------------------- */
int main(void){
    gpio_init();
    load_settings();
    rtc_init_xosc32k_1Hz();
    sei();

    // İlk tarih/saat – üretimde bir kez doğruya set etmelisin (UART yoksa kodda güncelle):
    g_year=2025; g_month=10; g_day=19; g_hour=17; g_min=0; g_sec=0;

    compute_today_plan();
    P.relay_on = false;
    relay_set(false);

    uint8_t last_min = 255;

    for(;;){
        button_service();

        // Her yeni dakikada plan uygulama
        if (g_min != last_min){
            last_min = g_min;

            if (S.mode==MODE_AUTO){
                apply_auto_logic();
            } else {
                // MANUAL kipte röleyi EEPROM'daki ile sabit tut
                if (P.relay_on != (bool)S.manual_state){
                    P.relay_on = (bool)S.manual_state;
                    relay_set(P.relay_on);
                }
            }

            // 00:05'te ertesi eşikleri değil, bugünün eşiklerini kullanıyoruz; günlük yeniden hesapla
            if (g_hour==0 && g_min==5){
                compute_today_plan();
            }
        }

        // (İsteğe bağlı) düşük güç için burada STANDBY uykuya geçebilirsin.
        // RTC kesmesi uyandırır.
    }
}
