#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fastnvmetrics {

// ── Constants ──────────────────────────────────────────────────────

constexpr uint32_t MAGIC   = 0x4E564D54u; // "NVMT" (NVMetrics Trace)
constexpr uint32_t VERSION = 1;

constexpr int MAX_CPU_CORES     = 16;
constexpr int MAX_POWER_RAILS   = 8;
constexpr int MAX_THERMAL_ZONES = 16;

// ── Binary format (packed structs) ─────────────────────────────────
//
// File layout:
//   [FileHeader]
//   [FastSample   × num_fast_samples]
//   [MediumSample × num_medium_samples]
//   [SlowSample   × num_slow_samples]
//   [SyncPoint    × num_sync_points]

#pragma pack(push, 1)

/// File header (728 bytes). Written once at start, updated at stop.
struct FileHeader {
    uint32_t magic;
    uint32_t version;

    // Board info
    char     board_name[32];
    uint8_t  num_cpu_cores;
    uint8_t  num_power_rails;
    uint8_t  num_thermal_zones;
    uint8_t  emc_available; // 1 if EMC sampling active

    // Sampling rates (Hz)
    uint32_t fast_hz;
    uint32_t medium_hz;
    uint32_t slow_hz;

    // Sample counts (updated at stop)
    uint64_t num_fast_samples;
    uint64_t num_medium_samples;
    uint64_t num_slow_samples;
    uint64_t num_sync_points;

    // Rail and zone names (null-terminated strings)
    char power_rail_names[MAX_POWER_RAILS][24];
    char thermal_zone_names[MAX_THERMAL_ZONES][24];

    uint8_t reserved[64];
};

/// Fast-tier sample (98 bytes). GPU, CPU, RAM, EMC at ~1 kHz.
struct FastSample {
    double   time_s;                      // seconds since engine start
    uint16_t gpu_load;                    // 0–1000 (÷10 for %)
    float    cpu_util[MAX_CPU_CORES];     // per-core %, 0.0–100.0
    float    cpu_aggregate;               // overall CPU %
    uint64_t ram_used_kb;                 // kB
    uint64_t ram_available_kb;            // kB
    float    emc_util;                    // EMC %, 0.0–100.0 (−1 if N/A)
};

/// Medium-tier sample (104 bytes). Power rails at ~100 Hz.
struct MediumSample {
    double   time_s;
    uint32_t voltage_mv[MAX_POWER_RAILS]; // millivolts
    uint32_t current_ma[MAX_POWER_RAILS]; // milliamps
    float    power_mw[MAX_POWER_RAILS];   // milliwatts (v×i/1000)
};

/// Slow-tier sample (72 bytes). Thermal sensors at ~10 Hz.
struct SlowSample {
    double time_s;
    float  temp_c[MAX_THERMAL_ZONES];     // degrees Celsius
};

/// Sync point — marks a phase boundary in the fast-tier timeline.
struct SyncPoint {
    uint64_t sync_id;          // incrementing 1, 2, 3, …
    uint64_t fast_sample_idx;  // index into fast-tier samples
};

#pragma pack(pop)

// ── Board configuration ────────────────────────────────────────────

struct PowerRailConfig {
    std::string label;        // e.g. "VDD_GPU_SOC"
    std::string voltage_path; // sysfs path to in*_input
    std::string current_path; // sysfs path to curr*_input
};

struct ThermalZoneConfig {
    std::string name;      // e.g. "cpu-thermal"
    std::string temp_path; // sysfs path to temp file
};

struct BoardConfig {
    std::string board_name;   // e.g. "agx_orin"
    int num_cpu_cores;
    std::string gpu_load_path;
    std::string emc_actmon_path;   // debugfs cactmon/mc_all, empty if unavailable
    std::string emc_clk_rate_path; // debugfs clk/emc/clk_rate, empty if unavailable
    std::vector<PowerRailConfig> power_rails;
    std::vector<ThermalZoneConfig> thermal_zones;
};

/// Auto-detect the current board from /proc/device-tree/compatible.
/// Validates that sysfs paths exist, disabling unavailable metrics.
/// Throws std::runtime_error if the board is unrecognized.
BoardConfig detect_board();

/// Get a pre-baked config by name ("agx_orin", "orin_nx").
/// Throws std::runtime_error if name is unknown.
BoardConfig get_board_config(const std::string &name);

// ── Engine configuration ───────────────────────────────────────────

struct EngineConfig {
    uint32_t fast_hz   = 1000;
    uint32_t medium_hz = 100;
    uint32_t slow_hz   = 10;
};

// ── Engine ─────────────────────────────────────────────────────────

class Engine {
public:
    Engine(const std::string &output_path,
           const BoardConfig &board,
           const EngineConfig &config = {});
    ~Engine();

    Engine(const Engine &)            = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&)                 = delete;
    Engine &operator=(Engine &&)      = delete;

    void     start();
    void     stop();
    void     wait_for_warmup();
    uint64_t sync();

    uint64_t sample_count() const;
    bool     is_running() const;

private:
    void run_fast();
    void run_medium();
    void run_slow();
    void write_file();

    // ── Readers (lseek + read on pre-opened fds) ──
    uint16_t read_gpu_load();
    void     read_cpu(float *per_core, float &aggregate);
    void     read_ram(uint64_t &used_kb, uint64_t &available_kb);
    float    read_emc();
    void     read_power(uint32_t *voltage, uint32_t *current, float *power);
    void     read_thermals(float *temps);

    void open_fds();
    void close_fds();

    // Config
    std::string  output_path_;
    BoardConfig  board_;
    EngineConfig config_;

    // Threads
    std::thread fast_thread_;
    std::thread medium_thread_;
    std::thread slow_thread_;

    // State
    std::atomic<bool>     running_{false};
    std::atomic<bool>     warmed_up_{false};
    std::atomic<uint64_t> fast_count_{0};

    // Warmup synchronization
    std::mutex              warmup_mtx_;
    std::condition_variable warmup_cv_;

    // Sync points
    std::vector<SyncPoint> sync_points_;
    std::mutex             sync_mtx_;

    // Sample buffers (each tier thread has exclusive write access;
    // read only after threads are joined in stop())
    std::vector<FastSample>   fast_samples_;
    std::vector<MediumSample> medium_samples_;
    std::vector<SlowSample>   slow_samples_;

    // Monotonic start time
    double t0_s_ = 0.0;

    // File descriptors (opened once, reused via lseek)
    int fd_gpu_load_  = -1;
    int fd_proc_stat_ = -1;
    int fd_meminfo_   = -1;
    double emc_sample_period_s_ = 0.0; // cactmon sample period (seconds)
    std::vector<int> fd_voltage_;
    std::vector<int> fd_current_;
    std::vector<int> fd_thermal_;

    // CPU delta state (per-core previous jiffies)
    struct CpuJiffies {
        uint64_t total = 0;
        uint64_t idle  = 0;
    };
    std::vector<CpuJiffies> prev_cpu_;
};

} // namespace fastnvmetrics
