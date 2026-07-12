# Contributing

Thanks for considering it — this is a small hobby project, but real PRs and
issues are welcome.

## Building

See the [README](README.md#flashing) for the full flashing instructions.
Short version:

```bash
# TFT_eSPI needs this repo's User_Setup.h instead of its own default one —
# copy it into the library's install directory, then:
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" .
```

Required: `arduino-cli`, board core `esp32:esp32` (CI pins `3.3.10`),
libraries `TFT_eSPI`, `ArduinoJson`, `PNGdec`.

Every push and pull request runs the same compile in
[`.github/workflows/build.yml`](.github/workflows/build.yml), so you'll get
a red X if something doesn't build — no need to have the hardware to find
out.

## Testing without the hardware

There's no host-side unit test harness (the codebase is Arduino/ESP-IDF
code, not easily built for a desktop target), so "testing" mostly means:

- **Compile-only sanity check.** `arduino-cli compile` (or just open a PR —
  CI does this automatically) catches most mistakes: type errors, missing
  includes, and — importantly — blowing the RAM ceiling (see below).
- **Display-only testing.** If you have an ESP32-S3 and an ST7789 wired up
  per the [README's pin table](README.md#wiring) but nothing else (no
  Wi-Fi credentials, no inverter, no sensors), flip `cfg::COLOR_TEST_MODE`
  to `true` in `Config.h`. It skips straight to a colour-bar test pattern
  with no network dependencies, which is enough to confirm wiring, SPI
  timing, and colour order (`TFT_BGR` / `TFT_INVERSION_OFF`) before you
  chase a "real" bug that's actually just a wiring problem.
- **Network clients are isolated classes.** `WeatherClient`, `PvClient`,
  `RadarClient`, and `FlightClient` each own one external integration and
  don't reach into UI code. If you're changing one of them, you can reason
  about it (and eventually add host-side tests) without touching the
  other three. `PvClient` in particular is a reasonable place to start if
  you're working on the "support other inverters" issue — see the open
  issues for context.
- **The real thing.** Ultimately this talks to live weather/radar/ADS-B
  APIs and a physical inverter, so some changes can only be verified on
  actual hardware. Say so in your PR description if you couldn't test a
  change end-to-end — that's fine, just flag it.

## The RAM ceiling — please don't skip this

This board has **no PSRAM** and a screen buffer that already eats a large
chunk of the ~320 KB of RAM. `tools/release.sh` refuses to publish a
release if the compiler reports more than **76000 bytes** of static
(global) RAM usage, and CI enforces the identical check on every PR:

```bash
# what both tools/release.sh and CI do, conceptually
STATIC=$(grep -oE 'Global variables use [0-9]+' build.log | grep -oE '[0-9]+')
[ "$STATIC" -le 76000 ]  # else fail
```

This isn't an arbitrary style rule — cross 76 KB of static RAM and there
isn't enough heap left for a TLS handshake, which means the device can no
longer check for or download its own OTA update. A firmware that can't
update itself over the air has to be recovered over USB, which defeats a
big part of the point of this project (see `v14` in the git history for
what happens when this goes wrong). If your change needs more static RAM,
the first questions to ask are "does this need to be a global/static
buffer?" and "can it be allocated only while in use, like `RadarClient`
does for its PNG decoder?".

## No secrets in the repository — ever

Wi-Fi credentials, the inverter's IP address, and location are runtime
configuration stored in the device's NVS flash, set through the web panel
or the serial console — never hardcoded, never committed. Concretely:

- Don't add real SSIDs, passwords, or IP addresses to any file, including
  comments, examples, or test fixtures.
- `.gitignore` already blocks `Secrets.h` / `secrets.*` as a last line of
  defense — don't work around it.
- If you need a placeholder for docs or examples, use something obviously
  fake (`your-network`, `192.168.1.x`, etc.), matching the existing style
  in `Config.h` and `Settings.h`.

## Style

- 2-space indentation, matching the existing code.
- Comments may be in Polish or English — the codebase is currently a mix
  (most existing comments are Polish); write in whichever you're
  comfortable with, don't feel obligated to translate existing comments in
  files you're touching for an unrelated change.
- Commit messages like `vNN: ...` are produced by `tools/release.sh` for
  release commits only — don't hand-write that format or bump
  `Version.h` yourself in a PR; the maintainer's release script owns
  version numbers.
- Keep PRs focused; if a change needs more RAM, more heap-fragmentation
  risk, or a new library, say so explicitly in the PR description.

## Where to start

Check the [issue tracker](../../issues), especially anything tagged
[`good first issue`](../../issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22).
