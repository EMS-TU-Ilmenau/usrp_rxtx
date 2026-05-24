# usrp_rxtx

Coherent, distributed measurement software for [USRP-family](https://www.ettus.com/products/) software-defined radios.

`usrp_rxtx` is a headless C++20 application built on top of [UHD](https://github.com/EttusResearch/uhd). It performs loss-less, hardware-timestamped Rx and Tx streaming with one or more USRPs per host, exposes received samples to arbitrary external consumers through a zero-copy shared-memory buffer, and integrates with an MQTT-based orchestration layer for distributed, multi-node measurement campaigns. It targets radio propagation, channel sounding, and ISAC researchers who need more than the UHD example programs but less than the overhead of a GNU Radio flowgraph. `usrp_rxtx` achieves sustained, loss-less, full-duplex, multi-channel streaming at maximum sample rates with the USRP X310 and B205 on moderate hardware.

The implementation, its design rationale, and the requirements it is meant to meet are described in detail in Chapter 2 of the author's doctoral thesis (to be published Q4/26 or Q1/27).

## Features

- Continuous, high-performance, multi-channel, multi-threaded, hardware-timestamped Rx and Tx streaming 
- Multi-node coherence maintained by transparent handling of streaming errors with zero padding (Rx) and time-aligned burst restarts (Tx)
- Optional file writing of received samples, started and stopped at run time, aligned to the next full UTC second across distributed nodes
- Zero-copy, multi-consumer shared-memory circular buffer with run-time attach/detach for external real-time processing or visualization consumers
- Time-triggered, PPS-aligned armed start for coherent operation across nodes
- MQTT-based telemetry (JSON payloads, auto-derived topics per node and device)
- SSH-based remote control via POSIX signals
- Asynchronous, structured JSON logging with a separate machine-readable log file per run
- INI-style configuration; no recompilation required
- Runs on x86_64 server-class hosts and on aarch64 single-board computers (e.g. Raspberry Pi 5)
- Unit-tested under AddressSanitizer and UndefinedBehaviorSanitizer

## Supported hardware

Tested on:

- USRP X310 (with UBX and TwinRX daughterboards)
- USRP B205

Other UHD-supported USRPs are likely to work but are not verified.

## Requirements

- Linux (x86_64 or aarch64)
- C++20 compiler with std::format support (GCC 13+ or Clang 17+)
- [Meson](https://mesonbuild.com/) and Ninja
- [UHD](https://github.com/EttusResearch/uhd) ≥ 4.3.0

### Dependency installation on Debian/Ubuntu

```sh
apt install --no-install-recommends build-essential meson pkg-config libssl-dev libuhd-dev libboost-system-dev
```

## Building

```sh
git clone --recursive https://github.com/EMS-TU-Ilmenau/usrp_rxtx.git usrp_rxtx
cd usrp_rxtx
make release
```

The resulting executable is `build/usrp_rxtx`.

Available Makefile targets:

| Target       | Description                                                      |
|--------------|------------------------------------------------------------------|
| `release`    | Optimized release build                                          |
| `debug`      | Debug build, no sanitizers                                       |
| `debug_asan` | Debug build with AddressSanitizer and UndefinedBehaviorSanitizer |
| `debug_msan` | Debug build with MemorySanitizer                                 |
| `test`       | Build and run unit tests under sanitizers                        |
| `clean`      | Remove the build directory                                       |

## Usage

```sh
sudo ./build/usrp_rxtx <config.cfg>
```

A minimal Rx-only configuration for a B205:

```ini
[usrp]
args = serial=DEADBEEF

[rx]
subdev = A:A
antenna = TX/RX
bandwidth = 56e6
freq_rf = 3.75e9
gain = 0.
rate = 60e6

[wr]
directory = /tmp
```

Further example configurations are provided in `config/`:

- `b205_rx.cfg`, `b205_tx.cfg`, `b205_rxtx.cfg`: B205, single channel
- `x310_ubx_hg.cfg`, `x310_ubx_xg.cfg`: X310 with two UBX daughterboards over single or dual 10 GbE
- `x310_twinrx_xg.cfg`: X310 with TwinRX daughterboards (four Rx channels) over dual 10 GbE

The full list of configurable parameters and their defaults is defined in `src/config.cfg`.

## Configuration

Configuration is a plain INI file with the following sections:

| Section   | Purpose                                                                                                       |
|-----------|---------------------------------------------------------------------------------------------------------------|
| `[usrp]`  | UHD device arguments and clock/time synchronization source                                                    |
| `[rx]`    | Rx subdev specification, antenna, center frequency (RF + DSP tuning), bandwidth, gain, sample rate, LO source |
| `[tx]`    | Same as `[rx]`, plus the path to the binary Tx waveform file (`std::complex<int16_t>`)                        |
| `[mqtt]`  | Broker host, port, credentials, topic prefix, optional raw-sample publication                                 |
| `[shmem]` | Mount points and sizes of the shared-memory descriptor and ring buffers                                       |
| `[wr]`    | Output directory for recorded sample files                                                                    |
| `[tune]`  | Host-side performance tuning knobs (e.g. PM QoS CPU-DMA latency)                                              |

Either `[rx]` or `[tx]` (or both) may be active. Empty values fall back to the device defaults.

## Synchronization

The `[usrp] sync` option selects how the USRP's clock and time references are initialized. Synchronization is performed once at startup, after the master clock rate has been set and before frontend tuning.

| Value        | Clock source       | Time source        | USRP time set from |
|--------------|--------------------|--------------------|--------------------|
| `host`       | Internal           | Internal           | Host system clock  |
| `10mhz`      | External 10 MHz    | Internal           | Host system clock  |
| `1pps`       | Internal           | External 1 PPS     | Host system clock, rounded to next PPS edge |
| `10mhz+1pps` | External 10 MHz    | External 1 PPS     | Host system clock, rounded to next PPS edge |
| `gpsdo`      | On-board GPSDO     | On-board GPSDO     | Host system clock, rounded to next PPS edge |
| *(empty)*    | USRP default       | USRP default       | Not set            |

For coherent operation across distributed nodes, a shared external 10 MHz reference and a common 1 PPS edge (`10mhz+1pps`) are required, or an on-board GPSDO (`gpsdo`) on each node. The `host` and `10mhz` modes derive USRP time from the host system clock alone and provide only host-clock-grade alignment between nodes; they are intended for single-node use or for setups in which NTP/PTP time synchronization across hosts is sufficient.

## Runtime control

`usrp_rxtx` responds to the following signals:

| Signal              | Effect                                                                          |
|---------------------|---------------------------------------------------------------------------------|
| `SIGUSR1`           | Toggle file writing of received samples (start, then stop, then start again, …) |
| `SIGUSR2`           | Toggle Tx muting                                                                |
| `SIGINT`, `SIGTERM` | Graceful shutdown: stop streaming, flush buffers, close files, exit             |
| `SIGHUP`            | Ignored to survive loss of SSH connection                                       |

For coordinated remote control across distributed nodes, SSH-dispatched signals are the intended mechanism.

## Output

Each active Rx channel is written to its own file in `[wr] directory`. The filename encodes host name, USRP device address, start timestamp (nanoseconds since the Unix epoch), and channel number, e.g.:

```
rx_hostname_addr=192.168.40.2_1073741824000000000_ch0.cint16.bin
```

The `.cint16.bin` suffix denotes interleaved I/Q 16-bit integer samples (`std::complex<int16_t>`). Recording starts at the next full UTC second after the `SIGUSR1` that initiated it, so files from spatially separated nodes are sample-aligned.

A machine-readable JSON log file alongside the recordings captures the full run-time configuration, hardware identity, driver and software version, and per-recording metadata (filenames, start timestamps, buffer offsets), so that recordings remain self-describing.

## Telemetry

If `[mqtt] host` is set to a non-default value, `usrp_rxtx` connects to the configured MQTT broker and publishes one JSON telemetry message per second under a topic derived from the host name and the USRP device address. Example payload:

```json
{
    "time_ns": 1073741824000000000,
    "rx_seconds": 3600.0,
    "tx_seconds": null,
    "wr_seconds": 60.0,
    "wr_backlog": 1048576,
    "wr_free": 4398046511104
}
```

| Field        | Type           | Description                                                                         |
|--------------|----------------|-------------------------------------------------------------------------------------|
| `time_ns`    | integer        | USRP hardware timestamp (nanoseconds since the Unix epoch)                          |
| `rx_seconds` | float / null   | Duration of uninterrupted Rx streaming in seconds; `null` if Rx is disabled         |
| `tx_seconds` | float / null   | Duration of uninterrupted Tx streaming in seconds; `null` if Tx is disabled         |
| `wr_seconds` | float / null   | Duration of the current recording in seconds; `null` if no recording is in progress |
| `wr_backlog` | integer        | Unwritten samples currently buffered in the circular buffer (bytes)                 |
| `wr_free`    | integer        | Remaining free space on the recording filesystem (bytes)                            |

Raw 16-bit IQ samples can optionally be published on a separate topic for remote real-time visualization (`[mqtt] pub_samples`).

## Citing

If you use `usrp_rxtx` in published work, please cite:

```bibtex
@misc{github_usrp_rxtx,
    author = {Andrich, Carsten},
    license = {GPLv3},
    title = {{usrp\_rxtx}},
    year = {2026},
    url = {https://github.com/EMS-TU-Ilmenau/usrp_rxtx},
}
```

## License

GPL-3.0-or-later (see [`COPYING`](COPYING) for the full text) unless otherwise specified by `SPDX-License-Identifier` file headers.
