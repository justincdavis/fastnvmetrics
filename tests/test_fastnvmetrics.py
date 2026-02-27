"""Tests for fastnvmetrics Python bindings and trace reader."""

from __future__ import annotations

import struct
import tempfile
import time
from pathlib import Path

import numpy as np
import pytest

from fastnvmetrics import (
    NVMetrics,
    detect_board,
    get_board_config,
    read_trace,
)

# ── Fixtures ────────────────────────────────────────────────────────


@pytest.fixture
def tmp_bin(tmp_path: Path) -> Path:
    """Return a temporary .bin path inside pytest's tmp directory."""
    return tmp_path / "trace.bin"


@pytest.fixture
def short_trace(tmp_bin: Path) -> Path:
    """Record a 200 ms trace and return the path."""
    with NVMetrics(str(tmp_bin)) as ft:
        ft.sync()
        time.sleep(0.1)
        ft.sync()
        time.sleep(0.1)
        ft.sync()
    return tmp_bin


@pytest.fixture
def emc_available() -> bool:
    """Check if EMC debugfs paths are accessible on this system."""
    cfg = detect_board()
    return bool(cfg.emc_actmon_path and cfg.emc_clk_rate_path)


@pytest.fixture
def emc_trace(tmp_path: Path, emc_available: bool) -> Path:
    """Record a 300 ms trace and return the path. Skips if EMC unavailable."""
    if not emc_available:
        pytest.skip("EMC debugfs not accessible (run setup_fastnvmetrics.sh)")
    p = tmp_path / "emc_trace.bin"
    with NVMetrics(str(p)) as ft:
        time.sleep(0.3)
    return p


# ── Board config tests ──────────────────────────────────────────────


class TestBoardConfig:
    def test_detect_board(self) -> None:
        cfg = detect_board()
        assert cfg.board_name
        assert cfg.num_cpu_cores > 0

    def test_get_agx_orin(self) -> None:
        cfg = get_board_config("agx_orin")
        assert cfg.board_name == "agx_orin"
        assert cfg.num_cpu_cores == 12
        assert len(cfg.power_rails) == 4
        assert len(cfg.thermal_zones) == 11

    def test_get_orin_nx(self) -> None:
        cfg = get_board_config("orin_nx")
        assert cfg.board_name == "orin_nx"
        assert cfg.num_cpu_cores == 8
        assert len(cfg.power_rails) >= 3
        assert len(cfg.thermal_zones) >= 10

    def test_unknown_board_raises(self) -> None:
        with pytest.raises(RuntimeError, match="Unknown board"):
            get_board_config("unknown")

    def test_agx_orin_rail_labels(self) -> None:
        cfg = get_board_config("agx_orin")
        labels = [r.label for r in cfg.power_rails]
        assert "VDD_GPU_SOC" in labels
        assert "VDD_CPU_CV" in labels
        assert "VIN_SYS_5V0" in labels
        assert "VDDQ_VDD2_1V8AO" in labels

    def test_agx_orin_thermal_names(self) -> None:
        cfg = get_board_config("agx_orin")
        names = [z.name for z in cfg.thermal_zones]
        assert "cpu-thermal" in names
        assert "gpu-thermal" in names
        assert "tj-thermal" in names

    def test_detect_board_validates_paths(self) -> None:
        cfg = detect_board()
        # After validation, gpu_load_path should be non-empty on Jetson
        assert cfg.gpu_load_path
        # All remaining rails should have valid paths
        for r in cfg.power_rails:
            assert r.voltage_path
            assert r.current_path


# ── Context manager tests ───────────────────────────────────────────


class TestContextManager:
    def test_start_stop(self, tmp_bin: Path) -> None:
        with NVMetrics(str(tmp_bin)) as ft:
            assert ft.is_running
            assert ft.sample_count > 0
        assert not ft.is_running

    def test_collects_samples(self, tmp_bin: Path) -> None:
        with NVMetrics(str(tmp_bin)) as ft:
            time.sleep(0.1)  # 100 ms -> ~100 fast samples
        assert ft.sample_count >= 50

    def test_file_created(self, tmp_bin: Path) -> None:
        with NVMetrics(str(tmp_bin)):
            time.sleep(0.05)
        assert tmp_bin.exists()
        assert tmp_bin.stat().st_size >= 728  # At least header

    def test_exception_in_context_still_writes(self, tmp_bin: Path) -> None:
        with pytest.raises(ValueError):
            with NVMetrics(str(tmp_bin)) as ft:
                time.sleep(0.05)
                raise ValueError("test error")
        # File should still be written despite exception
        assert tmp_bin.exists()
        assert tmp_bin.stat().st_size >= 728


# ── Sync point tests ────────────────────────────────────────────────


class TestSync:
    def test_sync_returns_incrementing_ids(self, tmp_bin: Path) -> None:
        with NVMetrics(str(tmp_bin)) as ft:
            assert ft.sync() == 1
            assert ft.sync() == 2
            assert ft.sync() == 3


# ── Read trace tests ────────────────────────────────────────────────


class TestReadTrace:
    def test_round_trip(self, short_trace: Path) -> None:
        data = read_trace(short_trace)

        # Header metadata
        assert data["board_name"]
        assert data["num_cpu_cores"] > 0
        assert data["fast_hz"] == 1000
        assert data["medium_hz"] == 100
        assert data["slow_hz"] == 10

        # Fast tier arrays
        assert len(data["fast_time_s"]) > 0
        assert data["gpu_load"].dtype == np.uint16
        assert data["cpu_util"].ndim == 2
        assert data["cpu_util"].shape[1] == data["num_cpu_cores"]
        assert data["cpu_aggregate"].dtype == np.float32
        assert data["ram_used_kb"].dtype == np.uint64
        assert data["emc_util"].dtype == np.float32

        # Medium tier
        assert data["medium_time_s"].dtype == np.float64

        # Slow tier
        assert data["slow_time_s"].dtype == np.float64

        # Sync points (forward-filled)
        assert data["sync_id"].dtype == np.uint64
        assert len(data["sync_id"]) == len(data["fast_time_s"])

    def test_bad_magic_raises(self) -> None:
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(b"\x00" * 728)
            path = f.name
        with pytest.raises(ValueError, match="Bad magic"):
            read_trace(path)
        Path(path).unlink()

    def test_file_too_small_raises(self) -> None:
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(b"\x00" * 10)
            path = f.name
        with pytest.raises(ValueError, match="too small"):
            read_trace(path)
        Path(path).unlink()

    def test_version_mismatch_raises(self) -> None:
        """Header with correct magic but wrong version should raise."""
        hdr = bytearray(728)
        # Write correct magic (0x4E564D54) little-endian
        struct.pack_into("<I", hdr, 0, 0x4E564D54)
        # Write bad version
        struct.pack_into("<I", hdr, 4, 99)

        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(bytes(hdr))
            path = f.name
        with pytest.raises(ValueError, match="Unsupported version"):
            read_trace(path)
        Path(path).unlink()


# ── Multi-rate reconstruction tests ─────────────────────────────────


class TestMultiRate:
    def test_all_three_tiers_have_data(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        assert len(data["fast_time_s"]) > 0, "No fast samples"
        assert len(data["medium_time_s"]) > 0, "No medium samples"
        assert len(data["slow_time_s"]) > 0, "No slow samples"

    def test_sample_count_ratios(self, short_trace: Path) -> None:
        """Fast samples should outnumber medium, which outnumber slow."""
        data = read_trace(short_trace)
        n_fast = len(data["fast_time_s"])
        n_med = len(data["medium_time_s"])
        n_slow = len(data["slow_time_s"])

        assert n_fast > n_med, f"fast ({n_fast}) <= medium ({n_med})"
        assert n_med >= n_slow, f"medium ({n_med}) < slow ({n_slow})"

    def test_medium_tier_shapes(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        n_med = len(data["medium_time_s"])
        n_rails = data["num_power_rails"]

        assert data["voltage_mv"].shape == (n_med, n_rails)
        assert data["current_ma"].shape == (n_med, n_rails)
        assert data["power_mw"].shape == (n_med, n_rails)

    def test_slow_tier_shapes(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        n_slow = len(data["slow_time_s"])
        n_zones = data["num_thermal_zones"]

        assert data["temp_c"].shape == (n_slow, n_zones)


# ── Timing tests ────────────────────────────────────────────────────


class TestTiming:
    def test_fast_timestamps_monotonic(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        diffs = np.diff(data["fast_time_s"])
        assert np.all(diffs > 0), "Fast timestamps not monotonically increasing"

    def test_medium_timestamps_monotonic(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        if len(data["medium_time_s"]) > 1:
            diffs = np.diff(data["medium_time_s"])
            assert np.all(diffs > 0), "Medium timestamps not monotonic"

    def test_slow_timestamps_monotonic(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        if len(data["slow_time_s"]) > 1:
            diffs = np.diff(data["slow_time_s"])
            assert np.all(diffs > 0), "Slow timestamps not monotonic"

    def test_fast_interval_approximate(self, short_trace: Path) -> None:
        """Average fast-tier interval should be close to 1 ms."""
        data = read_trace(short_trace)
        times = data["fast_time_s"]
        if len(times) < 20:
            pytest.skip("Too few samples for interval check")

        # Skip warmup samples (first 10)
        diffs = np.diff(times[10:])
        mean_ms = np.mean(diffs) * 1000
        # Allow 0.5–2.0 ms (generous due to scheduling jitter)
        assert 0.5 < mean_ms < 2.0, f"Mean fast interval = {mean_ms:.3f} ms"

    def test_all_tiers_start_near_zero(self, short_trace: Path) -> None:
        """All tier timestamps should start near t=0."""
        data = read_trace(short_trace)
        # Fast tier starts with warmup, so first sample is near 0
        assert data["fast_time_s"][0] < 0.5, "Fast tier start too late"
        if len(data["medium_time_s"]) > 0:
            # Medium waits for warmup (~10 ms), so starts slightly later
            assert data["medium_time_s"][0] < 0.5
        if len(data["slow_time_s"]) > 0:
            assert data["slow_time_s"][0] < 0.5


# ── Data range validation ───────────────────────────────────────────


class TestDataRanges:
    def test_gpu_load_range(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        assert np.all(data["gpu_load"] <= 1000)

    def test_cpu_util_range(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        assert np.all(data["cpu_util"] >= 0.0)
        assert np.all(data["cpu_util"] <= 100.0)

    def test_cpu_aggregate_range(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        assert np.all(data["cpu_aggregate"] >= 0.0)
        assert np.all(data["cpu_aggregate"] <= 100.0)

    def test_ram_nonzero(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        assert np.all(data["ram_used_kb"] > 0), "RAM used should be > 0"
        assert np.all(data["ram_available_kb"] > 0), "RAM available should be > 0"

    def test_power_positive(self, short_trace: Path) -> None:
        """Voltage, current, power should be non-negative."""
        data = read_trace(short_trace)
        if data["num_power_rails"] > 0 and len(data["medium_time_s"]) > 0:
            assert np.all(data["voltage_mv"] >= 0)
            assert np.all(data["current_ma"] >= 0)
            assert np.all(data["power_mw"] >= 0.0)

    def test_temperature_reasonable(self, short_trace: Path) -> None:
        """Active thermal zones should read 10–110 C."""
        data = read_trace(short_trace)
        if data["num_thermal_zones"] > 0 and len(data["slow_time_s"]) > 0:
            temps = data["temp_c"]
            active = temps[temps > 0]  # Skip powered-down zones (0 C)
            if len(active) > 0:
                assert np.all(active >= 10.0), f"Min temp = {active.min()}"
                assert np.all(active <= 110.0), f"Max temp = {active.max()}"


# ── Sync point forward-fill tests ───────────────────────────────────


class TestSyncForwardFill:
    def test_sync_id_matches_fast_length(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        assert len(data["sync_id"]) == len(data["fast_time_s"])

    def test_sync_id_starts_at_zero_before_first_sync(
        self, short_trace: Path
    ) -> None:
        """Samples before the first sync point should have sync_id=0."""
        data = read_trace(short_trace)
        # short_trace fixture calls sync() after warmup, so there should be
        # some samples with sync_id=0 at the start
        assert data["sync_id"][0] == 0

    def test_sync_ids_non_decreasing(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        diffs = np.diff(data["sync_id"].astype(np.int64))
        assert np.all(diffs >= 0), "sync_id not non-decreasing"

    def test_all_sync_ids_present(self, short_trace: Path) -> None:
        """All 3 sync IDs from the fixture should appear."""
        data = read_trace(short_trace)
        unique = set(np.unique(data["sync_id"]).tolist())
        # 0 (before first sync), 1, 2, 3
        assert {1, 2, 3}.issubset(unique), f"Missing sync IDs: {unique}"


# ── Rail and zone name tests ────────────────────────────────────────


class TestNames:
    def test_power_rail_names_in_trace(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        cfg = detect_board()
        expected = [r.label for r in cfg.power_rails]
        assert data["power_rail_names"] == expected

    def test_thermal_zone_names_in_trace(self, short_trace: Path) -> None:
        data = read_trace(short_trace)
        cfg = detect_board()
        expected = [z.name for z in cfg.thermal_zones]
        assert data["thermal_zone_names"] == expected


# ── Custom Hz tests ─────────────────────────────────────────────────


class TestCustomHz:
    def test_custom_rates_in_header(self, tmp_bin: Path) -> None:
        with NVMetrics(str(tmp_bin), fast_hz=500, medium_hz=50, slow_hz=5):
            time.sleep(0.1)
        data = read_trace(tmp_bin)
        assert data["fast_hz"] == 500
        assert data["medium_hz"] == 50
        assert data["slow_hz"] == 5

    def test_custom_rates_affect_sample_count(self, tmp_bin: Path) -> None:
        with NVMetrics(str(tmp_bin), fast_hz=500, medium_hz=50, slow_hz=5):
            time.sleep(0.2)  # 200 ms
        data = read_trace(tmp_bin)
        n_fast = len(data["fast_time_s"])
        # At 500 Hz, 200 ms => ~100 fast samples (plus warmup)
        # Allow wide range due to scheduling
        assert 50 < n_fast < 250, f"Expected ~100 fast samples, got {n_fast}"


# ── Edge case tests ─────────────────────────────────────────────────


class TestEdgeCases:
    def test_very_short_trace(self, tmp_bin: Path) -> None:
        """Start and immediately stop — should still produce a valid file."""
        with NVMetrics(str(tmp_bin)):
            pass  # stop immediately after warmup
        data = read_trace(tmp_bin)
        # Should have at least warmup samples
        assert len(data["fast_time_s"]) >= 10
        assert data["board_name"]

    def test_no_sync_points(self, tmp_bin: Path) -> None:
        """Trace without sync points should have all-zero sync_id."""
        with NVMetrics(str(tmp_bin)):
            time.sleep(0.05)
        data = read_trace(tmp_bin)
        assert np.all(data["sync_id"] == 0)


# ── EMC (memory controller) utilization tests ──────────────────────


class TestEMC:
    """Tests for EMC utilization sampling.

    These require debugfs access (run scripts/setup_fastnvmetrics.sh).
    All tests skip gracefully if EMC paths are not available.
    """

    def test_emc_config_paths(self, emc_available: bool) -> None:
        """Both EMC paths should be set in detected board config."""
        if not emc_available:
            pytest.skip("EMC debugfs not accessible")
        cfg = detect_board()
        assert cfg.emc_actmon_path, "emc_actmon_path empty after validation"
        assert cfg.emc_clk_rate_path, "emc_clk_rate_path empty after validation"

    def test_emc_config_paths_prebaked(self) -> None:
        """Pre-baked configs should have both EMC paths before validation."""
        for name in ("agx_orin", "orin_nx"):
            cfg = get_board_config(name)
            assert cfg.emc_actmon_path, f"{name}: emc_actmon_path not set"
            assert cfg.emc_clk_rate_path, f"{name}: emc_clk_rate_path not set"

    def test_emc_available_in_trace(self, emc_trace: Path) -> None:
        """Trace header should report emc_available=True."""
        data = read_trace(emc_trace)
        assert data["emc_available"] is True, (
            "emc_available is False — EMC fd failed to open or sample_period "
            "not read. Check debugfs permissions and cactmon module."
        )

    def test_emc_util_not_negative(self, emc_trace: Path) -> None:
        """No emc_util sample should be -1.0 (the N/A sentinel).

        This catches the original bug: sysfs_read() uses lseek+read which
        returns ESPIPE on debugfs, causing read_emc() to return -1.0.
        """
        data = read_trace(emc_trace)
        emc = data["emc_util"]
        n_negative = int(np.sum(emc < 0))
        assert n_negative == 0, (
            f"{n_negative}/{len(emc)} samples are -1.0 — debugfs read failing. "
            f"Likely cause: lseek on debugfs returns ESPIPE."
        )

    def test_emc_util_range(self, emc_trace: Path) -> None:
        """All emc_util values should be in [0.0, 100.0]."""
        data = read_trace(emc_trace)
        emc = data["emc_util"]
        assert np.all(emc >= 0.0), f"emc_util min = {emc.min()}"
        assert np.all(emc <= 100.0), f"emc_util max = {emc.max()}"

    def test_emc_util_not_saturated(self, emc_trace: Path) -> None:
        """EMC should NOT be pegged at 100% for every sample.

        This catches the original parsing bug: mc_all returns a raw activity
        counter (~14000), not a percentage. atoi("14000") clamped to 100.0
        means every sample reads as 100% utilization.
        """
        data = read_trace(emc_trace)
        emc = data["emc_util"]
        pct_saturated = np.mean(emc >= 99.0) * 100
        assert pct_saturated < 50.0, (
            f"{pct_saturated:.0f}% of samples >= 99% — likely parsing mc_all "
            f"as a raw integer instead of computing "
            f"mc_all / (clk_rate * sample_period) * 100"
        )

    def test_emc_util_varies(self, emc_trace: Path) -> None:
        """EMC utilization should show some variation across a 300 ms trace.

        A constant value (all identical) suggests the read is returning a
        stale or hardcoded value rather than live hardware data.
        """
        data = read_trace(emc_trace)
        emc = data["emc_util"]
        n_unique = len(np.unique(emc))
        assert n_unique > 1, (
            f"All {len(emc)} emc_util samples are identical ({emc[0]:.4f}) — "
            f"not reading live hardware data"
        )
