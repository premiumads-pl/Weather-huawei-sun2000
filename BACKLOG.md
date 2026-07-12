# Backlog

This file used to hold the project's free-form TODO list. It has been
turned into tracked [GitHub Issues](../../issues) instead, so status,
discussion, and ownership don't get lost in a Markdown file nobody
re-reads. The full original text is still in git history
(`git log -p -- BACKLOG.md`) if you want the original notes verbatim.

What happened to each section:

- **Memory (two-band rendering, 132 KB → 66 KB buffer)** — tracked as an
  issue, currently the top engineering priority.
- **Remote diagnostics (`/api/log`, `/api/diag`, `/api/reboot`)** — this
  was already done (the old file said so in its own heading); it's just
  documented in the [README](README.md#remote-diagnostics) now instead of
  sitting in a TODO list.
- **"To fix by the user in the panel" (default location precision, PV
  peak-power value)** — intentionally **not** turned into public issues.
  Those were personal reminders for the device owner to correct their own
  configuration through the web panel (see README → Configuration), not
  engineering work for contributors, and one of them referenced an
  approximate home address — not something to restate in a public tracker.
- **Ideas to consider** (radar overlay on the flight map, rain trend,
  multi-day PV history, radar-triggered alert, skipping the PV screen at
  night, verifying PSRAM) — each is now its own issue.
- **Known limitations** (RainViewer zoom cap, single Modbus session,
  Open-Meteo being a model) — these are inherent constraints, not bugs to
  fix, so they live in the README's
  [Known limitations](README.md#known-limitations) section rather than the
  issue tracker.

See also the extra issues for things this GitHub cleanup pass didn't get
to: ESP Web Tools browser flashing, decoupling the inverter client behind
an interface, and MQTT / Home Assistant autodiscovery.
