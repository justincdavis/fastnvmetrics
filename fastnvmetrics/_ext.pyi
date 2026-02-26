"""Type stubs for the fastnvmetrics C++ extension module."""

from __future__ import annotations


class PowerRailConfig:
    """Configuration for a single INA3221 power rail."""

    @property
    def label(self) -> str: ...
    @property
    def voltage_path(self) -> str: ...
    @property
    def current_path(self) -> str: ...

class ThermalZoneConfig:
    """Configuration for a single thermal zone."""

    @property
    def name(self) -> str: ...
    @property
    def temp_path(self) -> str: ...

class BoardConfig:
    """Detected or pre-baked board configuration."""

    @property
    def board_name(self) -> str: ...
    @property
    def num_cpu_cores(self) -> int: ...
    @property
    def gpu_load_path(self) -> str: ...
    @property
    def emc_actmon_path(self) -> str: ...
    @property
    def power_rails(self) -> list[PowerRailConfig]: ...
    @property
    def thermal_zones(self) -> list[ThermalZoneConfig]: ...

def detect_board() -> BoardConfig:
    """Auto-detect the current Jetson board and return its config."""
    ...

def get_board_config(name: str) -> BoardConfig:
    """Get a pre-baked board config by name ("agx_orin", "orin_nx")."""
    ...

class NVMetrics:
    """High-frequency Jetson Orin profiler.

    Samples GPU, CPU, EMC, power, RAM, and thermal metrics at
    configurable rates (default: 1 kHz / 100 Hz / 10 Hz).

    Parameters
    ----------
    output_path : str
        Path to the binary output file.
    fast_hz : int
        Fast-tier sampling rate (GPU, CPU, RAM, EMC). Default 1000.
    medium_hz : int
        Medium-tier sampling rate (power rails). Default 100.
    slow_hz : int
        Slow-tier sampling rate (thermals). Default 10.
    board : BoardConfig or None
        Board configuration. Auto-detected if None.
    """

    def __init__(
        self,
        output_path: str,
        fast_hz: int = 1000,
        medium_hz: int = 100,
        slow_hz: int = 10,
        board: BoardConfig | None = None,
    ) -> None: ...
    def start(self) -> None:
        """Start all sampling threads."""
        ...
    def stop(self) -> None:
        """Stop sampling, join threads, and write the output file."""
        ...
    def wait_for_warmup(self) -> None:
        """Block until warmup samples have been collected."""
        ...
    def sync(self) -> int:
        """Record a sync point and return its ID (1, 2, 3, ...)."""
        ...
    @property
    def sample_count(self) -> int:
        """Number of fast-tier samples collected so far."""
        ...
    @property
    def is_running(self) -> bool:
        """Whether the engine is currently sampling."""
        ...
    def __enter__(self) -> NVMetrics:
        """Start the profiler (context manager entry)."""
        ...
    def __exit__(self, *args: object) -> None:
        """Stop the profiler (context manager exit)."""
        ...
