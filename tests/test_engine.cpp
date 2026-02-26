#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <time.h>

#include "fastnvmetrics/fastnvmetrics.hpp"

using namespace fastnvmetrics;

// ── Helper: skip tests that require Jetson hardware ───────────────

static bool have_jetson() {
    try {
        detect_board();
        return true;
    } catch (...) {
        return false;
    }
}

#define SKIP_IF_NO_JETSON()                                       \
    do {                                                          \
        if (!have_jetson())                                       \
            GTEST_SKIP() << "Not running on a recognized Jetson"; \
    } while (0)

// ── Struct layout tests ────────────────────────────────────────────

TEST(StructLayout, FileHeader) { EXPECT_EQ(sizeof(FileHeader), 728); }

TEST(StructLayout, FastSample) { EXPECT_EQ(sizeof(FastSample), 98); }

TEST(StructLayout, MediumSample) { EXPECT_EQ(sizeof(MediumSample), 104); }

TEST(StructLayout, SlowSample) { EXPECT_EQ(sizeof(SlowSample), 72); }

TEST(StructLayout, SyncPoint) { EXPECT_EQ(sizeof(SyncPoint), 16); }

// ── FileHeader field offset/content tests ─────────────────────────

TEST(StructLayout, FileHeaderFieldOffsets) {
    FileHeader hdr{};
    auto base = reinterpret_cast<const char *>(&hdr);

    // magic at 0, version at 4
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.magic) - base, 0);
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.version) - base, 4);
    // board_name at 8 (32 bytes)
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.board_name) - base, 8);
    // num_cpu_cores at 40
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.num_cpu_cores) - base, 40);
    // sampling rates at 44
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.fast_hz) - base, 44);
    // sample counts at 56
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.num_fast_samples) - base, 56);
    // power_rail_names at 88 (8 × 24 = 192 bytes)
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.power_rail_names) - base, 88);
    // thermal_zone_names at 280 (16 × 24 = 384 bytes)
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.thermal_zone_names) - base, 280);
    // reserved at 664 (64 bytes → total 728)
    EXPECT_EQ(reinterpret_cast<const char *>(&hdr.reserved) - base, 664);
}

TEST(StructLayout, FastSampleFieldOffsets) {
    FastSample s{};
    auto base = reinterpret_cast<const char *>(&s);

    EXPECT_EQ(reinterpret_cast<const char *>(&s.time_s) - base, 0);         // f8
    EXPECT_EQ(reinterpret_cast<const char *>(&s.gpu_load) - base, 8);       // u2
    EXPECT_EQ(reinterpret_cast<const char *>(&s.cpu_util) - base, 10);      // f4 × 16
    EXPECT_EQ(reinterpret_cast<const char *>(&s.cpu_aggregate) - base, 74); // f4
    EXPECT_EQ(reinterpret_cast<const char *>(&s.ram_used_kb) - base, 78);   // u8
    EXPECT_EQ(reinterpret_cast<const char *>(&s.ram_available_kb) - base, 86); // u8
    EXPECT_EQ(reinterpret_cast<const char *>(&s.emc_util) - base, 94);      // f4
}

// ── Board config tests ─────────────────────────────────────────────

TEST(BoardConfig, GetAgxOrin) {
    auto cfg = get_board_config("agx_orin");
    EXPECT_EQ(cfg.board_name, "agx_orin");
    EXPECT_EQ(cfg.num_cpu_cores, 12);
    EXPECT_EQ(cfg.power_rails.size(), 4);
    EXPECT_EQ(cfg.thermal_zones.size(), 11);
    EXPECT_FALSE(cfg.gpu_load_path.empty());
    EXPECT_FALSE(cfg.emc_actmon_path.empty());
}

TEST(BoardConfig, GetOrinNx) {
    auto cfg = get_board_config("orin_nx");
    EXPECT_EQ(cfg.board_name, "orin_nx");
    EXPECT_EQ(cfg.num_cpu_cores, 8);
    EXPECT_GE(cfg.power_rails.size(), 3);
    EXPECT_GE(cfg.thermal_zones.size(), 10);
}

TEST(BoardConfig, UnknownBoardThrows) {
    EXPECT_THROW(get_board_config("unknown"), std::runtime_error);
}

TEST(BoardConfig, DetectBoard) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    EXPECT_FALSE(cfg.board_name.empty());
    EXPECT_GT(cfg.num_cpu_cores, 0);
    EXPECT_LE(cfg.num_cpu_cores, MAX_CPU_CORES);
}

TEST(BoardConfig, AgxOrinRailLabels) {
    auto cfg = get_board_config("agx_orin");
    ASSERT_EQ(cfg.power_rails.size(), 4);
    EXPECT_EQ(cfg.power_rails[0].label, "VDD_GPU_SOC");
    EXPECT_EQ(cfg.power_rails[1].label, "VDD_CPU_CV");
    EXPECT_EQ(cfg.power_rails[2].label, "VIN_SYS_5V0");
    EXPECT_EQ(cfg.power_rails[3].label, "VDDQ_VDD2_1V8AO");
}

TEST(BoardConfig, AgxOrinThermalZoneNames) {
    auto cfg = get_board_config("agx_orin");
    ASSERT_EQ(cfg.thermal_zones.size(), 11);
    EXPECT_EQ(cfg.thermal_zones[0].name, "cpu-thermal");
    EXPECT_EQ(cfg.thermal_zones[1].name, "gpu-thermal");
    EXPECT_EQ(cfg.thermal_zones[8].name, "tj-thermal");
}

TEST(BoardConfig, PathsNonEmpty) {
    // Pre-baked configs should have all paths filled
    for (const auto &name : {"agx_orin", "orin_nx"}) {
        auto cfg = get_board_config(name);
        EXPECT_FALSE(cfg.gpu_load_path.empty()) << name;
        EXPECT_FALSE(cfg.emc_actmon_path.empty()) << name;
        for (const auto &r : cfg.power_rails) {
            EXPECT_FALSE(r.voltage_path.empty()) << name << " " << r.label;
            EXPECT_FALSE(r.current_path.empty()) << name << " " << r.label;
        }
        for (const auto &z : cfg.thermal_zones) {
            EXPECT_FALSE(z.temp_path.empty()) << name << " " << z.name;
        }
    }
}

// ── Engine config validation ──────────────────────────────────────

TEST(EngineConfig, BadCoreCountThrows) {
    BoardConfig cfg;
    cfg.board_name = "test";
    cfg.num_cpu_cores = 0;  // Invalid
    cfg.gpu_load_path = "/dev/null";
    EXPECT_THROW(Engine("/tmp/ft_test_bad.bin", cfg), std::invalid_argument);
}

TEST(EngineConfig, TooManyCoresThrows) {
    BoardConfig cfg;
    cfg.board_name = "test";
    cfg.num_cpu_cores = MAX_CPU_CORES + 1;  // Over limit
    cfg.gpu_load_path = "/dev/null";
    EXPECT_THROW(Engine("/tmp/ft_test_bad.bin", cfg), std::invalid_argument);
}

TEST(EngineConfig, TooManyRailsThrows) {
    BoardConfig cfg;
    cfg.board_name = "test";
    cfg.num_cpu_cores = 4;
    cfg.gpu_load_path = "/dev/null";
    for (int i = 0; i < MAX_POWER_RAILS + 1; ++i)
        cfg.power_rails.push_back({"rail", "/dev/null", "/dev/null"});
    EXPECT_THROW(Engine("/tmp/ft_test_bad.bin", cfg), std::invalid_argument);
}

TEST(EngineConfig, TooManyZonesThrows) {
    BoardConfig cfg;
    cfg.board_name = "test";
    cfg.num_cpu_cores = 4;
    cfg.gpu_load_path = "/dev/null";
    for (int i = 0; i < MAX_THERMAL_ZONES + 1; ++i)
        cfg.thermal_zones.push_back({"zone", "/dev/null"});
    EXPECT_THROW(Engine("/tmp/ft_test_bad.bin", cfg), std::invalid_argument);
}

// ── Engine lifecycle tests ─────────────────────────────────────────

TEST(Engine, ConstructDestruct) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    Engine e("/tmp/fastnvmetrics_test.bin", cfg);
    EXPECT_FALSE(e.is_running());
    EXPECT_EQ(e.sample_count(), 0);
}

TEST(Engine, StartStopCycle) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    Engine e("/tmp/fastnvmetrics_test.bin", cfg);
    e.start();
    EXPECT_TRUE(e.is_running());
    e.wait_for_warmup();
    EXPECT_GT(e.sample_count(), 0);

    // Collect for 100 ms
    struct timespec req = {0, 100'000'000};
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);

    e.stop();
    EXPECT_FALSE(e.is_running());
    EXPECT_GT(e.sample_count(), 50);  // ~100 samples at 1 kHz
}

TEST(Engine, SyncPoints) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    Engine e("/tmp/fastnvmetrics_test_sync.bin", cfg);
    e.start();
    e.wait_for_warmup();

    EXPECT_EQ(e.sync(), 1);
    EXPECT_EQ(e.sync(), 2);
    EXPECT_EQ(e.sync(), 3);

    e.stop();
}

TEST(Engine, DoubleStartThrows) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    Engine e("/tmp/fastnvmetrics_test_ds.bin", cfg);
    e.start();
    EXPECT_THROW(e.start(), std::runtime_error);
    e.stop();
}

TEST(Engine, DestructorStopsRunningEngine) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    {
        Engine e("/tmp/fastnvmetrics_test_dtor.bin", cfg);
        e.start();
        e.wait_for_warmup();
        // Destructor should call stop() without crashing
    }
    // Verify the file was written
    std::ifstream f("/tmp/fastnvmetrics_test_dtor.bin", std::ios::binary);
    EXPECT_TRUE(f.good());
    f.seekg(0, std::ios::end);
    EXPECT_GE(f.tellg(), static_cast<std::streamoff>(sizeof(FileHeader)));
}

TEST(Engine, WarmupCompletesQuickly) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    Engine e("/tmp/fastnvmetrics_test_warmup.bin", cfg);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    e.start();
    e.wait_for_warmup();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (t1.tv_nsec - t0.tv_nsec) / 1e6;
    // Warmup = 10 samples at 1 kHz ≈ 10 ms, allow generous 200 ms budget
    EXPECT_LT(elapsed_ms, 200.0) << "Warmup took " << elapsed_ms << " ms";

    e.stop();
}

// ── Trace file format tests ───────────────────────────────────────

TEST(TraceFile, HeaderContent) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    const char *path = "/tmp/fastnvmetrics_test_hdr.bin";

    {
        Engine e(path, cfg, {1000, 100, 10});
        e.start();
        e.wait_for_warmup();

        struct timespec req = {0, 100'000'000};  // 100 ms
        clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);

        e.stop();
    }

    // Read and verify header
    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good());

    FileHeader hdr{};
    f.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    ASSERT_TRUE(f.good());

    EXPECT_EQ(hdr.magic, MAGIC);
    EXPECT_EQ(hdr.version, VERSION);
    EXPECT_STREQ(hdr.board_name, cfg.board_name.c_str());
    EXPECT_EQ(hdr.num_cpu_cores, cfg.num_cpu_cores);
    EXPECT_EQ(hdr.num_power_rails, cfg.power_rails.size());
    EXPECT_EQ(hdr.num_thermal_zones, cfg.thermal_zones.size());
    EXPECT_EQ(hdr.fast_hz, 1000);
    EXPECT_EQ(hdr.medium_hz, 100);
    EXPECT_EQ(hdr.slow_hz, 10);
    EXPECT_GT(hdr.num_fast_samples, 50);    // ~100 at 1 kHz
    EXPECT_GT(hdr.num_medium_samples, 5);   // ~10 at 100 Hz
    EXPECT_GT(hdr.num_slow_samples, 0);     // ~1 at 10 Hz
    EXPECT_EQ(hdr.num_sync_points, 0);

    // Verify rail names match config
    for (size_t i = 0; i < cfg.power_rails.size(); ++i)
        EXPECT_STREQ(hdr.power_rail_names[i], cfg.power_rails[i].label.c_str());

    // Verify zone names match config
    for (size_t i = 0; i < cfg.thermal_zones.size(); ++i)
        EXPECT_STREQ(hdr.thermal_zone_names[i], cfg.thermal_zones[i].name.c_str());
}

TEST(TraceFile, SyncPointsInFile) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    const char *path = "/tmp/fastnvmetrics_test_sp.bin";

    {
        Engine e(path, cfg);
        e.start();
        e.wait_for_warmup();
        e.sync();
        struct timespec req = {0, 20'000'000};  // 20 ms
        clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
        e.sync();
        e.sync();
        e.stop();
    }

    // Read header to get counts
    std::ifstream f(path, std::ios::binary);
    FileHeader hdr{};
    f.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    ASSERT_TRUE(f.good());

    EXPECT_EQ(hdr.num_sync_points, 3);

    // Seek to sync point section
    auto sync_offset = sizeof(FileHeader) +
                       hdr.num_fast_samples * sizeof(FastSample) +
                       hdr.num_medium_samples * sizeof(MediumSample) +
                       hdr.num_slow_samples * sizeof(SlowSample);
    f.seekg(sync_offset);

    SyncPoint sp[3];
    f.read(reinterpret_cast<char *>(sp), 3 * sizeof(SyncPoint));
    ASSERT_TRUE(f.good());

    EXPECT_EQ(sp[0].sync_id, 1);
    EXPECT_EQ(sp[1].sync_id, 2);
    EXPECT_EQ(sp[2].sync_id, 3);

    // Indices should be monotonically non-decreasing
    EXPECT_LE(sp[0].fast_sample_idx, sp[1].fast_sample_idx);
    EXPECT_LE(sp[1].fast_sample_idx, sp[2].fast_sample_idx);
}

TEST(TraceFile, FileSize) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    const char *path = "/tmp/fastnvmetrics_test_sz.bin";

    {
        Engine e(path, cfg);
        e.start();
        e.wait_for_warmup();
        struct timespec req = {0, 50'000'000};  // 50 ms
        clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
        e.stop();
    }

    // Read header
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    auto file_size = f.tellg();
    f.seekg(0);
    FileHeader hdr{};
    f.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));

    // Expected file size
    auto expected = sizeof(FileHeader) +
                    hdr.num_fast_samples * sizeof(FastSample) +
                    hdr.num_medium_samples * sizeof(MediumSample) +
                    hdr.num_slow_samples * sizeof(SlowSample) +
                    hdr.num_sync_points * sizeof(SyncPoint);

    EXPECT_EQ(static_cast<size_t>(file_size), expected);
}

TEST(TraceFile, FastSampleTimestampsMonotonic) {
    SKIP_IF_NO_JETSON();
    auto cfg = detect_board();
    const char *path = "/tmp/fastnvmetrics_test_mono.bin";

    {
        Engine e(path, cfg);
        e.start();
        e.wait_for_warmup();
        struct timespec req = {0, 50'000'000};  // 50 ms
        clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
        e.stop();
    }

    std::ifstream f(path, std::ios::binary);
    FileHeader hdr{};
    f.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));

    double prev_time = -1.0;
    for (uint64_t i = 0; i < hdr.num_fast_samples; ++i) {
        FastSample s{};
        f.read(reinterpret_cast<char *>(&s), sizeof(s));
        ASSERT_TRUE(f.good()) << "Failed to read fast sample " << i;
        EXPECT_GT(s.time_s, prev_time)
            << "Non-monotonic at sample " << i
            << " (" << s.time_s << " <= " << prev_time << ")";
        prev_time = s.time_s;
    }
}
