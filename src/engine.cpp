#include "fastnvmetrics/fastnvmetrics.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <time.h>
#include <unistd.h>

namespace fastnvmetrics {

// ── Helpers ────────────────────────────────────────────────────────

static double monotonic_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) * 1e-9;
}

/// Read a small sysfs/procfs file into buf via lseek+read on an open fd.
/// Returns number of bytes read, or 0 on error.
static ssize_t sysfs_read(int fd, char *buf, size_t bufsize) {
    if (fd < 0) return 0;
    if (lseek(fd, 0, SEEK_SET) < 0) return 0;
    ssize_t n = read(fd, buf, bufsize - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return n;
}

/// Read a sysfs file containing a single integer.
static int sysfs_read_int(int fd) {
    char buf[32];
    if (sysfs_read(fd, buf, sizeof(buf)) <= 0) return 0;
    return atoi(buf);
}

/// Advance pointer past the next occurrence of target char, or return nullptr.
static const char *skip_past(const char *p, char c) {
    while (*p && *p != c) ++p;
    return *p ? p + 1 : nullptr;
}

/// Parse a uint64 from string, advance pointer past trailing whitespace.
static uint64_t parse_u64(const char *&p) {
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    return v;
}

// ── Lifecycle ──────────────────────────────────────────────────────

Engine::Engine(const std::string &output_path,
               const BoardConfig &board,
               const EngineConfig &config)
    : output_path_(output_path), board_(board), config_(config) {

    if (board_.num_cpu_cores <= 0 || board_.num_cpu_cores > MAX_CPU_CORES)
        throw std::invalid_argument("num_cpu_cores must be 1–" +
                                    std::to_string(MAX_CPU_CORES));

    if (static_cast<int>(board_.power_rails.size()) > MAX_POWER_RAILS)
        throw std::invalid_argument("Too many power rails (max " +
                                    std::to_string(MAX_POWER_RAILS) + ")");

    if (static_cast<int>(board_.thermal_zones.size()) > MAX_THERMAL_ZONES)
        throw std::invalid_argument("Too many thermal zones (max " +
                                    std::to_string(MAX_THERMAL_ZONES) + ")");
}

Engine::~Engine() {
    if (running_.load(std::memory_order_relaxed)) stop();
}

void Engine::start() {
    if (running_.load(std::memory_order_relaxed))
        throw std::runtime_error("Engine already running");

    running_.store(true, std::memory_order_release);
    warmed_up_.store(false, std::memory_order_release);
    fast_count_.store(0, std::memory_order_release);

    fast_samples_.clear();
    medium_samples_.clear();
    slow_samples_.clear();
    sync_points_.clear();

    open_fds();
    prev_cpu_.assign(board_.num_cpu_cores, CpuJiffies{});

    t0_s_ = monotonic_s();

    fast_thread_   = std::thread(&Engine::run_fast, this);
    medium_thread_ = std::thread(&Engine::run_medium, this);
    slow_thread_   = std::thread(&Engine::run_slow, this);
}

void Engine::stop() {
    running_.store(false, std::memory_order_release);

    if (fast_thread_.joinable())   fast_thread_.join();
    if (medium_thread_.joinable()) medium_thread_.join();
    if (slow_thread_.joinable())   slow_thread_.join();

    write_file();
    close_fds();
}

void Engine::wait_for_warmup() {
    std::unique_lock<std::mutex> lk(warmup_mtx_);
    warmup_cv_.wait(lk, [this] {
        return warmed_up_.load(std::memory_order_acquire);
    });
}

uint64_t Engine::sync() {
    std::lock_guard<std::mutex> lk(sync_mtx_);
    uint64_t id = sync_points_.size() + 1;
    sync_points_.push_back(
        {id, fast_count_.load(std::memory_order_acquire)});
    return id;
}

uint64_t Engine::sample_count() const {
    return fast_count_.load(std::memory_order_acquire);
}

bool Engine::is_running() const {
    return running_.load(std::memory_order_acquire);
}

// ── Sysfs readers ──────────────────────────────────────────────────

uint16_t Engine::read_gpu_load() {
    int v = sysfs_read_int(fd_gpu_load_);
    return static_cast<uint16_t>(std::clamp(v, 0, 1000));
}

void Engine::read_cpu(float *per_core, float &aggregate) {
    // /proc/stat format:
    //   cpu  <user> <nice> <system> <idle> <iowait> <irq> <softirq> <steal> ...
    //   cpu0 <user> <nice> <system> <idle> <iowait> <irq> <softirq> <steal> ...
    //   cpu1 ...
    // We need the first (num_cpu_cores + 1) lines.
    char buf[4096];
    ssize_t n = sysfs_read(fd_proc_stat_, buf, sizeof(buf));
    if (n <= 0) {
        aggregate = 0.0f;
        std::memset(per_core, 0, board_.num_cpu_cores * sizeof(float));
        return;
    }

    // Parse aggregate line (starts with "cpu ")
    const char *p = buf;
    if (p[0] == 'c' && p[1] == 'p' && p[2] == 'u' && p[3] == ' ') {
        p += 4;
        while (*p == ' ') ++p;
        uint64_t vals[10] = {};
        for (int i = 0; i < 10 && *p && *p != '\n'; ++i)
            vals[i] = parse_u64(p);

        // total = sum of all fields (user+nice+system+idle+iowait+irq+softirq+steal)
        // guest/guest_nice are sub-counted within user/nice, so exclude
        uint64_t total = 0;
        for (int i = 0; i < 8; ++i) total += vals[i];
        uint64_t idle_total = vals[3] + vals[4]; // idle + iowait

        // We don't track aggregate prev (only per-core). Compute from
        // per-core results below.
        (void)total;
        (void)idle_total;
    }

    // Parse per-core lines
    float sum_util = 0.0f;
    int cores_parsed = 0;

    // Advance past first line
    p = buf;
    p = skip_past(p, '\n');

    for (int c = 0; c < board_.num_cpu_cores && p; ++c) {
        // Expect "cpuN " where N matches c
        if (!(p[0] == 'c' && p[1] == 'p' && p[2] == 'u')) break;
        // Skip "cpuN "
        const char *q = p + 3;
        while (*q >= '0' && *q <= '9') ++q;
        if (*q == ' ') ++q;

        uint64_t vals[10] = {};
        for (int i = 0; i < 10 && *q && *q != '\n'; ++i)
            vals[i] = parse_u64(q);

        uint64_t total = 0;
        for (int i = 0; i < 8; ++i) total += vals[i];
        uint64_t idle_val = vals[3] + vals[4];

        // Compute delta from previous
        uint64_t d_total = total - prev_cpu_[c].total;
        uint64_t d_idle  = idle_val - prev_cpu_[c].idle;
        prev_cpu_[c].total = total;
        prev_cpu_[c].idle  = idle_val;

        float util = 0.0f;
        if (d_total > 0)
            util = 100.0f * static_cast<float>(d_total - d_idle) /
                   static_cast<float>(d_total);
        per_core[c] = std::clamp(util, 0.0f, 100.0f);
        sum_util += per_core[c];
        ++cores_parsed;

        // Advance to next line
        p = skip_past(q, '\n');
    }

    // Zero-fill unused cores
    for (int c = cores_parsed; c < MAX_CPU_CORES; ++c)
        per_core[c] = 0.0f;

    aggregate = (cores_parsed > 0) ? sum_util / cores_parsed : 0.0f;
}

void Engine::read_ram(uint64_t &used_kb, uint64_t &available_kb) {
    // /proc/meminfo format:
    //   MemTotal:       64349376 kB
    //   MemFree:        35131272 kB
    //   MemAvailable:   57722372 kB
    //   ...
    char buf[1024];
    ssize_t n = sysfs_read(fd_meminfo_, buf, sizeof(buf));
    if (n <= 0) { used_kb = 0; available_kb = 0; return; }

    uint64_t mem_total = 0, mem_available = 0;
    const char *p = buf;

    while (*p) {
        if (std::strncmp(p, "MemTotal:", 9) == 0) {
            p += 9;
            while (*p == ' ') ++p;
            mem_total = parse_u64(p);
        } else if (std::strncmp(p, "MemAvailable:", 13) == 0) {
            p += 13;
            while (*p == ' ') ++p;
            mem_available = parse_u64(p);
            break; // Got both, done
        }
        // Skip to next line
        const char *nl = skip_past(p, '\n');
        if (!nl) break;
        p = nl;
    }

    available_kb = mem_available;
    used_kb = (mem_total >= mem_available) ? (mem_total - mem_available) : 0;
}

float Engine::read_emc() {
    if (fd_emc_ < 0) return -1.0f;

    // cactmon/mc_all format is TBD — try reading as an integer percentage
    char buf[64];
    if (sysfs_read(fd_emc_, buf, sizeof(buf)) <= 0) return -1.0f;

    int v = atoi(buf);
    return std::clamp(static_cast<float>(v), 0.0f, 100.0f);
}

void Engine::read_power(uint32_t *voltage, uint32_t *current, float *power) {
    int nrails = static_cast<int>(board_.power_rails.size());

    for (int i = 0; i < nrails; ++i) {
        uint32_t v = static_cast<uint32_t>(sysfs_read_int(fd_voltage_[i]));
        uint32_t c = static_cast<uint32_t>(sysfs_read_int(fd_current_[i]));
        voltage[i] = v;
        current[i] = c;
        power[i] = static_cast<float>(v) * static_cast<float>(c) / 1000.0f;
    }

    // Zero-fill unused rail slots
    for (int i = nrails; i < MAX_POWER_RAILS; ++i) {
        voltage[i] = 0;
        current[i] = 0;
        power[i] = 0.0f;
    }
}

void Engine::read_thermals(float *temps) {
    int nzones = static_cast<int>(board_.thermal_zones.size());

    for (int i = 0; i < nzones; ++i) {
        int millideg = sysfs_read_int(fd_thermal_[i]);
        temps[i] = static_cast<float>(millideg) / 1000.0f;
    }

    // Zero-fill unused zone slots
    for (int i = nzones; i < MAX_THERMAL_ZONES; ++i)
        temps[i] = 0.0f;
}

// ── Sampling loops ─────────────────────────────────────────────────

/// Add nanoseconds to a timespec, normalizing overflow.
static void timespec_add_ns(struct timespec &ts, long ns) {
    ts.tv_nsec += ns;
    while (ts.tv_nsec >= 1'000'000'000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1'000'000'000L;
    }
}

void Engine::run_fast() {
    const long interval_ns = 1'000'000'000L / config_.fast_hz;

    // Warmup: take a few samples to prime CPU delta state
    for (int i = 0; i < 10 && running_.load(std::memory_order_acquire); ++i) {
        FastSample s{};
        s.time_s = monotonic_s() - t0_s_;
        s.gpu_load = read_gpu_load();
        read_cpu(s.cpu_util, s.cpu_aggregate);
        read_ram(s.ram_used_kb, s.ram_available_kb);
        s.emc_util = read_emc();
        fast_samples_.push_back(s);
        fast_count_.fetch_add(1, std::memory_order_release);

        struct timespec req = {0, interval_ns};
        clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
    }

    {
        std::lock_guard<std::mutex> lk(warmup_mtx_);
        warmed_up_.store(true, std::memory_order_release);
    }
    warmup_cv_.notify_all();

    // Main sampling loop with absolute timing to avoid drift
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (running_.load(std::memory_order_acquire)) {
        FastSample s{};
        s.time_s = monotonic_s() - t0_s_;
        s.gpu_load = read_gpu_load();
        read_cpu(s.cpu_util, s.cpu_aggregate);
        read_ram(s.ram_used_kb, s.ram_available_kb);
        s.emc_util = read_emc();
        fast_samples_.push_back(s);
        fast_count_.fetch_add(1, std::memory_order_release);

        timespec_add_ns(next, interval_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
}

void Engine::run_medium() {
    wait_for_warmup();

    const long interval_ns = 1'000'000'000L / config_.medium_hz;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (running_.load(std::memory_order_acquire)) {
        MediumSample s{};
        s.time_s = monotonic_s() - t0_s_;
        read_power(s.voltage_mv, s.current_ma, s.power_mw);
        medium_samples_.push_back(s);

        timespec_add_ns(next, interval_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
}

void Engine::run_slow() {
    wait_for_warmup();

    const long interval_ns = 1'000'000'000L / config_.slow_hz;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (running_.load(std::memory_order_acquire)) {
        SlowSample s{};
        s.time_s = monotonic_s() - t0_s_;
        read_thermals(s.temp_c);
        slow_samples_.push_back(s);

        timespec_add_ns(next, interval_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
}

// ── File I/O ───────────────────────────────────────────────────────

void Engine::open_fds() {
    auto try_open = [](const std::string &path) -> int {
        if (path.empty()) return -1;
        return open(path.c_str(), O_RDONLY);
    };

    fd_gpu_load_  = try_open(board_.gpu_load_path);
    fd_proc_stat_ = try_open("/proc/stat");
    fd_meminfo_   = try_open("/proc/meminfo");
    fd_emc_       = try_open(board_.emc_actmon_path);

    for (const auto &r : board_.power_rails) {
        fd_voltage_.push_back(try_open(r.voltage_path));
        fd_current_.push_back(try_open(r.current_path));
    }
    for (const auto &z : board_.thermal_zones) {
        fd_thermal_.push_back(try_open(z.temp_path));
    }
}

void Engine::close_fds() {
    auto try_close = [](int &fd) {
        if (fd >= 0) { close(fd); fd = -1; }
    };

    try_close(fd_gpu_load_);
    try_close(fd_proc_stat_);
    try_close(fd_meminfo_);
    try_close(fd_emc_);

    for (auto &fd : fd_voltage_) try_close(fd);
    for (auto &fd : fd_current_) try_close(fd);
    for (auto &fd : fd_thermal_) try_close(fd);

    fd_voltage_.clear();
    fd_current_.clear();
    fd_thermal_.clear();
}

void Engine::write_file() {
    FILE *fp = fopen(output_path_.c_str(), "wb");
    if (!fp) throw std::runtime_error("Cannot open " + output_path_);

    // Build header
    FileHeader hdr{};
    hdr.magic   = MAGIC;
    hdr.version = VERSION;
    std::strncpy(hdr.board_name, board_.board_name.c_str(),
                 sizeof(hdr.board_name) - 1);
    hdr.num_cpu_cores     = static_cast<uint8_t>(board_.num_cpu_cores);
    hdr.num_power_rails   = static_cast<uint8_t>(board_.power_rails.size());
    hdr.num_thermal_zones = static_cast<uint8_t>(board_.thermal_zones.size());
    hdr.emc_available     = (fd_emc_ >= 0) ? 1 : 0;
    hdr.fast_hz           = config_.fast_hz;
    hdr.medium_hz         = config_.medium_hz;
    hdr.slow_hz           = config_.slow_hz;
    hdr.num_fast_samples   = fast_samples_.size();
    hdr.num_medium_samples = medium_samples_.size();
    hdr.num_slow_samples   = slow_samples_.size();
    hdr.num_sync_points    = sync_points_.size();

    for (size_t i = 0; i < board_.power_rails.size(); ++i)
        std::strncpy(hdr.power_rail_names[i],
                     board_.power_rails[i].label.c_str(), 23);

    for (size_t i = 0; i < board_.thermal_zones.size(); ++i)
        std::strncpy(hdr.thermal_zone_names[i],
                     board_.thermal_zones[i].name.c_str(), 23);

    fwrite(&hdr, sizeof(hdr), 1, fp);
    fwrite(fast_samples_.data(),   sizeof(FastSample),   fast_samples_.size(),   fp);
    fwrite(medium_samples_.data(), sizeof(MediumSample), medium_samples_.size(), fp);
    fwrite(slow_samples_.data(),   sizeof(SlowSample),   slow_samples_.size(),   fp);
    fwrite(sync_points_.data(),    sizeof(SyncPoint),    sync_points_.size(),    fp);

    fclose(fp);
}

} // namespace fastnvmetrics
