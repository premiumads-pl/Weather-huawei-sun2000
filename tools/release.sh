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
echo "==> kompiluje..."
rm -rf build
TMPDIR=/tmp arduino-cli compile --fqbn "$FQBN" --output-dir build . 2>&1 | tee build.log

# --- BARIERA RAM ---
# Bufor ekranu to 150 kB. Jeśli statyczny RAM przekroczy ~75 kB, na stertę nie
# zostaje dość miejsca na TLS i urządzenie przestaje umieć cokolwiek pobrać —
# łącznie z własną aktualizacją. Dokładnie tak zabiłem v14 (PNGdec: +47 kB).
STATIC=$(grep -oE 'Global variables use [0-9]+' build.log | grep -oE '[0-9]+' || echo 0)
LIMIT=76000
echo "==> statyczny RAM: ${STATIC} B (limit ${LIMIT} B)"
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
