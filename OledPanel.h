#pragma once

#include <cstdint>

#include "AutoData.h"

// Panel OLED 0,96" (SSD1306/SSD1315, 128x64, I2C) z czterema przyciskami — WYBOR
// TRYBU LADOWANIA AUTA. To DRUGIE URZADZENIE na tej samej plytce, a nie nowy widok
// glownego ekranu: ma wlasny sterownik, wlasny rytm rysowania i wlasne przyciski.
// Numerow widokow (cfg::VIEW_*) nie dotyka i dotykac nie ma po co.
//
// TRZY RZECZY, KTORE RZADZA CALYM TYM MODULEM
// -------------------------------------------
// 1. WLASNEGO BUFORA OBRAZU NIE MA. Pelna klatka SSD1306 to 128 x 64 / 8 = 1024 B,
//    a do bariery statycznego RAM-u (76 000 B, tools/release.sh) zostalo po v174
//    1816 B. Trzymanie tego kilobajta zjadloby 56% calego zapasu wydania.
//    Dlatego jedziemy w TRYBIE STRONICOWYM: bufor to JEDNA strona, 128 B, i osiem
//    przebiegow sklada pelna klatke. Z tego samego powodu odpada U8g2 w trybie
//    pelnego bufora i Adafruit_SSD1306 — obie alokuja ten kilobajt statycznie.
//    (v176) WYJATEK, KTORY REGULY NIE LAMIE: kopia obrazu dla podgladu WWW ma pelne
//    1024 B, ale siedzi w PSRAM (heap_caps_malloc, patrz shadow() nizej), wiec w
//    statycznym RAM-ie zostaje po niej SAM WSKAZNIK — 4 B, a nie kilobajt. Panel
//    dalej rysuje stronicowo; kopia tylko odklada te same bajty, ktore poszly na szklo.
// 2. RYSOWANIE NIE MOZE ZATRZYMAC GLOWNEGO EKRANU. Cala klatka to 1 kB po I2C,
//    czyli przy 400 kHz okolo 25 ms — POLOWA klatki ST7789. Dlatego step() robi
//    najwyzej JEDNA strone na obieg petli (~3 ms) i TYLKO wtedy, gdy tresc sie
//    zmienila. Osiem kolejnych obiegow sklada nowa klatke; przy braku zmian koszt
//    to sam odczyt czterech pinow.
// 3. PANEL NIE UDAJE, ZE COS USTAWIL. Kropka "tryb aktywny" pochodzi WYLACZNIE
//    z pola `tryb` w <prefix>/auto/stan. Zatwierdzenie publikuje polecenie i czeka;
//    bez potwierdzenia w cfg::OLED_CONFIRM_MS menu pisze o tym wprost.
//
// (v178) EKRAN TEST MA WYJSCIE POD PALCEM. Wchodzi sie w niego cfg::BTN_OK
// i cfg::BTN_BACK trzymanymi RAZEM przez cfg::OLED_TEST_HOLD_MS, a wychodzi
// KAZDYM krotkim nacisnieciem i puszczeniem dowolnego przycisku — patrz
// pollButtons() w OledPanel.cpp. Do v177 wejsciem bylo przytrzymanie DOWOLNEGO
// przycisku, a jedynym wyjsciem 10 s bezczynnosci, i to byla pulapka bez klamki:
// w tescie przyciski nic nie robia, wiec wlasciciel naciskal wiecej i mocniej,
// a kazde nacisniecie zerowalo licznik wyjscia. Skonczylo sie zgloszeniem, ze
// "przyciski sa zepsute i zmiana trybu nie dziala".
//
// (v187) MENU USTAWIEN — I JEDNA GRAMATYKA PRZYCISKOW NA KAZDYM POZIOMIE.
// Panel przestal byc jednoekranowym pilotem do auta i dostal drugie drzewo: cztery
// pozycje (Ekran / Jasność / Rotacja / Noc), a pod nimi podekrany. Zeby to dalo sie
// obsluzyc czterema guzikami BEZ INSTRUKCJI, obowiazuje jedna, wyjatkow nie ma:
//     ∧ ∨   poruszaj sie / zmieniaj wartosc
//     ✱     wejdz / zatwierdz        (cfg::BTN_OK)
//     #     wroc / anuluj            (cfg::BTN_BACK)
// "#" NIGDY nie robi nic innego niz KROK WSTECZ — ze spoczynku, gdzie cofac sie nie
// ma dokad, nie robi wiec NIC. To jest obietnica wobec wlasciciela, a nie detal
// implementacji: jedyny przycisk, ktorego mozna nacisnac na oslep i niczego nie
// zepsuc, przestaje byc taki w chwili, gdy gdziekolwiek zaczyna cos zatwierdzac.
// Ze spoczynku: ∧ albo ∨ -> wybor trybu ladowania (jak dotad, jedno nacisniecie),
// ✱ -> ustawienia, ✱ i # razem przez cfg::OLED_TEST_HOLD_MS -> ekran TEST.
//
// CZEGO W TYM MENU NIE MA I NIE BEDZIE: WiFi, MQTT, adresow, hasel i kluczy.
// Czterema przyciskami na 128x64 nie wpisuje sie hasla — probowanie tego skonczyloby
// sie ekranem wyboru znaku po znaku, ktory wyglada jak funkcja, a jest pulapka.
// To zostaje w panelu WWW, gdzie jest klawiatura.
//
// EDYCJA MA PODGLAD NA ZYWO I ODWROTNY BIEG. ✱ na wierszu wchodzi w edycje, ∧∨ zmieniaja
// wartosc W RAM-ie ustawien od razu (jasnosc realnie sie zmienia, czas ekranu realnie
// przyspiesza rotacje), ✱ zatwierdza i robi JEDEN zapis do NVS przez Settings::
// saveTuning(), a # przywraca wartosc sprzed wejscia w edycje. Bezczynnosc
// (cfg::OLED_SET_IDLE_MS) konczy sie tak samo jak # — bez zapisu.
namespace oled {

// Wykrywa modul po I2C (ACK pod 0x3C, potem 0x3D) i konfiguruje przyciski.
// BRAK MODULU NIE JEST BLEDEM: wlasciciel wgra firmware, zanim cokolwiek podlaczy.
// Nieudane wykrycie WYLACZA panel na cale zycie programu — step() wychodzi wtedy
// pierwsza linia, wiec nie ma ani ponawiania co petle, ani wiszacych transakcji I2C.
// Slad zostaje w dzienniku i w /api/diag (sekcja "oled").
void begin();

// (v187) TRZY WSKAZNIKI DO GLOWNEGO EKRANU — ustawiane RAZ, w setup().
// Menu ustawien (patrz nizej) musi umiec przypiac widok, zapytac o przypiety
// i wymusic jasnosc na czas podgladu. Wszystkie trzy rzeczy naleza do obiektu
// WeatherUi, ktory zyje w pogoda-gdynia.ino — a ten panel z zalozenia nie zna
// glownego ekranu i znac go nie ma po co. Wskazniki zamiast `extern WeatherUi ui`
// to dokladnie ten sam wzorzec, ktorym z panelem WWW rozmawia portal::
// setViewHandler(): jedno miejsce spiecia, w setup(), i zadnej zaleznosci w druga
// strone. Nieustawione (nullptr) nie sa bledem — podekran "Ekran" po prostu nic
// wtedy nie przypina, a podglad jasnosci ogranicza sie do zapisu w RAM.
//   pinFn(i)   — przypnij widok i (i < 0 zwalnia przypiecie, czyli wraca rotacja)
//   pinnedFn() — numer przypietego widoku albo -1; STAD bierze sie kropka na liscie
//   blFn(v,ms) — wymus jasnosc v na ms milisekund (WeatherUi::testBacklight)
void setUiHooks(void (*pinFn)(int), int (*pinnedFn)(), void (*blFn)(uint8_t, uint32_t));

// Jeden obieg: odczyt przyciskow, obsluga stanu, najwyzej JEDNA strona obrazu.
// Wolac z petli rysowania, po zlozeniu klatki glownego ekranu.
void step(const AutoModel& a, uint32_t now);

// --- podglad dla /api/diag (zero nowych pol w Diag, wiec zero bajtow w .bss) ---
bool present();            // czy modul odpowiedzial przy starcie
uint8_t address();         // 0x3C / 0x3D / 0 gdy nie ma
const char* screenName();  // "spoczynek" / "menu" / "test" / (v187) "ustawienia"
uint32_t pagesSent();      // ile stron poszlo na I2C od uruchomienia
uint32_t i2cErrors();      // ile transakcji NIE doszlo (urwany przewod, zly styk)
uint32_t lastStepUs();     // ile trwal ostatni obieg step() [us]
uint8_t buttonMask();      // bity 0..3 = K1..K4 wcisniete (po debounce)
const char* sentMode();    // ostatnio WYSLANY tryb ("" = nic nie wysylano)

// (v188) Ile probek ma wykres mocy ladowania w dolnym pasie ekranu spoczynkowego
// (0..128, jedna co 3 minuty). Odpowiada na dwa pytania naraz, ktorych bez kabla USB
// nie da sie rozstrzygnac inaczej: czy bufor w PSRAM w ogole powstal (przy nieudanej
// alokacji zawsze 0) i czy trwa sesja ladowania (liczba rosnie co 3 min). Zero przy
// dzialajacym buforze znaczy "od uruchomienia nie bylo ani jednej sesji".
uint8_t graphCount();

// --- (v176) PODGLAD I STEROWANIE Z PANELU WWW -------------------------------
// Strona ma pokazywac PIKSEL W PIKSEL to, co jest na szkle, i miec cztery klikalne
// przyciski robiace dokladnie to samo, co te na module. Wszystkie cztery funkcje
// wola ZADANIE SERWERA WWW, czyli INNE niz to, ktore rysuje panel — dlatego zadna
// z nich nie dotyka stanu ekranu bezposrednio.

// Kopia obrazu: 1024 B w PSRAM, uklad taki sam, jak w pamieci SSD1306 — osiem stron
// po 128 B, bajt = kolumna, bit = wiersz w obrebie strony (bit 0 = gorny).
// nullptr, gdy panelu nie ma albo gdy alokacja w PSRAM sie nie udala; to NIE jest
// blad — panel dziala dalej, po prostu podgladu nie ma i strona ma to obsluzyc.
// Kopia jest odswiezana STRONAMI, razem z wysylka na szklo, i tylko po UDANEJ
// transmisji, wiec przy urwanym przewodzie pokazuje to samo, co zostalo na szkle.
const uint8_t* shadow();

// Wirtualne nacisniecie przycisku. `role` to ROLA (cfg::BTN_UP / BTN_DOWN / BTN_OK /
// BTN_BACK), a NIE numer K na module — dzieki temu strona WWW nie musi wiedziec nic
// o tym, jak modul jest przykrecony. Poza zakresem 0..3 i przy niewykrytym panelu
// nie robi nic. Dziala jak KROTKIE nacisniecie i puszczenie POJEDYNCZEGO przycisku,
// wiec (v178) chwytu otwierajacego ekran TEST — cfg::BTN_OK i cfg::BTN_BACK trzymane
// RAZEM przez cfg::OLED_TEST_HOLD_MS — nie da sie tedy zlozyc.
// Sama akcja NIE wykonuje sie tutaj — patrz komentarz przy gInject w OledPanel.cpp.
void injectPress(uint8_t role);

// Podswietlony wiersz menu WYBORU TRYBU, 0..3 (to NIE jest tryb aktywny).
// (v187) Menu ustawien ma WLASNY kursor i celowo go tu nie wystawiamy: panel WWW
// rysuje ekran z kopii obrazu piksel w piksel (shadow()), wiec i tak widzi, gdzie
// stoi podswietlenie, a kolejna liczba w /api/diag musialaby znaczyc co innego na
// kazdym z pieciu ekranow ustawien.
uint8_t cursor();
const char* activeMode();  // tryb POTWIERDZONY przez <prefix>/auto/stan, "" gdy brak
                           // — to samo zrodlo, co kropka na ekranie, nigdy wlasna wysylka

}  // namespace oled
