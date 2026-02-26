#include "fastnvmetrics/fastnvmetrics.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace fastnvmetrics {

// ── Helpers ────────────────────────────────────────────────────────

static std::string read_file_string(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string s;
    std::getline(f, s, '\0');
    return s;
}

static bool path_readable(const std::string &path) {
    return access(path.c_str(), R_OK) == 0;
}

/// Count CPU cores from /proc/stat (lines matching "cpu[0-9]+").
static int count_cpu_cores() {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return 0;
    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        if (line.size() >= 4 && line[0] == 'c' && line[1] == 'p' &&
            line[2] == 'u' && line[3] >= '0' && line[3] <= '9') {
            ++count;
        }
    }
    return count;
}

// ── Pre-baked board configs ────────────────────────────────────────

static BoardConfig make_agx_orin() {
    BoardConfig c;
    c.board_name    = "agx_orin";
    c.num_cpu_cores = 12;
    c.gpu_load_path = "/sys/devices/platform/bus@0/17000000.gpu/load";
    c.emc_actmon_path = "/sys/kernel/debug/cactmon/mc_all";

    // INA3221 @ 0x40 (hwmon3)
    const std::string h0 =
        "/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon3";
    c.power_rails = {
        {"VDD_GPU_SOC",     h0 + "/in1_input", h0 + "/curr1_input"},
        {"VDD_CPU_CV",      h0 + "/in2_input", h0 + "/curr2_input"},
        {"VIN_SYS_5V0",     h0 + "/in3_input", h0 + "/curr3_input"},
    };
    // INA3221 @ 0x41 (hwmon4)
    const std::string h1 =
        "/sys/bus/i2c/drivers/ina3221/1-0041/hwmon/hwmon4";
    c.power_rails.push_back(
        {"VDDQ_VDD2_1V8AO", h1 + "/in2_input", h1 + "/curr2_input"});

    c.thermal_zones = {
        {"cpu-thermal",    "/sys/class/thermal/thermal_zone0/temp"},
        {"gpu-thermal",    "/sys/class/thermal/thermal_zone1/temp"},
        {"cv0-thermal",    "/sys/class/thermal/thermal_zone2/temp"},
        {"cv1-thermal",    "/sys/class/thermal/thermal_zone3/temp"},
        {"cv2-thermal",    "/sys/class/thermal/thermal_zone4/temp"},
        {"soc0-thermal",   "/sys/class/thermal/thermal_zone5/temp"},
        {"soc1-thermal",   "/sys/class/thermal/thermal_zone6/temp"},
        {"soc2-thermal",   "/sys/class/thermal/thermal_zone7/temp"},
        {"tj-thermal",     "/sys/class/thermal/thermal_zone8/temp"},
        {"tboard-thermal", "/sys/class/thermal/thermal_zone9/temp"},
        {"tdiode-thermal", "/sys/class/thermal/thermal_zone10/temp"},
    };

    return c;
}

static BoardConfig make_orin_nx() {
    BoardConfig c;
    c.board_name    = "orin_nx";
    c.num_cpu_cores = 8;
    c.gpu_load_path = "/sys/devices/platform/bus@0/17000000.gpu/load";
    c.emc_actmon_path = "/sys/kernel/debug/cactmon/mc_all";

    // INA3221 @ 0x40 — rail labels may differ on NX carrier boards.
    // These are defaults for the NVIDIA devkit carrier (P3768).
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
        {"cv0-thermal",    "/sys/class/thermal/thermal_zone2/temp"},
        {"cv1-thermal",    "/sys/class/thermal/thermal_zone3/temp"},
        {"cv2-thermal",    "/sys/class/thermal/thermal_zone4/temp"},
        {"soc0-thermal",   "/sys/class/thermal/thermal_zone5/temp"},
        {"soc1-thermal",   "/sys/class/thermal/thermal_zone6/temp"},
        {"soc2-thermal",   "/sys/class/thermal/thermal_zone7/temp"},
        {"tj-thermal",     "/sys/class/thermal/thermal_zone8/temp"},
        {"tboard-thermal", "/sys/class/thermal/thermal_zone9/temp"},
    };

    return c;
}

// ── Public API ─────────────────────────────────────────────────────

BoardConfig get_board_config(const std::string &name) {
    if (name == "agx_orin") return make_agx_orin();
    if (name == "orin_nx")  return make_orin_nx();
    throw std::runtime_error("Unknown board: " + name);
}

/// Validate and prune a board config: remove paths that don't exist,
/// override cpu core count from /proc/stat.
static void validate_config(BoardConfig &c) {
    // Override core count with runtime value
    int cores = count_cpu_cores();
    if (cores > 0) c.num_cpu_cores = cores;

    // GPU load
    if (!path_readable(c.gpu_load_path)) c.gpu_load_path.clear();

    // EMC (debugfs — may require setup script)
    if (!path_readable(c.emc_actmon_path)) c.emc_actmon_path.clear();

    // Power rails — keep only readable ones
    c.power_rails.erase(
        std::remove_if(c.power_rails.begin(), c.power_rails.end(),
                        [](const PowerRailConfig &r) {
                            return !path_readable(r.voltage_path) ||
                                   !path_readable(r.current_path);
                        }),
        c.power_rails.end());

    // Thermal zones — keep only readable ones
    c.thermal_zones.erase(
        std::remove_if(c.thermal_zones.begin(), c.thermal_zones.end(),
                        [](const ThermalZoneConfig &z) {
                            return !path_readable(z.temp_path);
                        }),
        c.thermal_zones.end());
}

BoardConfig detect_board() {
    // Read /proc/device-tree/compatible (null-separated strings)
    std::string compat = read_file_string("/proc/device-tree/compatible");

    BoardConfig config;

    if (compat.find("p3701") != std::string::npos) {
        // Jetson AGX Orin (p3701 module)
        config = make_agx_orin();
    } else if (compat.find("p3767") != std::string::npos) {
        // Jetson Orin NX (p3767 module)
        config = make_orin_nx();
    } else {
        throw std::runtime_error(
            "Unrecognized Jetson board. Compatible string: " + compat +
            "\nUse get_board_config() with an explicit board name, or add "
            "a new config. See docs/adding_a_board.md");
    }

    validate_config(config);
    return config;
}

} // namespace fastnvmetrics
