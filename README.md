# fastnvmetrics — High-Frequency Jetson Orin Profiler

Multi-rate hardware profiler for NVIDIA Jetson Orin devices. Samples
GPU utilization, CPU utilization (per-core + aggregate), EMC (memory
controller) utilization, power rails, RAM usage, and thermal sensors
at up to 1 kHz via direct sysfs/procfs reads.

Designed for NVIDIA Jetson AGX Orin and Orin NX running JetPack 6.x.

## Requirements

- **Platform**: NVIDIA Jetson Orin (AGX Orin, Orin NX) with JetPack 6.x
- **Compiler**: GCC or Clang with C++17 and `-march=armv8-a` support
- **Libraries**: pthread
- **Python**: >= 3.10
- **Python build deps**: [scikit-build-core](https://scikit-build-core.readthedocs.io/) >= 0.10, [nanobind](https://nanobind.readthedocs.io/) >= 2.0

## Installation

### From source (recommended)

```bash
git clone https://github.com/orinagx/fastnvmetrics.git
cd fastnvmetrics
pip install .
```

### Editable / development install

```bash
pip install -e . --no-build-isolation
```

### EMC setup (optional, one-time per boot)

To enable EMC (memory controller) utilization monitoring:

```bash
sudo bash scripts/setup_fastnvmetrics.sh
```

## Quick Start

```python
from fastnvmetrics import NVMetrics, read_trace
import time

# Record metrics for ~2 seconds
with NVMetrics("/tmp/trace.bin") as nv:
    # already warmed up and sampling
    nv.sync()                   # mark phase 1
    time.sleep(1)
    nv.sync()                   # mark phase 2
    time.sleep(1)
    print(f"Recorded {nv.sample_count} fast-tier samples")

# Read the trace file
data = read_trace("/tmp/trace.bin")
print(f"GPU load (mean): {data['gpu_load'].mean() / 10:.1f}%")
print(f"CPU aggregate (mean): {data['cpu_aggregate'].mean():.1f}%")
print(f"Power rails: {data['power_rail_names']}")
```

## Python API

### NVMetrics Class

```python
NVMetrics(output_path, *, fast_hz=1000, medium_hz=100, slow_hz=10, board=None)
```

Create a profiler engine. Auto-detects the Jetson board and opens sysfs
file descriptors for all available metrics.

**Constructor parameters:**

| Parameter     | Type              | Default      | Description                                    |
|---------------|-------------------|--------------|------------------------------------------------|
| `output_path` | `str`             | *(required)* | Path to the binary trace file to write         |
| `fast_hz`     | `int`             | `1000`       | Fast-tier rate: GPU, CPU, RAM, EMC             |
| `medium_hz`   | `int`             | `100`        | Medium-tier rate: power rails (I2C limited)    |
| `slow_hz`     | `int`             | `10`         | Slow-tier rate: thermal sensors                |
| `board`       | `BoardConfig\|None` | `None`     | Board config. Auto-detected if None.           |

**Methods:**

| Method               | Description                                                          |
|----------------------|----------------------------------------------------------------------|
| `start()`            | Spawn sampling threads. Raises `RuntimeError` if already running.    |
| `stop()`             | Stop threads, join, and write the output file.                       |
| `wait_for_warmup()`  | Block until warmup phase (10 fast samples) completes.                |
| `sync()`             | Record a sync point. Returns incrementing ID (`1`, `2`, `3`, ...).   |

**Properties (read-only):**

| Property       | Type   | Description                                              |
|----------------|--------|----------------------------------------------------------|
| `sample_count` | `int`  | Fast-tier samples recorded so far (safe while running)   |
| `is_running`   | `bool` | Whether sampling threads are active                      |

**Context manager:**

`NVMetrics` supports `with` statements. `__enter__` calls `start()` +
`wait_for_warmup()` automatically. On exit, sampling is silently
stopped if still running:

```python
with NVMetrics("/tmp/trace.bin") as nv:
    # already warmed up and sampling
    run_workload()
# auto-stopped on exit
```

### Sync Points

Call `sync()` to partition trace samples into epochs:

```python
from fastnvmetrics import NVMetrics, read_trace

with NVMetrics("/tmp/trace.bin") as nv:
    nv.sync()               # epoch 1
    run_workload_A()
    nv.sync()               # epoch 2
    run_workload_B()

data = read_trace("/tmp/trace.bin")
a_gpu = data["gpu_load"][data["sync_id"] == 1]
b_gpu = data["gpu_load"][data["sync_id"] == 2]
print(f"Workload A GPU: {a_gpu.mean() / 10:.1f}%")
print(f"Workload B GPU: {b_gpu.mean() / 10:.1f}%")
```

### `read_trace(path) -> TraceData`

Read a binary trace file. Returns a `TraceData` TypedDict:

**Header metadata:**

| Key                  | Type        | Description                     |
|----------------------|-------------|---------------------------------|
| `board_name`         | `str`       | Detected board name             |
| `num_cpu_cores`      | `int`       | Number of CPU cores             |
| `num_power_rails`    | `int`       | Number of active power rails    |
| `num_thermal_zones`  | `int`       | Number of active thermal zones  |
| `emc_available`      | `bool`      | Whether EMC data is present     |
| `fast_hz`            | `int`       | Fast-tier sampling rate (Hz)    |
| `medium_hz`          | `int`       | Medium-tier sampling rate (Hz)  |
| `slow_hz`            | `int`       | Slow-tier sampling rate (Hz)    |
| `power_rail_names`   | `list[str]` | Rail labels (e.g. "VDD_GPU_SOC")|
| `thermal_zone_names` | `list[str]` | Zone names (e.g. "cpu-thermal") |

**Fast tier (default 1 kHz):**

| Key              | Type                   | Description                         |
|------------------|------------------------|-------------------------------------|
| `fast_time_s`    | `ndarray[float64]`     | Timestamps (seconds since start)    |
| `gpu_load`       | `ndarray[uint16]`      | GPU utilization 0–1000 (÷10 for %)  |
| `cpu_util`       | `ndarray[float32]`     | Per-core CPU %, shape (N, cores)    |
| `cpu_aggregate`  | `ndarray[float32]`     | Overall CPU %                       |
| `ram_used_kb`    | `ndarray[uint64]`      | RAM used (kB)                       |
| `ram_available_kb`| `ndarray[uint64]`     | RAM available (kB)                  |
| `emc_util`       | `ndarray[float32]`     | EMC % (−1 if unavailable)           |

**Medium tier (default 100 Hz):**

| Key            | Type               | Description                          |
|----------------|--------------------|--------------------------------------|
| `medium_time_s`| `ndarray[float64]` | Timestamps                           |
| `voltage_mv`   | `ndarray[uint32]`  | Voltage per rail (mV), shape (N, R)  |
| `current_ma`   | `ndarray[uint32]`  | Current per rail (mA), shape (N, R)  |
| `power_mw`     | `ndarray[float32]` | Power per rail (mW), shape (N, R)    |

**Slow tier (default 10 Hz):**

| Key           | Type               | Description                           |
|---------------|--------------------|---------------------------------------|
| `slow_time_s` | `ndarray[float64]` | Timestamps                            |
| `temp_c`      | `ndarray[float32]` | Temperature per zone (°C), shape (N, Z) |

**Sync points:**

| Key       | Type               | Description                                    |
|-----------|--------------------|------------------------------------------------|
| `sync_id` | `ndarray[uint64]`  | Per-fast-sample epoch (0 = before first sync)  |

### Board Configuration

```python
from fastnvmetrics import detect_board, get_board_config

# Auto-detect from /proc/device-tree/compatible
board = detect_board()

# Or get a pre-baked config by name
board = get_board_config("agx_orin")

print(f"{board.board_name}: {board.num_cpu_cores} cores")
print(f"GPU: {board.gpu_load_path}")
print(f"Rails: {[r.label for r in board.power_rails]}")
```

To add support for a new Jetson board, see `docs/adding_a_board.md`.

## C++ API

The public header `include/fastnvmetrics/fastnvmetrics.hpp` exposes the
full API under `namespace fastnvmetrics`:

```cpp
#include "fastnvmetrics/fastnvmetrics.hpp"
#include <chrono>
#include <thread>

int main() {
    auto board = fastnvmetrics::detect_board();
    fastnvmetrics::EngineConfig cfg{1000, 100, 10};

    fastnvmetrics::Engine engine("trace.bin", board, cfg);
    engine.start();
    engine.wait_for_warmup();
    engine.sync();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    engine.sync();
    engine.stop();

    return 0;
}
```

**Key types:**

| Type               | Description                                                            |
|--------------------|------------------------------------------------------------------------|
| `BoardConfig`      | Board name, CPU count, sysfs paths for all metrics                     |
| `EngineConfig`     | `fast_hz`, `medium_hz`, `slow_hz` — sampling rates per tier            |
| `Engine`           | Owns fds, threads, sample buffers. Non-copyable, non-movable.          |
| `FileHeader`       | Packed struct (728 bytes) — magic, board info, sample counts, rail/zone names |
| `FastSample`       | Packed struct (98 bytes) — GPU, CPU, RAM, EMC                          |
| `MediumSample`     | Packed struct (104 bytes) — power rails                                |
| `SlowSample`       | Packed struct (72 bytes) — thermal sensors                             |
| `SyncPoint`        | Packed struct (16 bytes) — sync_id, fast_sample_idx                    |

## Binary Trace Format

Each trace file has a 728-byte packed header, followed by fast samples,
medium samples, slow samples, and optional sync points:

```
[FileHeader 728B] [FastSample × N] [MediumSample × M] [SlowSample × S] [SyncPoint × P]
```

**Header** (728 bytes, little-endian):

| Offset | Field                | Type         | Bytes | Description                       |
|--------|----------------------|--------------|-------|-----------------------------------|
| 0      | `magic`              | `uint32`     | 4     | `0x4E564D54` ("NVMT")             |
| 4      | `version`            | `uint32`     | 4     | Currently `1`                     |
| 8      | `board_name`         | `char[32]`   | 32    | Null-terminated board identifier  |
| 40     | `num_cpu_cores`      | `uint8`      | 1     | CPU core count                    |
| 41     | `num_power_rails`    | `uint8`      | 1     | Power rail count                  |
| 42     | `num_thermal_zones`  | `uint8`      | 1     | Thermal zone count                |
| 43     | `emc_available`      | `uint8`      | 1     | 1 if EMC active                   |
| 44     | `fast_hz`            | `uint32`     | 4     | Fast-tier rate (Hz)               |
| 48     | `medium_hz`          | `uint32`     | 4     | Medium-tier rate (Hz)             |
| 52     | `slow_hz`            | `uint32`     | 4     | Slow-tier rate (Hz)               |
| 56     | `num_fast_samples`   | `uint64`     | 8     | Fast sample count                 |
| 64     | `num_medium_samples` | `uint64`     | 8     | Medium sample count               |
| 72     | `num_slow_samples`   | `uint64`     | 8     | Slow sample count                 |
| 80     | `num_sync_points`    | `uint64`     | 8     | Sync point count                  |
| 88     | `power_rail_names`   | `char[8][24]`| 192   | Null-terminated rail labels       |
| 280    | `thermal_zone_names` | `char[16][24]`| 384  | Null-terminated zone names        |
| 664    | `reserved`           | `uint8[64]`  | 64    | Zero-filled                       |

## Project Structure

```
fastnvmetrics/
├── CMakeLists.txt              # C++17, nanobind, -march=armv8-a
├── pyproject.toml              # scikit-build-core + nanobind build backend
├── README.md
├── .gitignore
├── include/
│   └── fastnvmetrics/
│       └── fastnvmetrics.hpp   # Public C++ header (namespace fastnvmetrics)
├── src/
│   ├── config.cpp              # Board auto-detection and pre-baked configs
│   ├── engine.cpp              # Engine: sampling threads, sysfs readers, file I/O
│   └── bindings.cpp            # Thin nanobind Python bindings
├── fastnvmetrics/              # Python package
│   ├── __init__.py             # NVMetrics, TraceData, read_trace()
│   ├── _ext.pyi                # Type stubs for the native extension
│   └── py.typed                # PEP 561 marker
├── scripts/
│   └── setup_fastnvmetrics.sh  # EMC debugfs setup (sudo, one-time per boot)
├── docs/
│   └── adding_a_board.md       # Guide for adding new Jetson board configs
└── tests/
    ├── test_engine.cpp         # C++ GTest suite (28 tests)
    └── test_fastnvmetrics.py   # Python pytest suite (41 tests)
```

## Development

### Building

```bash
# Clone and install in editable mode
git clone https://github.com/orinagx/fastnvmetrics.git
cd fastnvmetrics
pip install -e . --no-build-isolation

# Force rebuild after C++ changes
rm -f fastnvmetrics/_ext*.so
pip install -e . --no-build-isolation
```

### Running a quick smoke test

```bash
python -c "
from fastnvmetrics import NVMetrics, read_trace
import time
with NVMetrics('/tmp/nvm_test.bin') as nv:
    time.sleep(2)
    print(f'Samples: {nv.sample_count}')
data = read_trace('/tmp/nvm_test.bin')
print(f'GPU load (mean): {data[\"gpu_load\"].mean() / 10:.1f}%')
print(f'CPU (mean): {data[\"cpu_aggregate\"].mean():.1f}%')
"
```

### Notes

- The warmup phase (10 fast-tier samples) runs before recording begins.
  Use `wait_for_warmup()` to block until warmup completes. The context
  manager calls this automatically.
- `sample_count` uses `std::atomic<uint64_t>` and is safe to poll from
  the main thread while sampling is running.
- The `Engine` is non-copyable and non-movable — it owns file descriptors
  and three sampling threads.
- INA3221 I2C reads are the physical bottleneck (~554 μs each), which is
  why power rails sample at 100 Hz rather than 1 kHz.
- EMC (memory controller) utilization requires the setup script to run
  once after each boot — it chmod's the debugfs paths for non-root access.

## License

MIT
