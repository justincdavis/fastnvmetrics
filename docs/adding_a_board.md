# Adding a New Jetson Board to fastnvmetrics

This guide walks through the process of discovering sysfs/procfs paths on a
new Jetson device and registering a `BoardConfig` in the fastnvmetrics codebase.

All commands below are run on the target Jetson device itself.

---

## Prerequisites

- JetPack 6.x installed (L4T 36.x, CUDA 12.2+, kernel 5.15)
- Shell access on the device (SSH or serial console)
- `sudo` access (needed only for debugfs/EMC setup, not for the profiler itself)

---

## Step 1: Identify the Board

### 1.1 Read the Device Tree Model

```bash
cat /proc/device-tree/model
```

Example outputs:
| Device | Model string |
|--------|-------------|
| AGX Orin 64 GB | `NVIDIA Jetson AGX Orin Developer Kit` |
| Orin NX 16 GB | `NVIDIA Orin NX Developer Kit` |
| Orin Nano 8 GB | `NVIDIA Orin Nano Developer Kit` |

### 1.2 Read the Compatible String

This is what `detect_board()` in `config.cpp` matches against.

```bash
cat /proc/device-tree/compatible | tr '\0' '\n'
```

Example outputs:
```
nvidia,p3737-0000+p3701-0005    # AGX Orin (module p3701 on carrier p3737)
nvidia,tegra234

nvidia,p3768-0000+p3767-0000    # Orin NX (module p3767 on carrier p3768)
nvidia,tegra234

nvidia,p3768-0000+p3767-0005    # Orin Nano (module p3767-0005 on carrier p3768)
nvidia,tegra234
```

**Record these values.** The module part number (e.g., `p3701`, `p3767`) is
what you'll match on in `detect_board()`. Different carrier boards may change
the first entry but not the module identifier.

### 1.3 Choose a Board Name

Pick a short, unique identifier for use in the codebase. Convention:
`<product>_<variant>`, all lowercase, underscores.

Examples: `agx_orin`, `orin_nx`, `orin_nano`

---

## Step 2: Count CPU Cores

```bash
grep -c '^processor' /proc/cpuinfo
```

Or equivalently, count `cpu[0-9]+` lines in `/proc/stat`:

```bash
grep -cE '^cpu[0-9]' /proc/stat
```

| Device | Cores |
|--------|-------|
| AGX Orin | 12 |
| Orin NX | 8 |
| Orin Nano | 6 |

The engine validates this at runtime via `/proc/stat` and overrides the
pre-baked value, but it should still be correct in the config for documentation
purposes.

---

## Step 3: Find the GPU Load Path

The Tegra GPU exposes a utilization file via sysfs. Search for it:

```bash
find /sys/devices -name "load" -path "*/gpu/*" 2>/dev/null
```

Expected result (all Orin variants share the same GPU block):

```
/sys/devices/platform/bus@0/17000000.gpu/load
```

**Verify it reads correctly:**

```bash
cat /sys/devices/platform/bus@0/17000000.gpu/load
```

Returns an integer 0–1000 (divide by 10 for percentage). Under idle
conditions, expect `0`. Run a GPU workload to see it change.

**Read latency benchmark** (optional, see [Step 7](#step-7-benchmark-read-latencies)):
Typical: ~320 μs per lseek+read on AGX Orin.

---

## Step 4: Find INA3221 Power Monitor Paths

Jetson boards use TI INA3221 triple-channel power monitors connected via I2C.
Each chip measures voltage and current on up to 3 rails.

### 4.1 List INA3221 Devices

```bash
ls -d /sys/bus/i2c/drivers/ina3221/*/hwmon/hwmon* 2>/dev/null
```

Example on AGX Orin:
```
/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3
/sys/bus/i2c/drivers/ina3221/1-0041/hwmon/hwmon4
```

The I2C addresses (`1-0040`, `1-0041`) vary by board. The hwmon numbers
(`hwmon3`, `hwmon4`) may also vary depending on kernel module probe order
(though they are typically stable across reboots on the same JetPack version).

### 4.2 Discover Rail Labels

Each INA3221 has 3 channels. Read the label files to identify which rail is which:

```bash
for hwmon in /sys/bus/i2c/drivers/ina3221/*/hwmon/hwmon*; do
    echo "=== $hwmon ==="
    for i in 1 2 3; do
        label=$(cat "$hwmon/curr${i}_label" 2>/dev/null || echo "(empty)")
        voltage=$(cat "$hwmon/in${i}_input" 2>/dev/null || echo "N/A")
        current=$(cat "$hwmon/curr${i}_input" 2>/dev/null || echo "N/A")
        echo "  Channel $i: label=$label  voltage=${voltage}mV  current=${current}mA"
    done
done
```

Example output on AGX Orin:
```
=== /sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3 ===
  Channel 1: label=VDD_GPU_SOC  voltage=843mV  current=6170mA
  Channel 2: label=VDD_CPU_CV   voltage=780mV  current=3437mA
  Channel 3: label=VIN_SYS_5V0  voltage=4968mV current=953mA
=== /sys/bus/i2c/drivers/ina3221/1-0041/hwmon/hwmon4 ===
  Channel 1: label=(empty)      voltage=N/A    current=N/A
  Channel 2: label=VDDQ_VDD2_1V8AO  voltage=1800mV  current=341mA
  Channel 3: label=(empty)      voltage=N/A    current=N/A
```

**Record:** For each active rail (non-empty label with readable values):
- **Label** (e.g., `VDD_GPU_SOC`)
- **Voltage path** (e.g., `/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3/in1_input`)
- **Current path** (e.g., `/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3/curr1_input`)

Units: voltage in mV, current in mA. The engine computes power as `v * c / 1000` (milliwatts).

### 4.3 Notes on INA3221 Read Latency

INA3221 reads go through the I2C bus and are significantly slower than sysfs
reads (~554 μs per register on AGX Orin). This is why power sampling runs on
the medium tier (100 Hz) rather than the fast tier (1 kHz).

---

## Step 5: Find Thermal Zone Paths

### 5.1 List All Thermal Zones

```bash
for zone in /sys/class/thermal/thermal_zone*/; do
    type=$(cat "${zone}type" 2>/dev/null)
    temp=$(cat "${zone}temp" 2>/dev/null || echo "EAGAIN")
    printf "%-40s  type=%-20s  temp=%s\n" "$zone" "$type" "$temp"
done
```

Example on AGX Orin:
```
/sys/class/thermal/thermal_zone0/   type=cpu-thermal           temp=47500
/sys/class/thermal/thermal_zone1/   type=gpu-thermal           temp=42000
/sys/class/thermal/thermal_zone2/   type=cv0-thermal           temp=EAGAIN
/sys/class/thermal/thermal_zone3/   type=cv1-thermal           temp=EAGAIN
/sys/class/thermal/thermal_zone4/   type=cv2-thermal           temp=EAGAIN
/sys/class/thermal/thermal_zone5/   type=soc0-thermal          temp=46000
/sys/class/thermal/thermal_zone6/   type=soc1-thermal          temp=44000
/sys/class/thermal/thermal_zone7/   type=soc2-thermal          temp=46500
/sys/class/thermal/thermal_zone8/   type=tj-thermal            temp=47500
/sys/class/thermal/thermal_zone9/   type=tboard-thermal        temp=37500
/sys/class/thermal/thermal_zone10/  type=tdiode-thermal        temp=39750
```

**Record:** For each zone:
- **Name** (the `type` value, e.g., `cpu-thermal`)
- **Temp path** (e.g., `/sys/class/thermal/thermal_zone0/temp`)

Temperature values are in millidegrees Celsius (divide by 1000 for °C).

Zones that return `EAGAIN` are powered-down accelerators (typically DLA/PVA
on `cv*-thermal`). The engine reads them as 0°C — include them in the config
so they're available when the accelerator is active.

### 5.2 Notes

- Different Orin variants may have different numbers of thermal zones
- The zone numbering is determined by kernel device-tree order and is
  stable across reboots on the same JetPack version
- `tj-thermal` is the junction temperature (max of all sensors) — always include it

---

## Step 6: Find EMC (Memory Controller) Actmon Path

EMC utilization is exposed via debugfs, which requires root access.

### 6.1 Check if cactmon Module is Loaded

```bash
ls /sys/kernel/debug/cactmon/ 2>/dev/null
```

If this fails with "Permission denied", try:

```bash
sudo ls /sys/kernel/debug/cactmon/
```

Expected:
```
mc_all
```

### 6.2 Read EMC Utilization

```bash
sudo cat /sys/kernel/debug/cactmon/mc_all
```

Returns an integer (utilization metric).

### 6.3 Set Up Non-Root Access

The `setup_fastnvmetrics.sh` script handles this:

```bash
sudo bash scripts/setup_fastnvmetrics.sh
```

This sets `o+rx` on `/sys/kernel/debug` and `/sys/kernel/debug/cactmon`,
and `o+r` on `mc_all`. Permissions reset on reboot.

### 6.4 If cactmon is Not Available

Some Orin variants or JetPack versions may not have the `tegra_cactmon_mc_all`
module. Try loading it:

```bash
sudo modprobe tegra_cactmon_mc_all
```

If unavailable, set `emc_actmon_path` to `""` in the config — the engine
will skip EMC sampling gracefully (returning -1.0).

The standard debugfs path for all Orin variants is:
```
/sys/kernel/debug/cactmon/mc_all
```

---

## Step 7: Benchmark Read Latencies

To verify 1 kHz sampling is feasible, benchmark each metric source. Create
and compile this C program on the target device:

```c
// bench_sysfs.c — Benchmark lseek+read latency for sysfs files
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define ITERATIONS 10000

static double bench(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1.0; }

    char buf[256];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < ITERATIONS; i++) {
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf));
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    close(fd);

    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    return elapsed / ITERATIONS * 1e6;  // microseconds per read
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <sysfs_path> [path2] ...\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        double us = bench(argv[i]);
        if (us >= 0)
            printf("%-60s  %.1f us/read  (%.1f kHz max)\n",
                   argv[i], us, 1000.0 / us);
    }
    return 0;
}
```

Compile and run:

```bash
gcc -O2 -o bench_sysfs bench_sysfs.c
./bench_sysfs \
    /sys/devices/platform/bus@0/17000000.gpu/load \
    /proc/stat \
    /proc/meminfo \
    /sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3/in1_input \
    /sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3/curr1_input \
    /sys/class/thermal/thermal_zone0/temp
```

### Reference Results (AGX Orin, JetPack 6.0)

| Source | Latency | Max Rate |
|--------|---------|----------|
| GPU load | 322.5 μs | 3.1 kHz |
| `/proc/stat` (CPU) | 23.9 μs | 41.8 kHz |
| `/proc/meminfo` (RAM) | 3.3 μs | 303 kHz |
| INA3221 voltage (I2C) | 554.2 μs | 1.8 kHz |
| INA3221 current (I2C) | 553.7 μs | 1.8 kHz |
| Thermal zone | 80.0 μs | 12.5 kHz |

### What to Check

- **GPU load** should be under ~500 μs for 1 kHz fast-tier to work. If
  significantly higher, consider lowering `fast_hz`.
- **INA3221** reads are I2C-limited (~550 μs per register). With N rails
  × 2 registers each, the total must fit within `1000/medium_hz` ms. For
  4 rails at 100 Hz: 8 reads × 554 μs = 4.4 ms ≪ 10 ms budget.
- **Thermal** reads should be well within the slow-tier budget (100 ms at 10 Hz).

---

## Step 8: Register the Board Config

### 8.1 Add the Pre-baked Config Function

Edit `src/fastnvmetrics/src/config.cpp`. Add a new `make_<board_name>()` function
following the existing pattern:

```cpp
static BoardConfig make_orin_nano() {
    BoardConfig c;
    c.board_name    = "orin_nano";
    c.num_cpu_cores = 6;
    c.gpu_load_path = "/sys/devices/platform/bus@0/17000000.gpu/load";
    c.emc_actmon_path = "/sys/kernel/debug/cactmon/mc_all";

    // INA3221 @ 0x40 — verify hwmon number on your device!
    const std::string h0 =
        "/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3";
    c.power_rails = {
        {"VDD_GPU_SOC",  h0 + "/in1_input", h0 + "/curr1_input"},
        {"VDD_CPU_CV",   h0 + "/in2_input", h0 + "/curr2_input"},
        {"VIN_SYS_5V0",  h0 + "/in3_input", h0 + "/curr3_input"},
    };

    c.thermal_zones = {
        {"cpu-thermal",    "/sys/class/thermal/thermal_zone0/temp"},
        {"gpu-thermal",    "/sys/class/thermal/thermal_zone1/temp"},
        // ... add all zones discovered in Step 5
    };

    return c;
}
```

### 8.2 Register in `get_board_config()`

Add the new board to the name-based lookup:

```cpp
BoardConfig get_board_config(const std::string &name) {
    if (name == "agx_orin")   return make_agx_orin();
    if (name == "orin_nx")    return make_orin_nx();
    if (name == "orin_nano")  return make_orin_nano();  // ← add this
    throw std::runtime_error("Unknown board: " + name);
}
```

### 8.3 Register in `detect_board()`

Add a match for the new board's device-tree compatible string:

```cpp
BoardConfig detect_board() {
    std::string compat = read_file_string("/proc/device-tree/compatible");

    BoardConfig config;

    if (compat.find("p3701") != std::string::npos) {
        config = make_agx_orin();
    } else if (compat.find("p3767") != std::string::npos) {
        // Orin NX and Orin Nano both use p3767 module
        // Distinguish by sub-variant:
        if (compat.find("p3767-0005") != std::string::npos) {
            config = make_orin_nano();   // ← Orin Nano
        } else {
            config = make_orin_nx();     // ← Orin NX (p3767-0000)
        }
    } else {
        throw std::runtime_error(
            "Unrecognized Jetson board. Compatible string: " + compat +
            "\nUse get_board_config() with an explicit board name, or add "
            "a new config. See docs/adding_a_board.md");
    }

    validate_config(config);
    return config;
}
```

**Important:** `detect_board()` calls `validate_config()` which:
1. Overrides `num_cpu_cores` from `/proc/stat` (handles power-mode changes)
2. Removes unreadable GPU/EMC paths
3. Prunes power rails with inaccessible voltage or current files
4. Prunes thermal zones that can't be opened

This means slightly-wrong paths are safe — they'll be silently dropped at
runtime. But correct paths are needed for the metrics to actually work.

### 8.4 Rebuild and Test

```bash
cd src/fastnvmetrics
pip install -e .
python -c "from fastnvmetrics import detect_board; b = detect_board(); print(b.board_name, b.num_cpu_cores)"
```

---

## Quick Reference: All Sysfs Paths

| Metric | Path Pattern | Format |
|--------|-------------|--------|
| Board identity | `/proc/device-tree/compatible` | Null-separated strings |
| Board model | `/proc/device-tree/model` | String |
| CPU cores | `/proc/stat` | Lines matching `cpu[0-9]+` |
| GPU utilization | `/sys/devices/platform/bus@0/17000000.gpu/load` | Integer 0–1000 |
| INA3221 voltage | `/sys/bus/i2c/drivers/ina3221/<addr>/hwmon/hwmon<N>/in<C>_input` | Integer (mV) |
| INA3221 current | `/sys/bus/i2c/drivers/ina3221/<addr>/hwmon/hwmon<N>/curr<C>_input` | Integer (mA) |
| INA3221 labels | `/sys/bus/i2c/drivers/ina3221/<addr>/hwmon/hwmon<N>/curr<C>_label` | String |
| Thermal zones | `/sys/class/thermal/thermal_zone<N>/temp` | Integer (millideg C) |
| Thermal names | `/sys/class/thermal/thermal_zone<N>/type` | String |
| EMC utilization | `/sys/kernel/debug/cactmon/mc_all` | Integer (needs root/setup) |

---

## Gotchas

1. **hwmon numbering is not guaranteed stable across JetPack versions.** If
   a JetPack update reorders kernel module loading, `hwmon3` could become
   `hwmon4`. The `validate_config()` call will catch this (paths become
   unreadable), but the config will need updating. A more robust approach
   would be to match by the INA3221 I2C address and search for the correct
   hwmon symlink at runtime — this is a potential future enhancement.

2. **Orin NX and Orin Nano share the same module part number (`p3767`)** but
   with different sub-variants (`p3767-0000` vs `p3767-0005`). Make sure the
   `detect_board()` match is specific enough.

3. **DLA/PVA thermal zones (`cv*-thermal`)** return `EAGAIN` when the
   accelerator is powered down. The engine handles this gracefully (reads as
   0°C). Include these zones in the config anyway.

4. **EMC debugfs requires the setup script** to run after each boot. Without
   it, EMC reads return -1.0 and the `emc_available` flag in the trace header
   is 0.

5. **Power-mode changes** can affect CPU core count (e.g., some modes disable
   cores). The engine re-counts cores from `/proc/stat` at startup, so this is
   handled automatically.

6. **Custom carrier boards** may have different INA3221 wiring than the NVIDIA
   devkit carrier. The I2C addresses and rail assignments could differ — always
   verify on the actual hardware.
