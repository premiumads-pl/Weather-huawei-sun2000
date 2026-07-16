#pragma once

#include <cstdint>
#include <pgmspace.h>

// Wygenerowane automatycznie (Natural Earth 10m land, rasteryzacja skanliniowa).
// SZEROKA mapa — 320 px, na caly ekran. Jedyny raster zatoki w projekcie: radar
// bierze ja w calosci, ekran samolotow jako wycinek (patrz gmapf nizej).
// Granice dobrane tak, zeby zachowac proporcje geograficzne: 349 m na piksel w obu osiach.
namespace gmapw {

constexpr float LAT_MIN = 54.3000f;
constexpr float LAT_MAX = 54.8400f;
constexpr float LON_MIN = 17.7350f;
constexpr float LON_MAX = 19.4650f;
constexpr int MAP_W = 320;
constexpr int MAP_H = 172;

// x0/x1 nie mieszcza sie juz w uint8_t (mapa ma 320 px) — stad uint16_t.
constexpr int SPAN_COUNT = 260;
static const uint16_t LAND_SPANS[260][2] PROGMEM = {
  { 77,107}, { 67,109}, { 57,111}, { 47,112}, { 37,114}, { 27,115}, { 24,117}, { 21,118},
  { 18,119}, { 15,121}, { 12,124}, {  9,126}, {  6,127}, {  3,129}, {  0,131}, {  0,133},
  {  0,129}, {133,134}, {  0,128}, {134,136}, {  0,128}, {135,138}, {  0,128}, {137,140},
  {  0,127}, {138,141}, {  0,127}, {139,143}, {  0,127}, {140,145}, {  0,126}, {141,146},
  {  0,126}, {143,148}, {  0,126}, {145,150}, {  0,125}, {148,152}, {  0,125}, {150,153},
  {  0,125}, {152,155}, {  0,125}, {153,157}, {  0,125}, {155,159}, {  0,125}, {156,160},
  {  0,125}, {158,162}, {  0,125}, {159,164}, {  0,125}, {160,166}, {  0,126}, {162,167},
  {  0,128}, {163,169}, {  0,130}, {166,171}, {  0,131}, {168,172}, {  0,132}, {169,174},
  {  0,132}, {169,176}, {  0,133}, {170,178}, {  0,134}, {171,179}, {  0,134}, {178,181},
  {  0,135}, {179,183}, {  0,135}, {180,185}, {  0,135}, {180,186}, {  0,136}, {181,188},
  {  0,136}, {181,189}, {  0,136}, {182,189}, {  0,136}, {183,190}, {  0,136}, {183,191},
  {  0,135}, {184,192}, {  0,135}, {184,192}, {  0,135}, {185,193}, {  0,136}, {186,194},
  {  0,137}, {186,195}, {  0,138}, {187,195}, {  0,138}, {187,196}, {  0,138}, {188,197},
  {  0,137}, {189,198}, {  0,137}, {145,145}, {190,198}, {  0,137}, {144,145}, {191,199},
  {  0,137}, {144,145}, {192,199}, {  0,138}, {143,144}, {192,200}, {  0,138}, {142,144},
  {193,200}, {  0,144}, {193,201}, {  0,144}, {194,201}, {  0,145}, {194,202}, {  0,145},
  {195,202}, {  0,146}, {196,203}, {  0,146}, {197,203}, {  0,147}, {198,203}, {  0,147},
  {198,203}, {  0,148}, {199,203}, {  0,148}, {199,203}, {  0,149}, {200,201}, {  0,149},
  {200,200}, {  0,149}, {  0,150}, {  0,150}, {  0,150}, {  0,151}, {  0,151}, {  0,151},
  {  0,152}, {  0,152}, {  0,152}, {  0,153}, {  0,153}, {  0,153}, {  0,154}, {  0,154},
  {  0,154}, {  0,154}, {  0,154}, {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153},
  {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153},
  {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,153}, {  0,154},
  {  0,154}, {  0,154}, {  0,154}, {  0,154}, {  0,154}, {  0,155}, {  0,155}, {  0,155},
  {  0,155}, {  0,156}, {  0,156}, {  0,157}, {  0,157}, {  0,160}, {  0,161}, {  0,163},
  {  0,164}, {  0,166}, {  0,169}, {  0,174}, {  0,175}, {  0,177}, {  0,177}, {319,319},
  {  0,178}, {317,319}, {  0,178}, {315,319}, {  0,179}, {313,319}, {  0,179}, {311,319},
  {  0,180}, {309,319}, {  0,181}, {307,319}, {  0,184}, {305,319}, {  0,187}, {303,319},
  {  0,189}, {296,319}, {  0,192}, {289,319}, {  0,195}, {282,319}, {  0,198}, {275,319},
  {  0,201}, {269,319}, {  0,204}, {225,225}, {262,319}, {  0,206}, {223,227}, {255,319},
  {  0,209}, {221,228}, {248,319}, {  0,229}, {241,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319}, {  0,319},
  {  0,319}, {  0,319}, {  0,319}, {  0,319},
};

static const uint16_t LAND_ROW_OFF[173] PROGMEM = {
     0,    1,    2,    3,    4,    5,    6,    7,    8,    9,   10,   11,
    12,   13,   14,   15,   16,   18,   20,   22,   24,   26,   28,   30,
    32,   34,   36,   38,   40,   42,   44,   46,   48,   50,   52,   54,
    56,   58,   60,   62,   64,   66,   68,   70,   72,   74,   76,   78,
    80,   82,   84,   86,   88,   90,   92,   94,   96,   98,  100,  102,
   104,  106,  109,  112,  115,  118,  121,  123,  125,  127,  129,  131,
   133,  135,  137,  139,  141,  143,  145,  146,  147,  148,  149,  150,
   151,  152,  153,  154,  155,  156,  157,  158,  159,  160,  161,  162,
   163,  164,  165,  166,  167,  168,  169,  170,  171,  172,  173,  174,
   175,  176,  177,  178,  179,  180,  181,  182,  183,  184,  185,  186,
   187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,
   199,  200,  201,  202,  203,  204,  205,  206,  208,  210,  212,  214,
   216,  218,  220,  222,  224,  226,  228,  230,  232,  234,  237,  240,
   243,  245,  246,  247,  248,  249,  250,  251,  252,  253,  254,  255,
   256,  257,  258,  259,  260,
};

}  // namespace gmapw

// Okno mapy dla ekranu SAMOLOTOW — wycinek rastra gmapw, bez wlasnych danych.
//
// Ekran samolotow dzieli szerokosc z lista lotow, wiec mapa ma 224 px, nie 320.
// Kiedys byl na to osobny raster (MapData.h, namespace gmap, LON 18.000-19.200).
// Mial 346.9 m/px przy 350.1 m/px gmapw — te same wody w dwoch skalach rozjezdzajacych
// sie o 1%. Teraz jest jeden raster, a ekran samolotow bierze z niego okno od piksela
// X_OFF. Granice LON licza sie z geometrii gmapw, wiec rzutowanie lon->x i raster
// NIE MOGA sie rozjechac — to jest cel tej konstrukcji, nie ozdoba.
//
// X_OFF = 49, bo (18.0000 - 17.7350) / 1.7300 * 320 = 49.02 px. Lewa krawedz okna
// wypada na LON 17.99991 — 0.02 px od dawnego 18.0000. Prawa siega 19.21091 zamiast
// 19.2000, czyli okno pokazuje 2 px WIECEJ na wschod niz stara mapa. Sprawdzone
// w float32: okno_x == gmapw_x - X_OFF co do piksela.
namespace gmapf {

constexpr int X_OFF = 49;              // przesuniecie okna w rastrze gmapw
constexpr int MAP_W = 224;             // szerokosc okna (reszta ekranu to lista lotow)
constexpr int MAP_H = gmapw::MAP_H;

constexpr float LON_PER_PX = (gmapw::LON_MAX - gmapw::LON_MIN) / gmapw::MAP_W;
constexpr float LON_MIN = gmapw::LON_MIN + X_OFF * LON_PER_PX;             // 17.99991
constexpr float LON_MAX = gmapw::LON_MIN + (X_OFF + MAP_W) * LON_PER_PX;   // 19.21091
constexpr float LAT_MIN = gmapw::LAT_MIN;
constexpr float LAT_MAX = gmapw::LAT_MAX;

static_assert(X_OFF + MAP_W <= gmapw::MAP_W, "okno gmapf wychodzi poza raster gmapw");

}  // namespace gmapf
