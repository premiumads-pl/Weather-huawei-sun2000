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
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs,PSRAM=enabled" .
```

Required: `arduino-cli`, board core `esp32:esp32` (CI pins `3.3.10`),
libraries `TFT_eSPI`, `ArduinoJson`, `PNGdec`.

**Use that FQBN exactly.** It is the same string as in `tools/release.sh` and
`.github/workflows/build.yml`. Nothing else needs to be passed on the command
line: everything that shapes the binary is either in the FQBN or in a file in
this repo (see `build_opt.h` below). If you ever find yourself needing an extra
`--build-property` to reproduce a release, that's a bug in this repo, not in
your setup — please open an issue.

Every push and pull request runs the same compile in
[`.github/workflows/build.yml`](.github/workflows/build.yml), so you'll get
a red X if something doesn't build — no need to have the hardware to find
out.

## `build_opt.h` — don't delete it, it's worth 95 KB of flash

The repo contains a two-line `build_opt.h`:

```
-fno-exceptions
-DPATRZ_CONTRIBUTING_MD_SEKCJA_BUILD_OPT_H_ZANIM_USUNIESZ_FNO_EXCEPTIONS
```

**What it is.** `build_opt.h` is an arduino-esp32 convention. Despite the `.h`
name it is *not* a header — the core's `platform.txt` copies it into the build
directory and passes it to the compiler as a GCC **response file**
(`@build_opt.h`), i.e. its contents are spliced in as extra command-line
arguments. This is why it has no comments: a response file has no comment
syntax, so a `#` or `//` line would be parsed as a bogus argument and break the
build. Hence the shouty `-D` on line 2 — it is a no-op macro that exists purely
so that whoever opens this file finds their way here. That's also why this
explanation lives in CONTRIBUTING.md rather than in the file itself.

**Why `-fno-exceptions`.** The esp32 core compiles everything with
`-fexceptions` by default (it's in the core's `flags/cpp_flags`). We don't use
exceptions anywhere, but the flag still makes GCC emit unwind tables and
exception-handling paths for every translation unit built from source — the
sketch, the Arduino core, TFT_eSPI, ArduinoJson, PNGdec, PubSubClient. Turning
it off is worth **95,608 bytes of flash**, measured on 17.07.2026 as the
difference between two `--clean` builds of the same tree, changing nothing but
this file:

```
without build_opt.h   1,771,806 B  (90%)
with build_opt.h      1,676,198 B  (85%)   -> -95,608 B
```

Static RAM is unaffected (73,040 B either way) — this buys flash, not RAM.

**Why a file and not a build flag.** Because it is a *compiler* flag, the core
picks it up automatically from the sketch folder. Local builds, `release.sh`,
and CI therefore all get it for free, with no flag to remember and no way for
the three to drift apart. One source of truth.

**How to check it's still safe.** `-fno-exceptions` makes `try`/`catch`/`throw`
a **compile error**, so the check is simply: *does the build pass?* If it does,
nothing in the tree uses exceptions and the flag costs us nothing. If a PR ever
adds a library that needs them, CI will fail loudly on that PR with a clear
error — it cannot fail silently at runtime. That's the whole safety argument.

Note the precompiled ESP-IDF blobs shipped with the core *are* built with
exceptions on (`CONFIG_COMPILER_CXX_EXCEPTIONS=y`), and mixing is fine: they
keep their own unwind tables, our translation units simply don't generate any.
Behaviour on a hypothetical `std::bad_alloc` from `operator new` is unchanged —
there was no `catch` for it before either, so it ended in `terminate()`/reboot
then and it does now.

**If you must remove it,** expect the binary to grow by ~95 KB and re-check it
still fits the 1,966,080 B app partition before releasing.

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

This board **does** have PSRAM — 2 MB of it, measured on the live device
(`/api/diag` → `"psram": 2097152`). An earlier version of this file claimed
otherwise; it was wrong, and the FQBN above says `PSRAM=enabled`.

That does **not** relax the ceiling, and here is the part worth
understanding: the 129 KB screen sprite lives in PSRAM, but **not because
TFT_eSPI puts it there**. Its `#if defined(CONFIG_SPIRAM_SUPPORT)` never
fires — core 3.x defines `CONFIG_SPIRAM`, not `CONFIG_SPIRAM_SUPPORT`. The
sprite lands in PSRAM through `CONFIG_SPIRAM_USE_MALLOC=1` combined with
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`: every allocation above 4 KB is
routed there automatically. Chasing that mechanism cost us a release —
see `audyt/SESJA-2026-07-15.md`.

**PSRAM buys you nothing for static globals.** They live in internal SRAM,
and so does the heap that TLS needs. Measured on the device: during an OTA
download the free heap drops to **1444 bytes**. That is the real reason for
the ceiling — grow static RAM and the device loses the ability to fetch its
own update, permanently, on a wall you cannot reach.

`tools/release.sh` refuses to publish a release if the compiler reports more
than **76000 bytes** of static (global) RAM usage, and CI enforces the
identical check on every PR:

```bash
# what both tools/release.sh and CI do, conceptually
STATIC=$(grep -iE 'dynamic|dynamicznej' build.log |
         grep -oE '[0-9]+ (bytes|bajt)' | grep -oE '^[0-9]+' | head -1)
[ -n "$STATIC" ] && [ "$STATIC" -gt 0 ] || exit 1   # unreadable => STOP, never pass
[ "$STATIC" -le 76000 ]                            # else fail
```

Two details in there are load-bearing. The pattern is loose because
`Global variables use` only matches English — a differently localised
`arduino-cli` would make it match nothing. And an unreadable value must
**stop** the release, not read as zero: `0 -le 76000` is true, so a barrier
that can't parse its input silently waves everything through.

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
