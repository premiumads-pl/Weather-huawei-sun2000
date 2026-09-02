cmake_minimum_required(VERSION 3.14)
project(pogoda_gdynia_host_tests CXX)

# Faza 1: testy jednostkowe URUCHAMIANE NA HOSCIE (zwykly kompilator systemowy),
# NIE arduino-cli, NIE ESP32. Cel: siatka testow pod czysta logika z naglowkow
# katalogu glownego, ktora juz dzis kompiluje sie na hoscie bez zadnych zmian
# w kodzie produkcyjnym. Zero plikow .h/.cpp w katalogu glownym jest tu
# dotykane — tylko #include z include_directories ponizej.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
# Windows: nie nadpisuj ustawien CRT nadrzednego projektu. Nieszkodliwe na
# macOS/Linux, ale trzyma konfiguracje przenosna na wypadek innego runnera CI.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(host_tests
  test_pv_meter.cpp
  test_pv_history.cpp
  test_room_history.cpp
  test_air_history.cpp
  test_cost_data.cpp
  test_auto_data.cpp
  test_graph_blob.cpp
  test_payback_hist.cpp
  test_format.cpp
  test_weather_data.cpp
  test_backoff_pattern.cpp
)

# Katalog glowny repo (jeden poziom wyzej niz test/) — stad testy wciagaja
# nagłówki produkcyjne (PvData.h, RoomHistory.h, ...) bez zadnej kopii.
target_include_directories(host_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(host_tests PRIVATE GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(host_tests)
