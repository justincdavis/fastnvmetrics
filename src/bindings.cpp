#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "fastnvmetrics/fastnvmetrics.hpp"

namespace nb = nanobind;
using namespace fastnvmetrics;

NB_MODULE(_ext, m) {
    m.doc() = "fastnvmetrics — High-frequency Jetson Orin profiler (C++ core)";

    // ── BoardConfig ────────────────────────────────────────────────

    nb::class_<PowerRailConfig>(m, "PowerRailConfig")
        .def_ro("label", &PowerRailConfig::label)
        .def_ro("voltage_path", &PowerRailConfig::voltage_path)
        .def_ro("current_path", &PowerRailConfig::current_path);

    nb::class_<ThermalZoneConfig>(m, "ThermalZoneConfig")
        .def_ro("name", &ThermalZoneConfig::name)
        .def_ro("temp_path", &ThermalZoneConfig::temp_path);

    nb::class_<BoardConfig>(m, "BoardConfig")
        .def_ro("board_name", &BoardConfig::board_name)
        .def_ro("num_cpu_cores", &BoardConfig::num_cpu_cores)
        .def_ro("gpu_load_path", &BoardConfig::gpu_load_path)
        .def_ro("emc_actmon_path", &BoardConfig::emc_actmon_path)
        .def_ro("emc_clk_rate_path", &BoardConfig::emc_clk_rate_path)
        .def_ro("power_rails", &BoardConfig::power_rails)
        .def_ro("thermal_zones", &BoardConfig::thermal_zones);

    m.def("detect_board", &detect_board,
          "Auto-detect the current Jetson board and return its config.");

    m.def("get_board_config", &get_board_config, nb::arg("name"),
          "Get a pre-baked board config by name (\"agx_orin\", \"orin_nx\").");

    // ── NVMetrics (Engine wrapper) ─────────────────────────────────

    nb::class_<Engine>(m, "NVMetrics")
        .def(
            "__init__",
            [](Engine *self, const std::string &output_path,
               uint32_t fast_hz, uint32_t medium_hz, uint32_t slow_hz,
               std::optional<BoardConfig> board) {
                BoardConfig cfg =
                    board.has_value() ? std::move(*board) : detect_board();
                EngineConfig ec{fast_hz, medium_hz, slow_hz};
                new (self) Engine(output_path, cfg, ec);
            },
            nb::arg("output_path"),
            nb::arg("fast_hz")   = 1000,
            nb::arg("medium_hz") = 100,
            nb::arg("slow_hz")   = 10,
            nb::arg("board")     = nb::none(),
            "Create an NVMetrics profiler.\n\n"
            "Parameters\n"
            "----------\n"
            "output_path : str\n"
            "    Path to the binary output file.\n"
            "fast_hz : int\n"
            "    Fast-tier sampling rate (GPU, CPU, RAM, EMC). Default 1000.\n"
            "medium_hz : int\n"
            "    Medium-tier sampling rate (power rails). Default 100.\n"
            "slow_hz : int\n"
            "    Slow-tier sampling rate (thermals). Default 10.\n"
            "board : BoardConfig or None\n"
            "    Board configuration. Auto-detected if None.\n")
        .def(
            "start",
            [](Engine &self) {
                nb::gil_scoped_release release;
                self.start();
            },
            "Start all sampling threads.")
        .def(
            "stop",
            [](Engine &self) {
                nb::gil_scoped_release release;
                self.stop();
            },
            "Stop sampling, join threads, and write the output file.")
        .def(
            "wait_for_warmup",
            [](Engine &self) {
                nb::gil_scoped_release release;
                self.wait_for_warmup();
            },
            "Block until warmup samples have been collected.")
        .def("sync", &Engine::sync,
             "Record a sync point and return its ID (1, 2, 3, ...).")
        .def_prop_ro("sample_count", &Engine::sample_count,
                     "Number of fast-tier samples collected so far.")
        .def_prop_ro("is_running", &Engine::is_running,
                     "Whether the engine is currently sampling.")
        .def(
            "__enter__",
            [](Engine &self) -> Engine & {
                {
                    nb::gil_scoped_release release;
                    self.start();
                    self.wait_for_warmup();
                }
                return self;
            },
            nb::rv_policy::reference,
            "Start the profiler (context manager entry).")
        .def(
            "__exit__",
            [](Engine &self, nb::object, nb::object, nb::object) {
                if (self.is_running()) {
                    nb::gil_scoped_release release;
                    try { self.stop(); } catch (...) {}
                }
            },
            nb::arg("exc_type").none(),
            nb::arg("exc_val").none(),
            nb::arg("exc_tb").none(),
            "Stop the profiler (context manager exit).");
}
