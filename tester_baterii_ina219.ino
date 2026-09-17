// ===================================================================
// tester_baterii_ina219.ino
// Tester baterii 18650 na tym samym ESP32 + TFT ILI9488 480x320
// (LovyanGFX) + dotyk XPT2046 + SD, co stacja pogody.
//
// Pomiar napiecia/pradu: INA219 (I2C, bocznik R100 = 0,1 Ohm)
//   VCC -> 3V3, GND -> GND, SDA -> GPIO21, SCL -> GPIO22
//   VIN+ / VIN- -> osobna sciezka pradowa: bateria(+) -> VIN+,
//   VIN- -> obciazenie, wspolny minus baterii i obciazenia -> GND
//
// Test rozladowania: START/STOP dotykiem, calkowanie pojemnosci
// (mAh) i energii (mWh) w czasie, automatyczne zatrzymanie przy
// zadanym napieciu odciecia (domyslnie 3,00 V, regulowane +/- na
// ekranie), log CSV na karcie SD (czas_s, V, mA, mW, mAh, mWh).
// Obciazenie jest reczne/zewnetrzne - program tylko informuje,
// kiedy trzeba je odlaczyc.
//
// WYMAGANE BIBLIOTEKI (Arduino Library Manager):
//   - LovyanGFX
//   - Adafruit INA219  (razem z zaleznoscia Adafruit BusIO)
// Reszta (Wire, SPI, SD) jest wbudowana w rdzen ESP32.
// ===================================================================

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// Wszystkie polozenia/rozmiary elementow na ekranie sa w osobnym pliku
// ustawienia.h (druga zakladka tego samego szkicu w Arduino IDE) -
// tam mozna je zmieniac bez grzebania w reszcie kodu.
#include "ustawienia.h"

// ---------- Piny TFT (magistrala VSPI) ----------
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_MOSI 23
#define TFT_MISO 19
#define TFT_SCLK 18
#define TFT_BL   32

// ---------- Piny dotyku XPT2046 (osobna magistrala HSPI) ----------
#define TOUCH_CS   15
#define TOUCH_MOSI 13
#define TOUCH_MISO 12
#define TOUCH_SCLK 14

// ---------- Piny SD (dzieli SCK/MOSI/MISO z TFT na VSPI) ----------
#define SD_CS 26

// ---------- Piny I2C (INA219) ----------
#define I2C_SDA 21
#define I2C_SCL 22

// ===================================================================
// Konfiguracja LovyanGFX (panel + dotyk) - te same ustawienia co w
// stacji pogody, potwierdzone dzialajace na tym samym sprzecie.
// ===================================================================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = TFT_SCLK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_dc   = TFT_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = TFT_CS;
      cfg.pin_rst  = TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width  = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = TFT_BL;
      cfg.invert = false;
      cfg.freq   = 44100;
      cfg.pwm_channel = 0;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 300;
      cfg.x_max = 3800;
      cfg.y_min = 200;
      cfg.y_max = 3700;
      cfg.pin_int  = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = 6;
      cfg.spi_host = HSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = TOUCH_SCLK;
      cfg.pin_mosi = TOUCH_MOSI;
      cfg.pin_miso = TOUCH_MISO;
      cfg.pin_cs   = TOUCH_CS;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;
bool sdDostepna = false;
uint16_t calData[8] = { 3749, 3883, 3765, 292, 276, 3879, 309, 283 };

bool pobierzDotyk(int32_t &x, int32_t &y) {
  if (!tft.getTouch(&x, &y)) return false;
  x = tft.width()  - x;
  y = tft.height() - y;
  return true;
}

// ===================================================================
// INA219
// ===================================================================
Adafruit_INA219 ina219;
bool ina219Ok = false;

// Korekcja prądu/mocy - ten konkretny moduł INA219 systematycznie
// zawyża odczyty prądu (sprawdzone multimetrem w serii z tym samym
// obciążeniem: 500mA realnie, moduł pokazywał 622mA), najpewniej
// bocznik w module ma inną rzeczywistą wartość niż zakładane w
// bibliotece 0,1 Ohm. Napięcie moduł mierzy dobrze (zgodne z
// multimetrem), więc korygujemy tylko prąd i moc.
const float KOREKTA_PRADU = 500.0 / 622.0; // ~0.804 - podmien, jesli zmienisz modul/rezystor

float odczytajPradMa() {
  return ina219.getCurrent_mA() * KOREKTA_PRADU;
}

float odczytajMocMw() {
  return ina219.getPower_mW() * KOREKTA_PRADU;
}

float napiecie = 0;     // V (napiecie na baterii, bus + spadek na boczniku)
float prad = 0;          // mA
float moc = 0;            // mW

// ===================================================================
// Stan testu
// ===================================================================
enum StanTestu { CZEKA, W_TRAKCIE, ZAKONCZONY };
StanTestu stan = CZEKA;

float progOdciecia = 3.00; // V - regulowany +/- na ekranie
const float PROG_MIN = 2.50;
const float PROG_MAX = 4.20;

unsigned long testStartMillis = 0;
unsigned long ostatniPomiarMillis = 0;
unsigned long ostatniLogMillis = 0;
double pojemnoscMah = 0;
double energiaMwh = 0;

File plikLogu;
String nazwaPlikuLogu = "";

unsigned long ostatniDotyk = 0;
unsigned long ostatnieOdswiezenie = 0;

// ===================================================================
// Strony ekranu i szybki test kondycji (rezystancja wewnetrzna)
// ===================================================================
enum Strona { STR_GLOWNA, STR_KONDYCJA };
Strona aktualnaStrona = STR_GLOWNA;

enum StanSzybkiegoTestu { ST_GOTOWY, ST_CZEKA_BEZ_OBCIAZENIA, ST_CZEKA_POD_OBCIAZENIEM, ST_WYNIK };
StanSzybkiegoTestu stanSzybki = ST_GOTOWY;

float vOtwarte = 0;               // V - napiecie bez obciazenia (krok 1)
float iOtwarte = 0;                // mA - prad w kroku 1 (powinien byc bliski 0)
float vObciazone = 0;             // V - napiecie pod obciazeniem (krok 2)
float iObciazone = 0;              // mA - prad pod obciazeniem (krok 2)
float rezystancjaWewnetrzna = -1; // mOhm, -1 = brak wyniku / blad pomiaru

// Struktura przycisku dotykowego.
struct Przycisk { int x, y, w, h; };
Przycisk btnMinus  = {BTN_MINUS_X, BTN_MINUS_Y, BTN_MINUS_W, BTN_MINUS_H};
Przycisk btnPlus   = {BTN_PLUS_X, BTN_PLUS_Y, BTN_PLUS_W, BTN_PLUS_H};
Przycisk btnStart  = {BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H};

// Druga strona - szybki test kondycji.
Przycisk btnKondycja      = {BTN_KONDYCJA_X, BTN_KONDYCJA_Y, BTN_KONDYCJA_W, BTN_KONDYCJA_H}; // strona glowna -> przelacza na strone kondycji
Przycisk btnPowrot        = {BTN_POWROT_X, BTN_POWROT_Y, BTN_POWROT_W, BTN_POWROT_H};         // strona kondycji -> powrot na strone glowna
Przycisk btnAkcjaKondycja = {BTN_AKCJA_KONDYCJA_X, BTN_AKCJA_KONDYCJA_Y, BTN_AKCJA_KONDYCJA_W, BTN_AKCJA_KONDYCJA_H}; // duzy przycisk sterujacy krokami testu kondycji

// Te dwa przyciski siedza blisko gornej/prawej krawedzi ekranu, gdzie
// tanie ekrany rezystancyjne czesto maja najgorsza kalibracje dotyku -
// dotyk rejestruje sie kawalek NIZEJ niz to, co widac narysowane.
// Zamiast przerabiac cala kalibracje, powiekszamy tylko obszar
// wykrywania dotyku (w dol) dla tych dwoch przyciskow - to, co widac
// na ekranie (btnKondycja/btnPowrot powyzej), zostaje bez zmian.
// Wysokosci BTN_*_DOTYK_H (w ustawienia.h) sa wieksze niz BTN_*_H.
Przycisk btnKondyckaDotyk = {BTN_KONDYCJA_X, BTN_KONDYCJA_Y, BTN_KONDYCJA_W, BTN_KONDYCJA_DOTYK_H};
Przycisk btnPowrotDotyk   = {BTN_POWROT_X, BTN_POWROT_Y, BTN_POWROT_W, BTN_POWROT_DOTYK_H};

// Recznie napisany prototyp funkcji korzystajacej z typu Przycisk.
// Arduino IDE samo generuje prototypy wszystkich funkcji i wstawia je
// TUZ PO OSTATNIM #include, czyli zanim struktura Przycisk jest w ogole
// zdefiniowana w pliku - to samo w sobie powodowalo blad kompilacji.
// Wlasny, recznie napisany prototyp (ponizej, PO definicji struktury)
// sprawia, ze Arduino nie generuje juz wlasnego dla tej funkcji.
bool wPrzycisku(int32_t x, int32_t y, Przycisk b);

// ===================================================================
// SD - log CSV
// ===================================================================
void inicjalizujSD() {
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, SD_CS);
  sdDostepna = SD.begin(SD_CS, SPI, 20000000);
}

String znajdzWolnaNazwePliku() {
  for (int i = 1; i < 1000; i++) {
    char nazwa[24];
    snprintf(nazwa, sizeof(nazwa), "/test_%03d.csv", i);
    if (!SD.exists(nazwa)) return String(nazwa);
  }
  return "/test_log.csv";
}

// ===================================================================
// Test rozladowania
// ===================================================================
void rozpocznijTest() {
  stan = W_TRAKCIE;
  testStartMillis = millis();
  ostatniPomiarMillis = testStartMillis;
  ostatniLogMillis = testStartMillis;
  pojemnoscMah = 0;
  energiaMwh = 0;

  if (sdDostepna) {
    nazwaPlikuLogu = znajdzWolnaNazwePliku();
    plikLogu = SD.open(nazwaPlikuLogu.c_str(), FILE_WRITE);
    if (plikLogu) {
      plikLogu.println("czas_s,napiecie_V,prad_mA,moc_mW,pojemnosc_mAh,energia_mWh");
      plikLogu.flush();
    }
  }
}

void zakonczTest() {
  stan = ZAKONCZONY;
  if (plikLogu) {
    plikLogu.flush();
    plikLogu.close();
  }
}

void resetujDoGotowosci() {
  stan = CZEKA;
}

void aktualizujPomiar() {
  if (!ina219Ok) return;

  float busV = ina219.getBusVoltage_V();
  float shuntMv = ina219.getShuntVoltage_mV();
  napiecie = busV + (shuntMv / 1000.0);
  prad = odczytajPradMa();
  moc = odczytajMocMw();

  if (stan == W_TRAKCIE) {
    unsigned long teraz = millis();
    double dtGodz = (teraz - ostatniPomiarMillis) / 1000.0 / 3600.0;
    if (prad > 0) { // liczymy tylko rozladowanie (prad plynacy z baterii)
      pojemnoscMah += prad * dtGodz;
      energiaMwh += moc * dtGodz;
    }
    ostatniPomiarMillis = teraz;

    // log na SD co 5 sekund
    if (plikLogu && (teraz - ostatniLogMillis > 5000)) {
      ostatniLogMillis = teraz;
      unsigned long czasTestuS = (teraz - testStartMillis) / 1000;
      plikLogu.printf("%lu,%.3f,%.1f,%.1f,%.2f,%.2f\n",
                       czasTestuS, napiecie, prad, moc, pojemnoscMah, energiaMwh);
      plikLogu.flush();
    }

    // automatyczne zatrzymanie przy progu odciecia
    if (napiecie <= progOdciecia) {
      zakonczTest();
    }
  }
}

// ===================================================================
// Szybki test kondycji (rezystancja wewnetrzna)
// ===================================================================
// Bez przekaznika nie da sie automatycznie odlaczac obciazenia, wiec
// uzytkownik robi to recznie w dwoch krokach prowadzonych przez ekran:
//  1) odlacza rezystor od masy -> pomiar napiecia "bez obciazenia"
//  2) podlacza rezystor z powrotem -> pomiar napiecia i pradu "pod
//     obciazeniem"
// Z roznicy napiec i pradu liczymy przyblizona rezystancje wewnetrzna
// ogniwa (razem z rezystancja przewodow/stykow, ktorej nie da sie tu
// oddzielic) - to wskaznik POROWNAWCZY (miedzy ogniwami, w czasie),
// a nie pomiar laboratoryjny.
void zmierzSrednio(float &vOut, float &iOut, int probek, int opoznienieMs) {
  float sumaV = 0, sumaI = 0;
  for (int k = 0; k < probek; k++) {
    float busV = ina219.getBusVoltage_V();
    float shuntMv = ina219.getShuntVoltage_mV();
    sumaV += busV + (shuntMv / 1000.0);
    sumaI += odczytajPradMa();
    delay(opoznienieMs);
  }
  vOut = sumaV / probek;
  iOut = sumaI / probek;
}

void obliczRezystancjeWewnetrzna() {
  float iAmp = iObciazone / 1000.0;
  float deltaV = vOtwarte - vObciazone;
  if (iOtwarte > 20 || iAmp < 0.02 || deltaV <= 0) {
    // rezystor prawdopodobnie nie zostal odlaczony/podlaczony we
    // wlasciwym kroku, albo prad jest za maly, zeby cokolwiek policzyc
    rezystancjaWewnetrzna = -1;
  } else {
    rezystancjaWewnetrzna = (deltaV / iAmp) * 1000.0; // mOhm
  }
}

// ===================================================================
// RYSOWANIE
// ===================================================================

bool wPrzycisku(int32_t x, int32_t y, Przycisk b) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

void rysujPrzyciskProg() {
  tft.fillRect(0, PASEK_PROG_Y, tft.width(), PASEK_PROG_H, TFT_BLACK); // zaczyna sie dokladnie pod tytulem, zeby go nie przycinac
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(PROG_TEKST_X, PROG_TEKST_Y);
  tft.printf("Prog odciecia: %.2f V", progOdciecia);

  tft.fillRect(btnMinus.x, btnMinus.y, btnMinus.w, btnMinus.h, TFT_NAVY);
  tft.drawRect(btnMinus.x, btnMinus.y, btnMinus.w, btnMinus.h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(2);
  tft.setCursor(btnMinus.x + 12, btnMinus.y + 4);
  tft.print("-");

  tft.fillRect(btnPlus.x, btnPlus.y, btnPlus.w, btnPlus.h, TFT_NAVY);
  tft.drawRect(btnPlus.x, btnPlus.y, btnPlus.w, btnPlus.h, TFT_WHITE);
  tft.setCursor(btnPlus.x + 10, btnPlus.y + 4);
  tft.print("+");

  tft.fillRect(btnKondycja.x, btnKondycja.y, btnKondycja.w, btnKondycja.h, TFT_DARKGREEN);
  tft.drawRect(btnKondycja.x, btnKondycja.y, btnKondycja.w, btnKondycja.h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.setTextSize(1);
  tft.setCursor(btnKondycja.x + 6, btnKondycja.y + 8);
  tft.print("Test kondycji >");
}

void rysujStatyczneElementy() {
  tft.fillScreen(TFT_BLACK);

  tft.drawFastHLine(0, GORNA_LINIA_Y, tft.width(), TFT_DARKGREY);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  {
    const char *tytul = "Tester baterii 18650";
    int szerTytulu = strlen(tytul) * 12; // przyblizona szerokosc znaku przy size2
    int txTytul = (tft.width() - szerTytulu) / 2;
    tft.setCursor(txTytul, TYTUL_Y);
    tft.print(tytul);
  }

  rysujPrzyciskProg();

  tft.drawFastHLine(0, LINIA1_Y, tft.width(), TFT_DARKGREY);
  tft.drawFastHLine(0, LINIA2_Y, tft.width(), TFT_DARKGREY);
  tft.drawFastHLine(0, LINIA3_Y, tft.width(), TFT_DARKGREY);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(KOL_NAPIECIE_X, ETYKIETA1_Y);
  tft.print("NAPIECIE");
  tft.setCursor(KOL_PRAD_X, ETYKIETA1_Y);
  tft.print("PRAD");
  tft.setCursor(KOL_MOC_X, ETYKIETA1_Y);
  tft.print("MOC");

  tft.setCursor(KOL_CZAS_X, ETYKIETA2_Y);
  tft.print("CZAS TESTU");
  tft.setCursor(KOL_POJEMNOSC_X, ETYKIETA2_Y);
  tft.print("POJEMNOSC");
  tft.setCursor(KOL_ENERGIA_X, ETYKIETA2_Y);
  tft.print("ENERGIA");

  rysujRamkeBaterii();
}

void rysujPomiary() {
  // Bez fillRect przed kazdym printf - to ono powodowalo migotanie
  // (czarny "blysk" na chwile przed narysowaniem nowej wartosci).
  // Zamiast tego: staly szerokosc pola (spacje z lewej strony), zeby
  // stara, dluzsza liczba zawsze zostala calkowicie nadpisana, a tlo
  // pod kazdym znakiem i tak jest czyszczone przez setTextColor(fg,bg).
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);

  tft.setCursor(KOL_NAPIECIE_X, WARTOSC1_Y);
  tft.printf("%5.2fV", napiecie);

  tft.setCursor(KOL_PRAD_X, WARTOSC1_Y);
  tft.printf("%4.0fmA", prad);

  tft.setCursor(KOL_MOC_X, WARTOSC1_Y);
  tft.printf("%4.0fmW", moc);
}

void rysujStatystykiTestu() {
  unsigned long czasS = (stan == CZEKA) ? 0 : (millis() - testStartMillis) / 1000;
  unsigned int godz = czasS / 3600;
  unsigned int min = (czasS % 3600) / 60;
  unsigned int sek = czasS % 60;

  // Tak samo jak w rysujPomiary() - bez fillRect, stala szerokosc pola.
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);

  tft.setCursor(KOL_CZAS_X, WARTOSC2_Y);
  tft.printf("%02u:%02u:%02u", godz, min, sek);

  tft.setCursor(KOL_POJEMNOSC_X, WARTOSC2_Y);
  tft.printf("%7.1f mAh", pojemnoscMah);

  tft.setCursor(KOL_ENERGIA_X, WARTOSC2_Y);
  tft.printf("%6.2f Wh", energiaMwh / 1000.0);
}

void rysujStatus() {
  tft.fillRect(0, STATUS_Y, tft.width(), STATUS_H, TFT_BLACK); // wiersz zwezony (byl wyzszy, zanim usunieto "Gotowy do startu testu")
  tft.setTextSize(2);
  tft.setCursor(STATUS_TEKST_X, STATUS_TEKST_Y);

  if (!ina219Ok) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("Brak polaczenia z INA219!");
  } else if (stan == CZEKA) {
    // Usuniety napis "Gotowy do startu testu" na prosbe - wiersz zostaje pusty.
  } else if (stan == W_TRAKCIE) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.print("Trwa test rozladowania...");
  } else if (stan == ZAKONCZONY) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("KONIEC TESTU - odlacz baterie!");
  }
}

void rysujPrzyciskStart() {
  uint16_t kolor;
  uint16_t kolorTekstu = TFT_BLACK; // granat jest za ciemny na czarny tekst - patrz nizej
  const char *napis;
  if (stan == CZEKA) { kolor = TFT_GREEN; napis = "START TESTU"; }
  else if (stan == W_TRAKCIE) { kolor = TFT_RED; napis = "ZATRZYMAJ TEST"; }
  else { kolor = TFT_NAVY; kolorTekstu = TFT_WHITE; napis = "DOTKNIJ, BY ZRESETOWAC"; }

  tft.fillRect(btnStart.x, btnStart.y, btnStart.w, btnStart.h, kolor);
  tft.drawRect(btnStart.x, btnStart.y, btnStart.w, btnStart.h, TFT_WHITE);
  tft.setTextColor(kolorTekstu, kolor);
  tft.setTextSize(2); // przycisk jest teraz nizszy, wiec mniejszy tekst (bylo size3)
  int szerTekstu = strlen(napis) * 12; // przyblizona szerokosc znaku przy size2
  int tx = btnStart.x + (btnStart.w - szerTekstu) / 2;
  if (tx < btnStart.x + 5) tx = btnStart.x + 5;
  tft.setCursor(tx, btnStart.y + (btnStart.h - 16) / 2);
  tft.print(napis);
}

void rysujRamkeBaterii() {
  // Statyczna ramka - rysowana raz (wywolywana z rysujStatyczneElementy()),
  // NIE co 300 ms. rysujBaterie() ponizej tylko podmienia wypelnienie i
  // procent, dzieki czemu ramka juz nie miga.
  tft.drawRect(BAT_X, BAT_Y, BAT_W, BAT_H, TFT_WHITE);
  tft.fillRect(BAT_X + BAT_W, BAT_Y + (BAT_H - BAT_NUB_H) / 2, BAT_NUB_W, BAT_NUB_H, TFT_WHITE);
}

void rysujBaterie() {
  // Orientacyjny wskaznik naladowania, liczony z biezacego napiecia wg
  // umownej krzywej ogniwa 18650 (3,00V = 0%, 4,20V = 100%) - pod
  // obciazeniem (test w trakcie) napiecie sie zanizy, wiec pasek wtedy
  // pokaze mniej niz realny stan naladowania. To normalne.
  //
  // Migotanie brало sie stad, ze kazde odswiezenie najpierw czyscilo caly
  // obszar na czarno (lacznie z ramka), a dopiero potem rysowalo wszystko
  // od nowa. Teraz: ramka jest juz narysowana (rysujRamkeBaterii, raz), a
  // tutaj tylko podmieniamy pasek wypelnienia - kolorem do fillW, a reszte
  // wnetrza (jesli sie skurczyl) na czarno, bez dotykania ramki.
  const float BAT_V_PUSTA = 3.00;
  const float BAT_V_PELNA = 4.20;

  float procent = (napiecie - BAT_V_PUSTA) / (BAT_V_PELNA - BAT_V_PUSTA) * 100.0;
  if (procent < 0) procent = 0;
  if (procent > 100) procent = 100;

  uint16_t kolorWypelnienia;
  if (procent < 20) kolorWypelnienia = TFT_RED;
  else if (procent < 50) kolorWypelnienia = TFT_YELLOW;
  else kolorWypelnienia = TFT_GREEN;

  const int pad = 4;
  int innerW = BAT_W - pad * 2;
  int fillW = (int)(innerW * (procent / 100.0));

  if (fillW > 0) {
    tft.fillRect(BAT_X + pad, BAT_Y + pad, fillW, BAT_H - pad * 2, kolorWypelnienia);
  }
  if (fillW < innerW) {
    tft.fillRect(BAT_X + pad + fillW, BAT_Y + pad, innerW - fillW, BAT_H - pad * 2, TFT_BLACK);
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(BAT_X + BAT_W + BAT_NUB_W + 12, BAT_Y + (BAT_H - 16) / 2);
  tft.printf("%3.0f%%", procent);
}

void rysujStopke() {
  tft.fillRect(0, STOPKA_Y, tft.width(), STOPKA_H, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(STOPKA_TEKST_X, STOPKA_TEKST_Y);
  String tekst;
  if (!sdDostepna) {
    tekst = "SD: BRAK KARTY";
  } else if (nazwaPlikuLogu.length() > 0) {
    tekst = "SD: OK  " + nazwaPlikuLogu;
  } else {
    tekst = "SD: OK  (brak aktywnego logu)";
  }
  tft.print(tekst);
}

void rysujCale() {
  rysujStatyczneElementy();
  rysujPomiary();
  rysujStatystykiTestu();
  rysujStatus();
  rysujBaterie();
  rysujPrzyciskStart();
  rysujStopke();
}

// ===================================================================
// Strona: szybki test kondycji
// ===================================================================
void rysujAkcjaKondycja() {
  uint16_t kolor;
  uint16_t kolorTekstu = TFT_BLACK; // granat jest za ciemny na czarny tekst - patrz nizej
  const char *linia1;
  const char *linia2 = "";

  switch (stanSzybki) {
    case ST_GOTOWY:
      kolor = TFT_GREEN;
      linia1 = "ROZPOCZNIJ TEST";
      break;
    case ST_CZEKA_BEZ_OBCIAZENIA:
      kolor = TFT_YELLOW;
      linia1 = "ODLACZ REZYSTOR OD MASY";
      linia2 = "dotknij, gdy gotowe";
      break;
    case ST_CZEKA_POD_OBCIAZENIEM:
      kolor = TFT_YELLOW;
      linia1 = "PODLACZ REZYSTOR Z POWROTEM";
      linia2 = "dotknij, gdy gotowe";
      break;
    default: // ST_WYNIK
      kolor = TFT_NAVY;
      kolorTekstu = TFT_WHITE;
      linia1 = "NOWY TEST";
      break;
  }

  tft.fillRect(btnAkcjaKondycja.x, btnAkcjaKondycja.y, btnAkcjaKondycja.w, btnAkcjaKondycja.h, kolor);
  tft.drawRect(btnAkcjaKondycja.x, btnAkcjaKondycja.y, btnAkcjaKondycja.w, btnAkcjaKondycja.h, TFT_WHITE);
  tft.setTextColor(kolorTekstu, kolor);
  tft.setTextSize(2);

  int dlugoscL2 = strlen(linia2);
  int szer1 = strlen(linia1) * 12;
  int tx1 = btnAkcjaKondycja.x + (btnAkcjaKondycja.w - szer1) / 2;
  if (tx1 < btnAkcjaKondycja.x + 5) tx1 = btnAkcjaKondycja.x + 5;
  tft.setCursor(tx1, btnAkcjaKondycja.y + (dlugoscL2 ? 18 : 30));
  tft.print(linia1);

  if (dlugoscL2) {
    tft.setTextSize(1);
    int szer2 = dlugoscL2 * 6;
    int tx2 = btnAkcjaKondycja.x + (btnAkcjaKondycja.w - szer2) / 2;
    if (tx2 < btnAkcjaKondycja.x + 5) tx2 = btnAkcjaKondycja.x + 5;
    tft.setCursor(tx2, btnAkcjaKondycja.y + 48);
    tft.print(linia2);
  }
}

void rysujOdczytZywyKondycja() {
  if (stanSzybki != ST_CZEKA_BEZ_OBCIAZENIA && stanSzybki != ST_CZEKA_POD_OBCIAZENIEM) return;

  float busV = ina219Ok ? ina219.getBusVoltage_V() : 0;
  float shuntMv = ina219Ok ? ina219.getShuntVoltage_mV() : 0;
  float v = busV + (shuntMv / 1000.0);
  float i = ina219Ok ? odczytajPradMa() : 0;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(KONDYCJA_ZYWO_X, KONDYCJA_ZYWO_Y);
  tft.printf("Na zywo: %5.2fV  %4.0fmA", v, i);
}

void rysujWynikKondycja() {
  tft.fillRect(0, KONDYCJA_WYNIK_Y, tft.width(), KONDYCJA_WYNIK_H, TFT_BLACK);
  if (stanSzybki != ST_WYNIK) return;

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(KONDYCJA_WYNIK_X, KONDYCJA_WYNIK_L1_Y);
  tft.printf("Bez obciazenia:   %5.2f V", vOtwarte);
  tft.setCursor(KONDYCJA_WYNIK_X, KONDYCJA_WYNIK_L2_Y);
  tft.printf("Pod obciazeniem:  %5.2f V   %4.0f mA", vObciazone, iObciazone);

  tft.setCursor(KONDYCJA_WYNIK_X, KONDYCJA_WYNIK_L3_Y);
  if (rezystancjaWewnetrzna < 0) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.print("Za maly prad - sprawdz rezystor");
  } else {
    uint16_t kolorOceny;
    const char *ocena;
    if (rezystancjaWewnetrzna < 100) { kolorOceny = TFT_GREEN; ocena = "DOBRA"; }
    else if (rezystancjaWewnetrzna < 250) { kolorOceny = TFT_YELLOW; ocena = "SREDNIA"; }
    else { kolorOceny = TFT_RED; ocena = "SLABA"; }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.printf("Rez. wewn.: %.0f mOhm  ", rezystancjaWewnetrzna);
    tft.setTextColor(kolorOceny, TFT_BLACK);
    tft.print(ocena);
  }
}

void rysujStronaKondycja() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(KONDYCJA_TYTUL_X, KONDYCJA_TYTUL_Y);
  tft.print("Test kondycji baterii");

  tft.fillRect(btnPowrot.x, btnPowrot.y, btnPowrot.w, btnPowrot.h, TFT_NAVY);
  tft.drawRect(btnPowrot.x, btnPowrot.y, btnPowrot.w, btnPowrot.h, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(1);
  tft.setCursor(btnPowrot.x + 8, btnPowrot.y + 6);
  tft.print("< Powrot");

  tft.drawFastHLine(0, KONDYCJA_LINIA_Y, tft.width(), TFT_DARKGREY);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(KONDYCJA_OPIS_X, KONDYCJA_OPIS1_Y);
  tft.print("Krotka proba obciazenia zamiast wielogodzinnego rozladowania -");
  tft.setCursor(KONDYCJA_OPIS_X, KONDYCJA_OPIS2_Y);
  tft.print("licza sie rezystancje wewnetrzna ogniwa. Wynik porownawczy.");

  rysujAkcjaKondycja();
  rysujOdczytZywyKondycja();
  rysujWynikKondycja();
}

// ===================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  tft.init();
  tft.setRotation(1);
  tft.setTouchCalibrate(calData);
  tft.setBrightness(200);
  tft.fillScreen(TFT_BLACK);

  Wire.begin(I2C_SDA, I2C_SCL);
  ina219Ok = ina219.begin();

  inicjalizujSD();

  rysujCale();
}

void loop() {
  int32_t x, y;
  bool dotkniecie = pobierzDotyk(x, y) && (millis() - ostatniDotyk > 300);

  if (dotkniecie) {
    ostatniDotyk = millis();

    if (aktualnaStrona == STR_GLOWNA) {
      if (wPrzycisku(x, y, btnMinus)) {
        progOdciecia -= 0.05;
        if (progOdciecia < PROG_MIN) progOdciecia = PROG_MIN;
        rysujPrzyciskProg();
      } else if (wPrzycisku(x, y, btnPlus)) {
        progOdciecia += 0.05;
        if (progOdciecia > PROG_MAX) progOdciecia = PROG_MAX;
        rysujPrzyciskProg();
      } else if (wPrzycisku(x, y, btnKondyckaDotyk)) {
        aktualnaStrona = STR_KONDYCJA;
        stanSzybki = ST_GOTOWY;
        rysujStronaKondycja();
      } else if (wPrzycisku(x, y, btnStart)) {
        if (stan == CZEKA) {
          rozpocznijTest();
        } else if (stan == W_TRAKCIE) {
          zakonczTest();
        } else if (stan == ZAKONCZONY) {
          resetujDoGotowosci();
        }
        rysujCale();
      }
    } else { // STR_KONDYCJA
      if (wPrzycisku(x, y, btnPowrotDotyk)) {
        aktualnaStrona = STR_GLOWNA;
        rysujCale();
      } else if (wPrzycisku(x, y, btnAkcjaKondycja)) {
        if (stanSzybki == ST_GOTOWY) {
          stanSzybki = ST_CZEKA_BEZ_OBCIAZENIA;
        } else if (stanSzybki == ST_CZEKA_BEZ_OBCIAZENIA) {
          zmierzSrednio(vOtwarte, iOtwarte, 8, 40);
          stanSzybki = ST_CZEKA_POD_OBCIAZENIEM;
        } else if (stanSzybki == ST_CZEKA_POD_OBCIAZENIEM) {
          zmierzSrednio(vObciazone, iObciazone, 8, 40);
          obliczRezystancjeWewnetrzna();
          stanSzybki = ST_WYNIK;
        } else { // ST_WYNIK
          stanSzybki = ST_GOTOWY;
        }
        rysujStronaKondycja();
      }
    }
  }

  // Pomiar i odswiezanie na zywo co ok. 300 ms (bez migotania - patrz
  // komentarz w projekcie stacji pogody: stala szerokosc pola zamiast
  // fillRect przed kazdym printf). aktualizujPomiar() dziala zawsze w
  // tle, niezaleznie od strony, zeby dlugi test rozladowania na
  // stronie glownej nie przestal liczyc pojemnosci/logowac, kiedy
  // jestes na stronie kondycji.
  if (millis() - ostatnieOdswiezenie > 300) {
    ostatnieOdswiezenie = millis();
    aktualizujPomiar();

    if (aktualnaStrona == STR_GLOWNA) {
      rysujPomiary();
      rysujStatystykiTestu();
      rysujBaterie();
      if (stan == ZAKONCZONY) rysujStatus(); // upewnij sie ze komunikat konca zostaje widoczny
    } else {
      rysujOdczytZywyKondycja();
    }
  }
}
