[README_1.md](https://github.com/user-attachments/files/32343190/README_1.md)
# Tester baterii 18650 (ESP32)

Prosty tester ogniw 18650 na ESP32 z dotykowym wyświetlaczem TFT. Pozwala zrobić pełny test rozładowania z pomiarem pojemności i energii, albo szybki (kilkusekundowy) test kondycji ogniwa na podstawie rezystancji wewnętrznej.

## Funkcje

- Pomiar napięcia, prądu i mocy na żywo (czujnik INA219).
- Pełny test rozładowania: start/stop dotykiem, zliczanie pojemności (mAh) i energii (Wh), automatyczne zatrzymanie przy ustawionym napięciu odcięcia (domyślnie 3,00 V, regulowane na ekranie w krokach 0,05 V).
- Log CSV na karcie SD (czas, napięcie, prąd, moc, pojemność, energia) - osobny plik dla każdego testu.
- Szybki test kondycji baterii - w kilka sekund liczy przybliżoną rezystancję wewnętrzną ogniwa (na podstawie różnicy napięcia z obciążeniem i bez) i daje ocenę DOBRA / SREDNIA / SLABA. Przydatny, gdy nie ma czasu na wielogodzinny test rozładowania.
- Graficzny wskaźnik naładowania (pasek + %) liczony z bieżącego napięcia.
- Obsługa dotyku z powiększonym polem wykrywania przy przyciskach blisko krawędzi ekranu (kompensacja słabszej kalibracji dotyku w rogach).
- Wszystkie pozycje i rozmiary elementów na ekranie wydzielone do osobnego pliku (`ustawienia.h`), żeby można było poprawić układ bez grzebania w reszcie kodu.

## Sprzęt

- ESP32 (dowolna płytka deweloperska).
- Wyświetlacz TFT 3.5" 480x320, sterownik ILI9488, z dotykiem rezystancyjnym XPT2046.
- Czujnik prądu/napięcia INA219 (bocznik R100 = 0,1 Ω).
- Rezystor mocy jako obciążenie testowe (np. 8 Ω / 10 W) + przełącznik do ręcznego odłączania obciążenia.
- Karta microSD (opcjonalnie - bez niej program działa, tylko bez logowania).
- Uchwyt/gniazdo do ogniwa 18650.

Obciążenie jest podłączane/odłączane ręcznie - program tylko informuje na ekranie, kiedy to zrobić (nie ma tu przekaźnika sterującego obciążeniem).

## Podłączenie

### TFT (magistrala VSPI)

| Sygnał | GPIO |
|---|---|
| CS | 5 |
| DC | 2 |
| RST | 4 |
| MOSI | 23 |
| MISO | 19 |
| SCLK | 18 |
| BL (podświetlenie) | 32 |

### Dotyk XPT2046 (osobna magistrala HSPI)

| Sygnał | GPIO |
|---|---|
| CS | 15 |
| MOSI | 13 |
| MISO | 12 |
| SCLK | 14 |

### Karta SD (dzieli SCK/MOSI/MISO z TFT na VSPI)

| Sygnał | GPIO |
|---|---|
| CS | 26 |

### INA219 (I2C)

| Sygnał | GPIO |
|---|---|
| SDA | 21 |
| SCL | 22 |

VCC -> 3V3, GND -> GND.

### Ścieżka prądowa INA219

`VIN+` i `VIN-` to wejścia pomiarowe **w serii z obciążeniem**, a nie zwykłe zasilanie:

- Plus baterii -> `VIN+`
- `VIN-` -> obciążenie (rezystor + przełącznik)
- Wspólny minus baterii i obciążenia -> `GND`

`VIN-` nigdy nie powinien być podpięty bezpośrednio do GND - to zwarłoby bocznik i pomiar prądu nie działałby poprawnie.

## Wymagane biblioteki (Arduino Library Manager)

- **LovyanGFX**
- **Adafruit INA219** (razem z zależnością Adafruit BusIO)

Reszta (`Wire`, `SPI`, `SD`) jest wbudowana w rdzeń ESP32.

## Instalacja

1. Sklonuj/pobierz to repozytorium.
2. Otwórz `tester_baterii_ina219.ino` w Arduino IDE - `ustawienia.h` powinien pojawić się automatycznie jako druga zakładka (musi leżeć w tym samym folderze co plik `.ino`).
3. Zainstaluj biblioteki wymienione wyżej.
4. Wybierz płytkę ESP32 i port, wgraj szkic.

## Użycie

### Ekran główny - test rozładowania

- **Prog odciecia** (+/-) - napięcie, przy którym test zatrzyma się automatycznie (2,50-4,20 V).
- **START TESTU** - rozpoczyna test, zlicza pojemność/energię i loguje na SD (jeśli karta jest dostępna). Przycisk zmienia się w **ZATRZYMAJ TEST**.
- Po zakończeniu (ręcznym albo automatycznym po osiągnięciu progu) przycisk zmienia się w **DOTKNIJ, BY ZRESETOWAC**.
- Pasek baterii pod wskaźnikami pokazuje orientacyjny poziom naładowania na podstawie bieżącego napięcia (umowna krzywa ogniwa 18650: 3,00 V = 0%, 4,20 V = 100%). Pod obciążeniem, w trakcie testu, napięcie siada, więc pasek pokaże wtedy mniej niż realny stan naładowania - to normalne.
- **Test kondycji >** przenosi na drugi ekran.

### Test kondycji baterii (szybki test)

Alternatywa dla wielogodzinnego testu rozładowania, gdy chcesz szybko porównać kondycję ogniw. Krok po kroku, sterowane jednym dużym przyciskiem:

1. **ROZPOCZNIJ TEST**
2. **ODLACZ REZYSTOR OD MASY** - odłącz ręcznie obciążenie, dotknij, gdy gotowe (pomiar napięcia bez obciążenia).
3. **PODLACZ REZYSTOR Z POWROTEM** - podłącz obciążenie z powrotem, dotknij, gdy gotowe (pomiar napięcia i prądu pod obciążeniem).
4. Wynik: przybliżona rezystancja wewnętrzna ogniwa w mΩ i ocena **DOBRA** (< 100 mΩ) / **SREDNIA** (100-250 mΩ) / **SLABA** (> 250 mΩ).

To pomiar porównawczy (między ogniwami, w czasie), a nie laboratoryjny - obejmuje też rezystancję przewodów i styków.

## Kalibracja prądu

Tanie moduły INA219 często mają bocznik o innej rzeczywistej wartości niż zakładane w bibliotece 0,1 Ω, przez co prąd/moc są zawyżone. W kodzie jest stała korekcyjna:

```cpp
const float KOREKTA_PRADU = 500.0 / 622.0; // ~0.804
```

Wyznaczona przez porównanie z multimetrem podłączonym w serii z obciążeniem (multimetr pokazywał 500 mA, moduł 622 mA). Jeśli wymienisz moduł INA219 albo bocznik, zmierz ponownie prąd multimetrem przy dowolnym obciążeniu i podmień tę stałą na `(wartosc_z_multimetra / wartosc_pokazana_przez_program)`.

## Zmiana układu ekranu

Wszystkie pozycje, rozmiary i odstępy elementów na ekranie są w `ustawienia.h`, jako stałe `#define`. Większość pozycji liczy się automatycznie względem poprzedniego elementu (`pozycja = koniec poprzedniego + odstęp GAP_...`), więc zmiana jednego odstępu przesuwa wszystko poniżej, bez potrzeby przeliczania reszty. Plik ma na górze komentarz z pełnym wyjaśnieniem tej zasady.

## Pliki

| Plik | Opis |
|---|---|
| `tester_baterii_ina219.ino` | Główny kod programu |
| `ustawienia.h` | Pozycje i rozmiary elementów na ekranie |

## Uwagi

- Sprzęt (TFT + dotyk + SD) skonfigurowany tak samo, jak w innym projekcie na tym samym ESP32 (stacja pogody) - potwierdzone działające ustawienia LovyanGFX.
- Kalibracja dotyku (`calData`) jest specyficzna dla konkretnego egzemplarza wyświetlacza - jeśli dotyk jest niedokładny na innym module, trzeba ją wyznaczyć ponownie.
