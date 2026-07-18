#pragma once

#include <cstdint>
#include <pgmspace.h>

// Wygenerowane automatycznie przez tools/gen_map.py z Natural Earth 10m land
// (rasteryzacja skanliniowa) - NIE EDYTOWAC RECZNIE. Zeby odtworzyc/zmienic:
//
//   python3 tools/gen_map.py --shapefile /tmp/ne/ne_10m_land/ne_10m_land.shp --center-lat 54.57 --center-lon 18.60 --width-km 300 --width 320 --height 172 --namespace gmapr --out MapDataRadar.h
//
// Zrodlo: https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-land/
// Granice dobrane pod izotropowa skale: 937.50 m/px poziomo, 937.55 m/px pionowo.
namespace gmapr {

constexpr float LAT_MIN = 53.8457f;
constexpr float LAT_MAX = 55.2943f;
constexpr float LON_MIN = 16.2756f;
constexpr float LON_MAX = 20.9244f;
constexpr int MAP_W = 320;
constexpr int MAP_H = 172;

// Recznie dopisane DO wygenerowanego pliku (ten sam wzorzec co gmapf w
// MapDataWide.h): NIE jest to dana zrodlowa z shapefile'a, tylko wartosc
// WYLICZONA z czterech stalych nad ta linia, ktore gen_map.py juz wygenerowal
// -- wiec ponowne uruchomienie komendy z naglowka pliku odtworzy identyczny
// wynik dla tej linii tez, o ile ktos ja przy okazji nie skasuje (gen_map.py
// jej dzis nie emituje samo, patrz uwaga w tools/gen_map.py).
//
// Uzywana przez WeatherUi::drawViewRadar do przeliczenia predkosci wiatru
// (km/h) na przesuniecie w pikselach tej mapy MIEDZY klatkami radaru. Do v109
// ta skala byla wpisana na sztywno w WeatherUi.cpp jako 349 m/px -- ale to byla
// skala gmapw (111 km szerokosci / 320 px). Ta mapa pokrywa ~300 km na tej
// samej liczbie pikseli (320), wiec KAZDY piksel odpowiada wiekszemu kawalkowi
// terenu -- konkretnie 300/111.7 =~ 2,69x wiecej. Zostawienie starej stalej
// 349 kazaloby chmurom "jechac" 2,69x za wolno w metrach/s, czyli na ekranie
// (gdzie krok liczony jest w PIKSELACH) front przesuwalby sie 2,69x ZA SZYBKO
// wzgledem tego, co realnie pokazuje kolejna klatka -- rozjazd rosnacy z kazda
// klatka animacji.
//
// Wzor: (LAT_MAX-LAT_MIN) w stopniach * 111320 m/stopien (przyblizenie sferyczne,
// jak w tools/gen_map.py/KM_PER_DEG_LAT) / MAP_H wierszy = m/px PIONOWO. Granice
// gmapr sa dobrane pod skale IZOTROPOWA (patrz naglowek pliku: 937.50 poziomo vs
// 937.55 pionowo -- roznica 0,05 m/px, promil), wiec jedna stala starcza dla
// obu osi interpolacji wiatru (ktora i tak liczy wspolna dlugosc wektora (dx,dy),
// patrz WeatherUi.cpp). Wychodzi ~937,5 m/px -- zweryfikowane numerycznie.
constexpr float M_PER_PX = (LAT_MAX - LAT_MIN) * 111320.f / MAP_H;

constexpr int SPAN_COUNT = 263;
static const uint16_t LAND_SPANS[263][2] PROGMEM = {
  {319,319}, {319,319}, {318,319}, {318,319}, {317,319}, {316,319}, {316,318}, {315,317},
  {315,317}, {314,317}, {314,316}, {313,315}, {312,314}, {311,314}, {310,314}, {309,313},
  {308,312}, {308,311}, {307,310}, {306,309}, {305,308}, {304,307}, {304,307}, {303,306},
  {302,305}, {301,304}, {301,304}, {300,303}, {298,301}, {297,300}, {296,299}, {295,298},
  {294,297}, {292,296}, {291,295}, {290,294}, {288,293}, {254,256}, {287,293}, {254,257},
  {261,263}, {267,275}, {285,293}, {253,294}, {305,310}, {252,312}, {252,313}, {252,314},
  {252,315}, {251,319}, {251,319}, {251,319}, {251,319}, {251,319}, {251,319}, {252,319},
  {252,319}, {125,141}, {252,319}, {114,142}, {253,319}, {109,144}, {253,319}, {106,146},
  {253,319}, {103,148}, {253,319}, {100,149}, {254,319}, { 96,148}, {151,151}, {254,319},
  { 90,148}, {152,153}, {254,319}, { 85,147}, {153,154}, {254,319}, { 80,147}, {154,156},
  {253,319}, { 74,147}, {157,158}, {253,319}, { 71,147}, {158,159}, {253,319}, { 69,147},
  {160,161}, {252,319}, { 67,148}, {161,163}, {252,319}, { 65,149}, {163,165}, {251,319},
  { 63,150}, {164,166}, {251,319}, { 61,150}, {167,168}, {250,319}, { 59,151}, {168,170},
  {250,319}, { 57,151}, {169,171}, {249,319}, { 54,151}, {169,171}, {250,319}, { 52,151},
  {170,172}, {249,319}, { 51,151}, {170,173}, {249,319}, { 50,151}, {171,174}, {248,319},
  { 50,151}, {154,154}, {172,174}, {247,319}, { 49,154}, {172,175}, {247,319}, { 48,154},
  {173,175}, {246,319}, { 47,154}, {174,176}, {246,319}, { 46,155}, {174,176}, {245,319},
  { 44,155}, {175,175}, {245,319}, { 39,156}, {244,319}, { 37,156}, {243,319}, { 34,156},
  {243,319}, { 27,157}, {242,319}, { 20,157}, {241,319}, { 19,157}, {240,319}, { 17,157},
  {239,319}, { 15,157}, {239,319}, { 15,157}, {238,319}, { 14,157}, {237,319}, { 13,157},
  {236,319}, { 12,157}, {235,319}, { 14,157}, {233,319}, { 11, 11}, { 14,157}, {232,319},
  { 10, 10}, { 13,157}, {231,319}, {  9,158}, {231,319}, {  8,158}, {229,319}, {  8,158},
  {228,319}, {  7,158}, {227,319}, {  6,160}, {226,319}, {  5,161}, {225,319}, {  5,163},
  {224,319}, {  4,166}, {222,319}, {  3,166}, {218,319}, {  2,167}, {216,319}, {  2,168},
  {214,319}, {  1,  1}, {  3,171}, {209,319}, {  1,  1}, {  4,174}, {202,319}, {  4,177},
  {184,184}, {196,319}, {  3,186}, {189,319}, {  1,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
};

static const uint16_t LAND_ROW_OFF[173] PROGMEM = {
    0,   0,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,
   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,
   22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,
   34,  35,  36,  37,  39,  43,  45,  46,  47,  48,  49,  50,
   51,  52,  53,  54,  55,  56,  57,  59,  61,  63,  65,  67,
   69,  72,  75,  78,  81,  84,  87,  90,  93,  96,  99, 102,
  105, 108, 111, 114, 117, 120, 124, 127, 130, 133, 136, 139,
  141, 143, 145, 147, 149, 151, 153, 155, 157, 159, 161, 163,
  165, 168, 171, 173, 175, 177, 179, 181, 183, 185, 187, 189,
  191, 193, 196, 199, 202, 204, 205, 206, 207, 208, 209, 210,
  211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222,
  223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234,
  235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246,
  247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258,
  259, 260, 261, 262, 263,
};

}  // namespace gmapr
