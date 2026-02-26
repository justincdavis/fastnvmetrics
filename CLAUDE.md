# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

fastnvmetrics is a C++/Python high-frequency profiler for NVIDIA Jetson Orin devices. It samples GPU utilization, CPU utilization (per-core + aggregate), EMC (memory controller) utilization, power rails, RAM usage, and thermal sensors at up to 1 kHz via direct sysfs/procfs reads with nanobind Python bindings.

**Platform requirement:** NVIDIA Jetson Orin (AGX Orin, Orin NX) running JetPack 6.x (L4T 36.x).

## Build Commands

```bash
# Install (standard)
pip install .

# Install (editable, for development)
pip install -e . --no-build-isolation

# Rebuild after C++ changes
rm -f fastnvmetrics/_ext*.so && pip install -e . --no-build-isolation

# Build and run C++ tests (GTest)
cmake -B build -G "Unix Makefiles" -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build

# Run Python tests
pytest tests/test_fastnvmetrics.py -v

# Run a single Python test
pytest tests/test_fastnvmetrics.py -v -k test_name

# EMC debugfs setup (one-time per boot, requires sudo)
sudo bash scripts/setup_fastnvmetrics.sh
```

## Architecture

### Multi-rate sampling

Three tiers, each on its own thread:
- **Fast** (1 kHz): GPU load, CPU util (12 cores + aggregate), RAM, EMC — budget ~350 μs
- **Medium** (100 Hz): Power rails (4 rails × voltage + current via INA3221 I2C) — budget ~4.4 ms
- **Slow** (10 Hz): Thermal sensors (11 zones) — budget ~880 μs

### Binary format

All data is packed structs written to a binary file:
- **FileHeader** (728 bytes): magic `0x4E564D54`, version, board info, sampling rates, sample counts, rail/zone names
- **FastSample** (98 bytes): time_s, gpu_load, cpu_util[16], cpu_aggregate, ram_used_kb, ram_available_kb, emc_util
- **MediumSample** (104 bytes): time_s, voltage_mv[8], current_ma[8], power_mw[8]
- **SlowSample** (72 bytes): time_s, temp_c[16]
- **SyncPoint** (16 bytes): sync_id (u64), fast_sample_idx (u64) — appended after all samples

### C++ core (`include/fastnvmetrics/fastnvmetrics.hpp`, `src/engine.cpp`, `src/config.cpp`)

`fastnvmetrics::Engine` is non-copyable/non-movable. It owns pre-opened sysfs file descriptors and three sampling threads.

Board config auto-detection reads `/proc/device-tree/compatible` and matches against pre-baked configs (AGX Orin p3701, Orin NX p3767). Runtime validation prunes inaccessible sysfs paths.

Sampling uses the lseek+read pattern on pre-opened fds for minimal overhead. Timing uses CLOCK_MONOTONIC with TIMER_ABSTIME for drift-free intervals.

Thread safety: `sample_count` and `is_running` use atomics; sync points vector protected by mutex; warmup uses condition_variable + mutex.

### Python bindings (`src/bindings.cpp`)

nanobind module `_ext` exposes `Engine` as `NVMetrics`. Blocking calls (`start`, `stop`, `wait_for_warmup`) release the GIL. The context manager (`__enter__`) calls `start()` then `wait_for_warmup()`.

### Python package (`fastnvmetrics/__init__.py`)

`read_trace()` parses the binary file into a `TraceData` TypedDict with numpy arrays. Three tiers are forward-filled and sync points are expanded into a per-fast-sample `sync_id` array.

### Build system

CMake 3.18+ with scikit-build-core backend. Compiler flags: `-O3 -march=armv8-a`. Static library `fastnvmetrics_core` linked into nanobind module `_ext`. Dependencies: nanobind >= 2.0, Python >= 3.10, numpy (runtime).

## Key Conventions

- INA3221 I2C reads dominate (~554 μs each) — power must sample at lower rate
- `emc_actmon_path` requires debugfs setup script (one-time sudo per boot)
- Board configs are pre-baked with runtime validation; see `docs/adding_a_board.md` to add new boards
- Type stubs in `fastnvmetrics/_ext.pyi` must be kept in sync with `src/bindings.cpp`
