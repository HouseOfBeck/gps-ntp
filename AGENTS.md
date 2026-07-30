# AGENTS.md

This file defines the project constraints and working rules for Codex and other
automated contributors. It applies to the entire repository.

## Start Here

- Treat the repository root and the current `main` branch as authoritative.
- Before changing anything, inspect the repository tree, `git status`, and
  recent history. Do not assume paths or files from earlier sessions still
  exist.
- The project originally lived in `gps_uart_test/`, but that directory was
  removed. Sources now live under `main/`, and the application entry point is
  `main/main.c`.
- The initial known-good repository baseline is commit `6bd38ca` (`Initial
  GPS-disciplined ESP32 NTP server`). Do not reset current work to that commit;
  preserve subsequent intentional changes and all unrelated user work.
- Keep changes narrowly scoped. Do not modify functional source code for a
  documentation-only task unless a documentation-related build issue requires
  it.

## Platform and Dependencies

- Target: ESP32-S3 with 16 MB flash
- SDK: ESP-IDF v6.0.2
- Network interface: W5500 Ethernet only; do not add Wi-Fi
- GPS receiver: GT-U7 at 9600 baud with a separate PPS signal
- Display: SSD1306-compatible 128x64 OLED at I2C address `0x3C`
- Managed components:
  - `espressif/w5500` 2.0.0
  - `espressif/wiznet_common` 1.0.0

Use ESP-IDF v6.0.2 APIs. Prefer the modern ESP-IDF I2C master API for display
work.

## Fixed Pin Assignments

Do not change GPIO assignments unless the user explicitly requests it.

### GT-U7 GPS

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| GPS TX / ESP RX | 44 |
| GPS RX / ESP TX | 43 |
| PPS | 3 |
| VCC | 5 V |
| GND | GND |

### W5500 Ethernet

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| MOSI | 11 |
| MISO | 12 |
| CLK | 13 |
| CS | 14 |
| INT | 10 |
| RESET | 9 |

### OLED

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| SDA | 17 |
| SCL | 18 |
| VCC | 3.3 V |
| GND | GND |

### Reserved microSD Pins

The microSD interface is not currently used, but these pins are reserved.

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| MOSI | 6 |
| MISO | 5 |
| CLK | 7 |
| CS | 4 |

## Clock and NTP Correctness Invariants

Clock correctness has priority over display behavior and nonessential
diagnostics.

- Only the `SYNCED` state may advertise Stratum 1.
- `UNSYNC`, `ACQUIRING`, `HOLDOVER`, and `PPS_LOST` must not claim valid
  Stratum-1 time. The current safe response is LI=3 and Stratum 16.
- Synchronization requires both a fresh PPS edge and a valid RMC sentence
  paired to that PPS.
- The RMC sentence following a PPS identifies that PPS UTC second.
- Never synchronize from RMC validity alone.
- Never reuse a duplicate or stale PPS edge.
- Detect stale PPS and RMC using elapsed monotonic timer time.
- Do not silently continue advertising Stratum 1 after GPS or PPS loss.
- Preserve integer-safe interpolation between PPS anchors. The conceptual rate
  correction is:

  ```text
  corrected_elapsed_us =
      elapsed_monotonic_us * 1000000 / measured_us_per_second
  ```

- Keep operation ordering and intermediate widths safe from truncation and
  overflow.
- Do not add empirical UTC offsets to hide timing or scaling errors.
- Keep Unix-to-NTP epoch and Q0.32 fractional-second conversion integer-only in
  the packet timing path.
- NTP receive and transmit timestamps should be sampled as close as practical
  to receive and send, respectively.
- The NTP reference timestamp represents the most recent accepted GPS PPS
  anchor.

## Concurrency and Timing Rules

- Keep the PPS ISR minimal. Do not parse GPS, update the OLED, log verbosely, or
  perform network work from the ISR.
- Avoid blocking calls in the PPS capture path and NTP packet timing path.
- Do not hold clock-state locks during socket operations, formatting, logging,
  or I2C transfers.
- Add read-only snapshot/accessor APIs when another module needs status; do not
  reach into another module's private globals.
- OLED operation is non-critical. Initialization or update failures must never
  interfere with GPS, PPS, Ethernet, or NTP.
- Keep OLED traffic outside the NTP timing path and update it at a modest rate.
- Keep HTTP status collection and rendering outside the PPS ISR and NTP packet
  timing path. HTTP failure must not stop the timing services.
- Preserve useful serial diagnostics, but avoid high-rate logging that can
  disturb timing.
- Check ESP-IDF, socket, and peripheral errors. Degrade safely when a
  non-critical peripheral fails.

## Module Boundaries

- `main/main.c`: application startup and high-level initialization
- `main/app_config.*`: NVS-backed appliance configuration and validation
- `main/gps_receiver.*`: UART/NMEA reception and PPS capture
- `main/clock_discipline.*`: PPS/RMC association, state machine, clock snapshot
  API, and UTC discipline
- `main/clock_math.*`: portable integer clock-rate and NTP-fraction math
- `main/ethernet_w5500.*`: W5500, Ethernet events, DHCP, MAC, and network
  snapshots
- `main/ntp_server.*`: UDP/123 request validation and NTP responses
- `main/status_model.*`: read-only clock, NTP, network, and uptime aggregation
- `main/http_status_server.*`: non-critical TCP/80 HTTP presentation
- `main/mdns_discovery.*`: non-critical Ethernet hostname and service discovery
- `main/oled_display.*`: non-critical SSD1306 display task and rendering
- `tests/clock_math_test.c`: host-side clock math tests

Respect these boundaries when adding features. Keep timing math independently
testable where practical.

## Required Validation

For every change, run both commands from the repository root:

```sh
idf.py build
./tests/run_clock_math_test.sh
```

Report both results before stopping. Do not flash hardware automatically.
Flashing is a user action unless explicitly requested.

For clock-related changes, add or extend tests before changing behavior.
At minimum preserve coverage for:

- 100,000 us at 0 ppm
- 100,000 us, 500,000 us, and 999,000 us with a 1,000,005 us/s local rate
- monotonic interpolation between PPS anchors
- 500,000 us converting to approximately `0x80000000` in NTP fractional form

## Repository Hygiene

- Preserve unrelated changes in a dirty worktree.
- Do not use destructive Git commands unless explicitly authorized.
- Do not commit, push, flash, or open a pull request unless requested.
- Do not edit generated content in `build/` or managed component sources.
- Add dependencies through `main/idf_component.yml` and document them.
- Update `README.md` when hardware, pin assignments, dependencies, behavior, or
  validation procedures change.
