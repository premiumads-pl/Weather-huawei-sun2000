#!/usr/bin/env bash
# Buduje firmware, podnosi numer wersji i publikuje GitHub Release.
# Urządzenie sprawdza .../releases/latest/download/version.json co 15 minut
# i samo się aktualizuje.
#
#   ./tools/release.sh "opis zmian"

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs,PSRAM=enabled"
NOTES="${1:-Aktualizacja}"

# --- podnieś numer wersji ---
CUR=$(grep -oE '#define FW_VERSION [0-9]+' Version.h | grep -oE '[0-9]+')
NEW=$((CUR + 1))
sed -i '' "s/#define FW_VERSION ${CUR}/#define FW_VERSION ${NEW}/" Version.h
echo "==> wersja ${CUR} -> ${NEW}"

# --- kompilacja ---
# Bez zadnych --build-property. Jedyna nadpisana flaga kompilatora
# (-fno-exceptions, ~95 kB flasha) siedzi w build_opt.h w katalogu szkicu i rdzen
# esp32 dociaga ja sam — tak samo tutaj, w CI i u kogos, kto buduje wg README.
# Jesli kiedys pojawi sie tu flaga, ktorej nie ma w README i .github/workflows/,
# to od tej chwili wydajemy inna binarke niz ta, ktora ludzie moga odtworzyc.
# Patrz CONTRIBUTING.md, sekcja o build_opt.h.
echo "==> kompiluje..."
rm -rf build
TMPDIR=/tmp arduino-cli compile --fqbn "$FQBN" --output-dir build . 2>&1 | tee build.log

# --- BARIERA RAM ---
# Bufor ekranu to 150 kB. Jeśli statyczny RAM przekroczy ~75 kB, na stertę nie
# zostaje dość miejsca na TLS i urządzenie przestaje umieć cokolwiek pobrać —
# łącznie z własną aktualizacją. Dokładnie tak zabiłem v14 (PNGdec: +47 kB).
# Odporne na jezyk arduino-cli. Linia RAM zawiera "dynamic"/"dynamicznej" w obu
# jezykach; bierzemy pierwsza liczbe przed bytes/bajt (= zmienne globalne).
#
# UWAGA — sprostowanie, zeby ktos tego nie cofnal w dobrej wierze:
# stary wzorzec 'Global variables use' NIE byl zepsuty na maszynie, na ktorej to repo
# jest wydawane. Sprawdzone 16.07.2026: arduino-cli mowi tam po ANGIELSKU i stary grep
# trafial poprawnie. Bariera nie byla slepa i v100 NIE przeszlo z zerem — takie
# twierdzenie pojawilo sie w pierwszej wersji tego komentarza i bylo bledne (autor
# zdiagnozowal wlasne, polskojezyczne srodowisko i przypisal to wydaniu).
# Zmiana zostaje, bo jest OBRONA NA PRZYSZLOSC: wystarczy inna lokalizacja, inna wersja
# arduino-cli albo zmiana formatu wyjscia, zeby wzorzec przestal trafiac. A wtedy
# STATIC=0 i warunek "0 > 76000" przepuscilby dowolnie duzy firmware — czyli bariera
# umarlaby po cichu. Stad dwie rzeczy naraz: luzniejszy wzorzec I bezpiecznik nizej.
STATIC=$(grep -iE 'dynamic|dynamicznej' build.log | grep -oE '[0-9]+ (bytes|bajt)' | grep -oE '^[0-9]+' | head -1 || true)
if [ -z "$STATIC" ]; then STATIC=0; fi   # pusty wynik (zmiana formatu) => 0 => bezpiecznik nizej
LIMIT=76000
echo "==> statyczny RAM: ${STATIC} B (limit ${LIMIT} B)"
# Bezpiecznik: STATIC=0 znaczy "nie odczytalem", NIE "zero RAM". Bez tego bariera
# jest slepa przy kazdej zmianie formatu/jezyka wyjscia. Zatrzymujemy i cofamy wersje.
if [ "$STATIC" -eq 0 ]; then
  echo ""
  echo "!!! STOP: nie odczytalem statycznego RAM z build.log (format/jezyk arduino-cli?)."
  echo "!!! Bariera bylaby slepa — nie ryzykuje wydania. Cofam podniesienie wersji."
  sed -i '' "s/#define FW_VERSION ${NEW}/#define FW_VERSION ${CUR}/" Version.h
  rm -f build.log
  exit 1
fi
if [ "$STATIC" -gt "$LIMIT" ]; then
  echo ""
  echo "!!! STOP: statyczny RAM ${STATIC} B > ${LIMIT} B."
  echo "!!! Przy tak małej stercie urządzenie nie zestawi TLS i NIE ZAKTUALIZUJE SIĘ"
  echo "!!! po sieci — wyjście tylko przez USB. Cofam podniesienie wersji."
  sed -i '' "s/#define FW_VERSION ${NEW}/#define FW_VERSION ${CUR}/" Version.h
  rm -f build.log
  exit 1
fi
rm -f build.log

# --- BARIERA 2: ADRES gPir W RTC ---------------------------------------------
# gPir trzyma statystyki PIR zbierane TYGODNIAMI i ma je przezyc OTA. Przezyje tylko
# wtedy, gdy nowa wersja szuka ich pod TYM SAMYM adresem, pod ktorym zapisala je stara.
#
# Problem: RTC_NOINIT_ATTR to _SECTION_ATTR_IMPL(".rtc_noinit", __COUNTER__) — kazda
# zmienna dostaje wlasna sekcje .rtc_noinit.<licznik>, a linker sklada je przez
# *(.rtc_noinit .rtc_noinit.*) BEZ SORT(). Czyli o adresie gPir decyduje KOLEJNOSC
# EMISJI SEKCJI przez GCC, a nie zaden kontrakt. Dolozenie drugiej zmiennej (gLdr w
# v108) potrafi przesunac gPir — i wtedy magic sie nie zgadza, kod robi zimny start
# i kasuje zbior. Na biurku wyglada to DOKLADNIE jak poprawny pierwszy rozruch.
# Awaria jest cicha. Ten bezpiecznik zamienia ja w glosna.
#
# 0x50000200 = _rtc_noinit_start. W v107 gPir byl jedyna zmienna rtc_noinit, wiec stal
# tam z koniecznosci; od v108 stoi tam, bo gLdr zadeklarowano NAD nim (GCC emituje
# sekcje w kolejnosci odwrotnej do deklaracji — zaobserwowane, nie gwarantowane).
#
# Jesli to STOPUJE, a Ty naprawde chcesz przeniesc gPir: pogodz sie ze strata zbioru
# albo zrob to porzadnie (jedna struktura z podstrukturami — ukladu pol WEWNATRZ
# struktury pilnuje ABI) i dopiero wtedy zmien ten adres tutaj.
NM=$(find "$HOME/Library/Arduino15/packages/esp32/tools" -name "xtensa-esp32s3-elf-nm" -type f 2>/dev/null | head -1)
ELF=$(find build -name "*.ino.elf" | head -1)
GPIR_WANT=50000200
if [ -z "$NM" ] || [ -z "$ELF" ]; then
  echo ""
  echo "!!! STOP: nie znalazlem nm ('${NM}') albo ELF-a ('${ELF}')."
  echo "!!! Bariera adresu gPir bylaby slepa — nie ryzykuje kasowania statystyk PIR."
  sed -i '' "s/#define FW_VERSION ${NEW}/#define FW_VERSION ${CUR}/" Version.h
  exit 1
fi
# Od v113 zbiorow w RTC jest WIECEJ NIZ JEDEN i kazdy trzyma wielotygodniowy pomiar:
#   gPir @ 0x50000200 — rytm doby, zbierany od 17.07, przezyl juz 9 restartow
#   gLdr @ 0x500002c0 — jasnosc i zdarzenia "zostawione swiatlo", od v108
# (gPvRtc @ 0x50000398 celowo NIE jest tu pilnowany: to najmlodszy zbior, stoi NAJWYZEJ,
# wiec dokladanie kolejnych zmiennych przesuwa jego, a nie tamtych dwoch. Gdy uzbiera
# dane warte ochrony — dopisz go do listy.)
# Sprawdzamy KAZDY: przesuniecie ktoregokolwiek kasuje jego zbior po cichu, bo magic
# sie nie zgodzi i kod zrobi "zimny start", nie do odroznienia od pierwszego uruchomienia.
RTC_FAIL=0
for PAIR in "gPir:50000200" "gLdr:500002c0"; do
  SYM="${PAIR%%:*}"; WANT="${PAIR##*:}"
  AT=$("$NM" "$ELF" | awk -v s="$SYM" '$3==s{print $1}' | head -1)
  if [ -z "$AT" ]; then AT="(brak symbolu)"; fi
  echo "==> ${SYM} @ 0x${AT} (oczekiwane 0x${WANT})"
  if [ "$AT" != "$WANT" ]; then
    echo "!!! ${SYM} PRZESUNIETY: 0x${AT} zamiast 0x${WANT}"
    RTC_FAIL=1
  fi
done
if [ "$RTC_FAIL" -ne 0 ]; then
  echo ""
  echo "!!! STOP: zmienna w RTC zmienila adres. OTA SKASUJE jej zbior i nie powie ani slowa"
  echo "!!! — na biurku wyglada to dokladnie jak poprawny pierwszy rozruch."
  echo "!!! Patrz komentarz nad ta bariera. Cofam podniesienie wersji."
  sed -i '' "s/#define FW_VERSION ${NEW}/#define FW_VERSION ${CUR}/" Version.h
  exit 1
fi

BIN=$(ls build/*.ino.bin | head -1)
cp "$BIN" build/firmware.bin
SIZE=$(stat -f%z build/firmware.bin)
SHA=$(shasum -a 256 build/firmware.bin | cut -d' ' -f1)
echo "==> firmware.bin: ${SIZE} B"

cat > build/version.json <<EOF
{
  "version": ${NEW},
  "size": ${SIZE},
  "sha256": "${SHA}",
  "notes": "${NOTES}"
}
EOF

# --- commit + tag + release ---
git add -A
git commit -m "v${NEW}: ${NOTES}" || echo "(brak zmian w kodzie)"
git push origin main

git tag -f "v${NEW}"
git push -f origin "v${NEW}"

gh release create "v${NEW}" \
  build/firmware.bin build/version.json \
  --title "v${NEW}" --notes "${NOTES}" --latest

echo ""
echo "==> Gotowe. Wyswietlacz pobierze v${NEW} w ciagu 15 minut."
