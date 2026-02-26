"""fastnvmetrics — High-frequency Jetson Orin profiler.

Samples GPU utilization, CPU utilization (per-core + aggregate),
EMC utilization, power rails, RAM usage, and thermal sensors
at up to 1 kHz via direct sysfs/procfs reads.

Example
-------
>>> from fastnvmetrics import NVMetrics, read_trace
>>> with NVMetrics("trace.bin") as nv:
...     nv.sync()          # mark phase boundary
...     run_workload()
...     nv.sync()
>>> data = read_trace("trace.bin")
>>> data["gpu_load"]       # numpy array, 0–1000
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import TypedDict

import numpy as np
import numpy.typing as npt

from ._ext import BoardConfig  # noqa: F401
from ._ext import NVMetrics  # noqa: F401
from ._ext import PowerRailConfig  # noqa: F401
from ._ext import ThermalZoneConfig  # noqa: F401
from ._ext import detect_board  # noqa: F401
from ._ext import get_board_config  # noqa: F401

__all__ = [
    "NVMetrics",
    "BoardConfig",
    "PowerRailConfig",
    "ThermalZoneConfig",
    "detect_board",
    "get_board_config",
    "TraceData",
    "read_trace",
]

# ── Binary format constants ─────────────────────────────────────────

_MAGIC = 0x4E564D54
_VERSION = 1
_HEADER_SIZE = 728
_MAX_CPU_CORES = 16
_MAX_POWER_RAILS = 8
_MAX_THERMAL_ZONES = 16

# Struct sizes (must match C++ packed structs)
_FAST_SAMPLE_SIZE = 98
_MEDIUM_SAMPLE_SIZE = 104
_SLOW_SAMPLE_SIZE = 72
_SYNC_POINT_SIZE = 16


class TraceData(TypedDict):
    """Parsed trace file contents."""

    # Header metadata
    board_name: str
    num_cpu_cores: int
    num_power_rails: int
    num_thermal_zones: int
    emc_available: bool
    fast_hz: int
    medium_hz: int
    slow_hz: int
    power_rail_names: list[str]
    thermal_zone_names: list[str]

    # Fast tier (1 kHz)
    fast_time_s: npt.NDArray[np.float64]
    gpu_load: npt.NDArray[np.uint16]
    cpu_util: npt.NDArray[np.float32]       # shape (N, num_cpu_cores)
    cpu_aggregate: npt.NDArray[np.float32]
    ram_used_kb: npt.NDArray[np.uint64]
    ram_available_kb: npt.NDArray[np.uint64]
    emc_util: npt.NDArray[np.float32]

    # Medium tier (100 Hz)
    medium_time_s: npt.NDArray[np.float64]
    voltage_mv: npt.NDArray[np.uint32]      # shape (N, num_power_rails)
    current_ma: npt.NDArray[np.uint32]      # shape (N, num_power_rails)
    power_mw: npt.NDArray[np.float32]       # shape (N, num_power_rails)

    # Slow tier (10 Hz)
    slow_time_s: npt.NDArray[np.float64]
    temp_c: npt.NDArray[np.float32]         # shape (N, num_thermal_zones)

    # Sync points
    sync_id: npt.NDArray[np.uint64]         # per fast sample (forward-filled)


def read_trace(path: str | Path) -> TraceData:
    """Read a fastnvmetrics binary trace file.

    Parameters
    ----------
    path : str or Path
        Path to the .bin trace file.

    Returns
    -------
    TraceData
        Dictionary of numpy arrays and metadata.
    """
    data = Path(path).read_bytes()
    if len(data) < _HEADER_SIZE:
        raise ValueError(f"File too small ({len(data)} bytes)")

    # ── Parse header ────────────────────────────────────────────────
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != _MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:08X} (expected 0x{_MAGIC:08X})")
    if version != _VERSION:
        raise ValueError(f"Unsupported version: {version}")

    board_name = data[8:40].split(b"\x00", 1)[0].decode("utf-8")
    (num_cpu, num_rails, num_zones, emc_avail) = struct.unpack_from(
        "<BBBB", data, 40
    )
    (fast_hz, medium_hz, slow_hz) = struct.unpack_from("<III", data, 44)
    (n_fast, n_med, n_slow, n_sync) = struct.unpack_from("<QQQQ", data, 56)

    # Rail names: offset 88, 8 × 24 = 192 bytes
    rail_names: list[str] = []
    for i in range(num_rails):
        off = 88 + i * 24
        name = data[off : off + 24].split(b"\x00", 1)[0].decode("utf-8")
        rail_names.append(name)

    # Zone names: offset 88 + 192 = 280, 16 × 24 = 384 bytes
    zone_names: list[str] = []
    for i in range(num_zones):
        off = 280 + i * 24
        name = data[off : off + 24].split(b"\x00", 1)[0].decode("utf-8")
        zone_names.append(name)

    # ── Parse samples ───────────────────────────────────────────────
    offset = _HEADER_SIZE

    # Fast tier
    fast_bytes = n_fast * _FAST_SAMPLE_SIZE
    fast_dt = np.dtype(
        [
            ("time_s", "<f8"),
            ("gpu_load", "<u2"),
            ("cpu_util", "<f4", (_MAX_CPU_CORES,)),
            ("cpu_aggregate", "<f4"),
            ("ram_used_kb", "<u8"),
            ("ram_available_kb", "<u8"),
            ("emc_util", "<f4"),
        ]
    )
    fast_arr = np.frombuffer(data, dtype=fast_dt, count=n_fast, offset=offset)
    offset += fast_bytes

    # Medium tier
    med_bytes = n_med * _MEDIUM_SAMPLE_SIZE
    med_dt = np.dtype(
        [
            ("time_s", "<f8"),
            ("voltage_mv", "<u4", (_MAX_POWER_RAILS,)),
            ("current_ma", "<u4", (_MAX_POWER_RAILS,)),
            ("power_mw", "<f4", (_MAX_POWER_RAILS,)),
        ]
    )
    med_arr = np.frombuffer(data, dtype=med_dt, count=n_med, offset=offset)
    offset += med_bytes

    # Slow tier
    slow_bytes = n_slow * _SLOW_SAMPLE_SIZE
    slow_dt = np.dtype(
        [("time_s", "<f8"), ("temp_c", "<f4", (_MAX_THERMAL_ZONES,))]
    )
    slow_arr = np.frombuffer(data, dtype=slow_dt, count=n_slow, offset=offset)
    offset += slow_bytes

    # Sync points
    sync_dt = np.dtype([("sync_id", "<u8"), ("fast_sample_idx", "<u8")])
    sync_arr = np.frombuffer(data, dtype=sync_dt, count=n_sync, offset=offset)

    # Forward-fill sync_id into per-fast-sample array
    sync_ids = np.zeros(n_fast, dtype=np.uint64)
    for sp in sync_arr:
        idx = int(sp["fast_sample_idx"])
        if idx < n_fast:
            sync_ids[idx:] = sp["sync_id"]

    return TraceData(
        board_name=board_name,
        num_cpu_cores=num_cpu,
        num_power_rails=num_rails,
        num_thermal_zones=num_zones,
        emc_available=bool(emc_avail),
        fast_hz=fast_hz,
        medium_hz=medium_hz,
        slow_hz=slow_hz,
        power_rail_names=rail_names,
        thermal_zone_names=zone_names,
        # Fast tier — trim CPU/power/thermal arrays to actual count
        fast_time_s=fast_arr["time_s"].copy(),
        gpu_load=fast_arr["gpu_load"].copy(),
        cpu_util=fast_arr["cpu_util"][:, :num_cpu].copy(),
        cpu_aggregate=fast_arr["cpu_aggregate"].copy(),
        ram_used_kb=fast_arr["ram_used_kb"].copy(),
        ram_available_kb=fast_arr["ram_available_kb"].copy(),
        emc_util=fast_arr["emc_util"].copy(),
        # Medium tier
        medium_time_s=med_arr["time_s"].copy() if n_med else np.array([], dtype=np.float64),
        voltage_mv=med_arr["voltage_mv"][:, :num_rails].copy() if n_med else np.empty((0, num_rails), dtype=np.uint32),
        current_ma=med_arr["current_ma"][:, :num_rails].copy() if n_med else np.empty((0, num_rails), dtype=np.uint32),
        power_mw=med_arr["power_mw"][:, :num_rails].copy() if n_med else np.empty((0, num_rails), dtype=np.float32),
        # Slow tier
        slow_time_s=slow_arr["time_s"].copy() if n_slow else np.array([], dtype=np.float64),
        temp_c=slow_arr["temp_c"][:, :num_zones].copy() if n_slow else np.empty((0, num_zones), dtype=np.float32),
        # Sync points
        sync_id=sync_ids,
    )
