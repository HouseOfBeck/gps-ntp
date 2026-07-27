# GPS-Disciplined ESP32 NTP Server

A compact Stratum-1 NTP appliance built around an ESP32-S3, a GT-U7 GPS
receiver, and a W5500 Ethernet controller. A PPS-disciplined software clock
provides UTC to a minimal UDP/123 NTP server, while a 128x64 OLED shows live
clock and network status.

The design is deliberately Ethernet-only. GPS/PPS correctness is the primary
concern: the server advertises Stratum 1 only while it has a fresh, valid
PPS/RMC synchronization pair and immediately degrades its NTP response when
that timing source becomes stale.

## Features

- GPS UTC from valid NMEA RMC sentences
- PPS capture using the ESP32 monotonic timer
- Rolling 32-second PPS rate estimate
- Integer-only interpolation and NTP timestamp conversion in timing-sensitive
  paths
- W5500 Ethernet with DHCP and an ESP32-derived Ethernet MAC address
- Standard 48-byte NTP v3/v4 server responses on UDP port 123
- Safe LI=3 / Stratum 16 responses whenever the clock is not synchronized
- SSD1306-compatible 128x64 status display
- Host-side unit tests for clock interpolation and NTP fractional conversion
- No Wi-Fi dependency or fallback

## Architecture

```text
 GT-U7 UART (RMC) ─┐
                   ├──> clock discipline ──> read-only clock snapshots
 GT-U7 PPS ────────┘             │                    │
                                │                    ├──> NTP timestamps
                                │                    └──> OLED status
                                │
 W5500 Ethernet ──> DHCP/lwIP ──┴──> UDP/123 NTP server
          │
          └──> read-only network snapshot ──> OLED status
```

The PPS ISR only captures monotonic timing information. GPS parsing, clock
state transitions, Ethernet work, NTP packet processing, and OLED I2C traffic
run outside the ISR.

The clock associates a fresh PPS edge with the valid RMC sentence that follows
it. The RMC UTC second identifies that PPS edge. A rolling PPS window estimates
the ESP timer's local rate, and the disciplined clock interpolates from the
most recent accepted UTC/PPS anchor using integer arithmetic.

The OLED obtains clock and network snapshots before rendering. It does not
hold clock-state locks during I2C writes, and display failure is non-fatal.

## Hardware

### Required parts

- ESP32-S3 development board with 16 MB flash
- W5500 Ethernet module
- GT-U7 GPS receiver with PPS output
- Active or passive GPS antenna appropriate for the receiver/module
- SSD1306-compatible 128x64 I2C OLED at address `0x3C`
- 3.3 V and 5 V supplies available as listed below
- Common ground between all modules
- Ethernet connection to a DHCP-enabled LAN

Confirm the voltage requirements and signal levels of the specific modules
before wiring. The connections below describe the hardware used by this
project.

## Complete Pinout

### GT-U7 GPS

| GT-U7 signal | ESP32-S3 connection |
| --- | --- |
| TX | GPIO44 (UART RX) |
| RX | GPIO43 (UART TX) |
| PPS | GPIO3 |
| VCC | 5 V |
| GND | GND |

The receiver is configured for 9600 baud, 8 data bits, no parity, and one stop
bit.

### W5500 Ethernet

| W5500 signal | ESP32-S3 connection |
| --- | --- |
| MOSI | GPIO11 |
| MISO | GPIO12 |
| CLK | GPIO13 |
| CS | GPIO14 |
| INT | GPIO10 |
| RESET | GPIO9 |

The W5500 uses the ESP32-S3 SPI2 host. Its Ethernet MAC is derived from the
ESP32 factory MAC using the `ESP_MAC_ETH` interface identity.

### OLED

| OLED signal | ESP32-S3 connection |
| --- | --- |
| SDA | GPIO17 |
| SCL | GPIO18 |
| VCC | 3.3 V |
| GND | GND |

The display uses I2C address `0x3C`, a 400 kHz bus, and the modern ESP-IDF I2C
master API.

### Reserved microSD interface

The microSD interface is not currently used. These GPIOs are reserved to avoid
future pin conflicts.

| microSD signal | ESP32-S3 connection |
| --- | --- |
| MOSI | GPIO6 |
| MISO | GPIO5 |
| CLK | GPIO7 |
| CS | GPIO4 |

## Software Requirements

- [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/index.html)
- A host toolchain supported by that ESP-IDF release
- Git
- A C compiler for the portable host-side clock math test

Managed component dependencies are resolved by ESP-IDF:

| Component | Version |
| --- | ---: |
| `espressif/w5500` | 2.0.0 |
| `espressif/wiznet_common` | 1.0.0 |

The W5500 requirement is declared in `main/idf_component.yml`; exact resolved
versions are recorded in `dependencies.lock`. The OLED driver is implemented
in-tree and adds no external display dependency.

## Build Setup

Clone the repository and enter its root:

```sh
git clone git@github.com:HouseOfBeck/gps-ntp.git
cd gps-ntp
```

Install ESP-IDF v6.0.2, then activate that environment. Adjust the path to
match the local ESP-IDF installation:

```sh
. "$HOME/esp/esp-idf/export.sh"
idf.py --version
```

For an ESP-IDF v6.0.2 installation managed by Espressif Installation Manager,
the activation command may instead be:

```sh
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
idf.py --version
```

`idf.py --version` should report ESP-IDF v6.0.2. On a new configuration, set
the target before building:

```sh
idf.py set-target esp32s3
idf.py build
```

The checked-in `sdkconfig` contains the current project configuration,
including the ESP32-S3 target and 16 MB flash setup. Avoid casually replacing
it with defaults from another board or ESP-IDF release.

## Flash and Monitor

Flashing is intentionally a separate, explicit hardware step. Replace `PORT`
with the board's serial device:

```sh
idf.py -p PORT flash
idf.py -p PORT monitor
```

Or flash and open the serial monitor in one command:

```sh
idf.py -p PORT flash monitor
```

Use `Ctrl-]` to exit the ESP-IDF monitor. This repository's automated
development workflow builds and tests but does not flash hardware
automatically.

## Clock Discipline

Synchronization requires a fresh PPS edge and a valid RMC sentence paired to
that edge. A valid RMC sentence by itself is never sufficient. Each PPS can be
accepted only once, preventing duplicate or stale edges from being reused.

The current implementation:

- accepts an RMC/PPS pair only while the PPS is suitably fresh
- keeps a UTC-second plus monotonic-microsecond anchor
- estimates local timer rate over a rolling window of up to 32 PPS intervals
- interpolates UTC using portable integer math
- treats PPS and accepted RMC data as stale after 2.5 seconds
- exposes snapshots so consumers do not access private clock state directly

Conceptually, interpolation applies:

```text
elapsed_us = now_monotonic_us - pps_monotonic_us

corrected_elapsed_us =
    elapsed_us * 1000000 / measured_us_per_second

utc_now = pps_utc + corrected_elapsed_us
```

The implementation represents the rate as an integer observation window to
retain precision and avoid floating-point behavior in timestamp generation.

## NTP Behavior

The server listens on UDP port 123 and accepts normal client-mode (mode 3) NTP
v3 and v4 requests. Responses are standard 48-byte server-mode (mode 4)
packets.

When synchronized:

- Leap indicator: 0, no warning
- Stratum: 1
- Reference identifier: `GPS`
- Originate timestamp: copied from the client's transmit timestamp
- Receive timestamp: sampled immediately after receiving the request, as close
  to socket reception as practical
- Transmit timestamp: sampled from the disciplined clock near transmission
- Reference timestamp: the most recent accepted GPS PPS UTC anchor

Unix UTC is converted to the NTP 1900 epoch with the 2,208,988,800-second epoch
offset. Fractional microseconds are converted to the NTP 32-bit fractional
field using integer arithmetic.

When the clock is not synchronized, the server replies with leap indicator 3
(alarm condition), Stratum 16, and a non-GPS reference identifier. It does not
silently continue serving apparent Stratum-1 time.

## Synchronization States

| State | Meaning | NTP advertisement |
| --- | --- | --- |
| `UNSYNC` | No usable UTC discipline, or GPS data is invalid | LI=3, Stratum 16 |
| `ACQUIRING` | PPS or valid RMC activity is present, but no accepted anchor exists yet | LI=3, Stratum 16 |
| `SYNCED` | Fresh accepted PPS/RMC anchor and fresh source data | LI=0, Stratum 1, refid `GPS` |
| `HOLDOVER` | PPS remains present, but the accepted RMC/UTC association is stale | LI=3, Stratum 16 |
| `PPS_LOST` | An anchor exists, but PPS has become stale | LI=3, Stratum 16 |

Only `SYNCED` is considered synchronized. Although the software clock can
continue to advance in a degraded state, degraded states deliberately do not
claim valid Stratum-1 service.

## OLED Status

The SSD1306-compatible display updates at approximately 4 Hz and writes only
when the framebuffer changes. Its normal layout is:

```text
GPS NTP SERVER
01:09:38 UTC
SYNCED  STRATUM 1
PPS +5.38 PPM
192.168.1.199
```

Depending on current state, the status line prominently shows `ACQUIRING`,
`HOLDOVER`, `PPS LOST`, or `UNSYNC`. Before DHCP or after link loss, the final
line reports the relevant Ethernet state instead of an IP address.

OLED initialization and updates are non-critical. If the display is absent or
an I2C operation fails, the failure is logged and GPS, PPS, Ethernet, and NTP
continue operating.

## Tests and Hardware Validation

Every change should pass both required checks from the repository root:

```sh
idf.py build
./tests/run_clock_math_test.sh
```

The host-side test builds `clock_math.c` with strict warnings and verifies:

- exact 0 ppm interpolation
- correction for a measured local rate of 1,000,005 us/s
- 100 ms, 500 ms, and near-one-second interpolation cases
- monotonic output across repeated calls between PPS anchors
- conversion of 500,000 us to the NTP half-second fraction `0x80000000`

After flashing manually, a practical validation sequence is:

1. Place the GPS antenna with a clear view of the sky.
2. Watch the serial log for valid RMC data, PPS capture, and an accepted
   PPS/RMC anchor.
3. Confirm Ethernet link and DHCP address assignment.
4. Confirm the OLED progresses from `ACQUIRING` to `SYNCED  STRATUM 1`.
5. Query UDP/123 from another LAN host and verify LI=0, Stratum 1, refid `GPS`,
   and a reference timestamp on the accepted integer UTC second.
6. Compare multiple consecutive NTP timestamps and confirm their elapsed time
   follows client elapsed time rather than merely checking absolute offset.
7. Disconnect or obstruct the GPS/PPS source and verify the server promptly
   returns LI=3 / Stratum 16 and the OLED shows a degraded state.
8. Restore GPS reception and verify clean reacquisition before Stratum 1 is
   advertised again.

Packet timing can be inspected from another machine with a capture filter such
as:

```sh
sudo tcpdump -ni INTERFACE udp port 123
```

### chrony comparison

For a longer comparison, add the ESP32's DHCP address to a test machine's
chrony configuration. For example:

```text
server 192.168.1.199 iburst minpoll 4 maxpoll 4
```

Then inspect the source and statistics:

```sh
chronyc sources -v
chronyc sourcestats -v
chronyc tracking
```

In one approximately 25-minute LAN comparison against a Mac mini disciplined
by Internet NTP sources, the ESP32 source measured about +4.45 ms offset with
approximately 66 us standard deviation and about +0.114 ppm estimated residual
frequency. Typical LAN request processing was on the order of tens of
microseconds. These figures describe one setup, network, antenna position, and
reference clock; they are validation examples, not guaranteed specifications.

## Known Limitations

- GPS acquisition is highly sensitive to antenna placement. Indoor operation,
  nearby structures, poor antenna orientation, and insufficient sky view can
  greatly delay or prevent a valid fix. Cold acquisition should be tested
  outdoors or near a suitable window with the antenna correctly oriented.
- NTP timestamps are generated in software. There is no W5500 or ESP32
  hardware packet timestamping, so lwIP, SPI, task scheduling, and LAN queueing
  contribute delay and asymmetry.
- Synchronized responses always use LI=0; scheduled leap-second announcement
  propagation is not currently implemented.
- There is no quantified long-term holdover model. `HOLDOVER` is deliberately
  advertised as unsynchronized rather than claiming Stratum 1.
- Ethernet configuration is DHCP-only in the current implementation.
- The OLED driver targets the SSD1306-compatible controller behavior used by
  the tested 128x64 display.
- The reserved microSD interface is not implemented.
- There is no Wi-Fi fallback, web status page, or persistent NTP statistics.

## Repository Structure

```text
.
├── AGENTS.md                     Project rules for automated contributors
├── CMakeLists.txt                ESP-IDF top-level project
├── dependencies.lock            Resolved managed-component versions
├── main/
│   ├── CMakeLists.txt            Application component definition
│   ├── idf_component.yml         W5500 managed-component requirement
│   ├── main.c                    Application entry point
│   ├── clock_discipline.c/.h     PPS/RMC discipline and state snapshots
│   ├── clock_math.c/.h           Portable integer timing math
│   ├── ethernet_w5500.c/.h       W5500, DHCP, and network state
│   ├── gps_receiver.c/.h         GPS UART parsing and PPS capture
│   ├── ntp_server.c/.h           UDP/123 NTP server
│   └── oled_display.c/.h         SSD1306 display task and renderer
├── sdkconfig                     ESP32-S3 project configuration
└── tests/
    ├── clock_math_test.c         Host-side timing math tests
    └── run_clock_math_test.sh    Portable test build/run script
```
