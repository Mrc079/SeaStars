"""STM32 text protocol parsing and validated command construction."""

from __future__ import annotations

from dataclasses import dataclass
import re


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class PositionEvent:
    tank1: int
    tank2: int
    zeroed1: bool | None = None
    zeroed2: bool | None = None


@dataclass(frozen=True)
class ImuEvent:
    heading: float
    roll: float
    pitch: float
    calibration: int


@dataclass(frozen=True)
class ImuStatusEvent:
    model: str | None
    address: int | None
    online: bool


@dataclass(frozen=True)
class DepthEvent:
    depth_m: float
    temperature_c: float
    pressure_mbar: float
    sensor_depth_m: float | None
    surface_calibrated: bool


@dataclass(frozen=True)
class MissionEvent:
    state: str
    elapsed_ms: int
    fault: str
    active: bool


@dataclass(frozen=True)
class MessageEvent:
    text: str


ProtocolEvent = PositionEvent | ImuEvent | ImuStatusEvent | DepthEvent | MissionEvent | MessageEvent

_POSITION = re.compile(
    r"\bPOS1:\s*(-?\d+)\s+POS2:\s*(-?\d+)"
    r"(?:\s+Z1:\s*([01])\s+Z2:\s*([01]))?\b",
    re.IGNORECASE,
)
_IMU = re.compile(
    r"\bIMU\s+H:\s*(-?\d+(?:\.\d+)?)\s+R:\s*(-?\d+(?:\.\d+)?)"
    r"\s+P:\s*(-?\d+(?:\.\d+)?)\s+C:\s*(\d+)\b",
    re.IGNORECASE,
)
_IMU_STATUS = re.compile(
    r"\bSENSOR\s+IMU:\s*([A-Z0-9_]+)(?:\s+ADDR:\s*0X([0-9A-F]{2}))?\b",
    re.IGNORECASE,
)
_DEPTH = re.compile(
    r"\bDEPTH\s+M:\s*(-?\d+(?:\.\d+)?)\s+T:\s*(-?\d+(?:\.\d+)?)"
    r"\s+P:\s*(\d+(?:\.\d+)?)(?:\s+S:\s*(-?\d+(?:\.\d+)?))?"
    r"\s+Z:\s*([01])\b",
    re.IGNORECASE,
)
_MISSION = re.compile(
    r"\bAUTO\s+STATE:\s*([A-Z0-9_]+)\s+ELAPSED:\s*(\d+)"
    r"\s+FAULT:\s*([A-Z0-9_]+)\s+ACTIVE:\s*([01])\b",
    re.IGNORECASE,
)


def parse_line(line: str) -> ProtocolEvent:
    text = line.strip()
    match = _IMU_STATUS.search(text)
    if match:
        token = match.group(1).upper()
        online = token not in {"NOT_FOUND", "LOST", "NONE"}
        return ImuStatusEvent(
            model=token if online else None,
            address=int(match.group(2), 16) if match.group(2) is not None else None,
            online=online,
        )
    match = _POSITION.search(text)
    if match:
        return PositionEvent(
            int(match.group(1)),
            int(match.group(2)),
            match.group(3) == "1" if match.group(3) is not None else None,
            match.group(4) == "1" if match.group(4) is not None else None,
        )
    match = _IMU.search(text)
    if match:
        calibration = int(match.group(4))
        if not 0 <= calibration <= 255:
            return MessageEvent(text)
        return ImuEvent(
            float(match.group(1)),
            float(match.group(2)),
            float(match.group(3)),
            calibration,
        )
    match = _DEPTH.search(text)
    if match:
        return DepthEvent(
            depth_m=float(match.group(1)),
            temperature_c=float(match.group(2)),
            pressure_mbar=float(match.group(3)),
            sensor_depth_m=float(match.group(4)) if match.group(4) is not None else None,
            surface_calibrated=match.group(5) == "1",
        )
    match = _MISSION.search(text)
    if match:
        return MissionEvent(
            state=match.group(1).upper(),
            elapsed_ms=int(match.group(2)),
            fault=match.group(3).upper(),
            active=match.group(4) == "1",
        )
    return MessageEvent(text)


def _device(index: int) -> int:
    if index not in (1, 2):
        raise ProtocolError("Cihaz numarası 1 veya 2 olmalı")
    return index


def move_tank(index: int, steps: int) -> str:
    _device(index)
    if isinstance(steps, bool) or not isinstance(steps, int):
        raise ProtocolError("Adım değeri tam sayı olmalı")
    if abs(steps) > 1_000_000:
        raise ProtocolError("Adım değeri güvenli sınırın dışında")
    return f"S{index} {steps}"


def set_tank_speed(index: int, steps_per_second: int) -> str:
    _device(index)
    if not 50 <= steps_per_second <= 2_000:
        raise ProtocolError("Tank hızı 50–2000 adım/s aralığında olmalı")
    return f"V{index} {steps_per_second}"


def zero_tank(index: int) -> str:
    _device(index)
    return f"ZERO{index}"


def set_device_position(index: int, position: int) -> str:
    _device(index)
    return f"SETPOS{index} {int(position)}"


def set_thruster(index: int, percent: float, power_limit: float = 100.0) -> str:
    _device(index)
    if not -100 <= percent <= 100:
        raise ProtocolError("İtici gücü -100 ile 100 arasında olmalı")
    limited = max(-power_limit, min(power_limit, float(percent)))
    return f"T{index} {int(round(limited * 2))}"


def configure(name: str, value: int | float | str) -> str:
    allowed = {
        "DEPTH_MM",
        "MAX_DEPTH_MM",
        "DEPTH_TOL_MM",
        "OFFSET_MM",
        "DENSITY",
        "HEADING_OFFSET_CDEG",
        "HEADING_SIGN",
        "BALANCE_AXIS",
        "BALANCE_SIGN",
        "LEVEL_KP_X100",
        "MAX_BALANCE_PCT",
        "ROUTE",
        "DIVE_BALLAST_PCT",
        "HOVER_BALLAST_PCT",
        "DIVE_MS",
        "HOVER_SETTLE_MS",
        "TEST_FORWARD_MS",
        "TEST_FORWARD_PCT",
        "AUTO_DELAY_MS",
        "AUTO_ZERO_TANKS",
        "STRAIGHT_MS",
        "FORWARD_PCT",
        "CIRCLE_FORWARD_PCT",
        "CIRCLE_TURN_PCT",
        "HEADING_KP_X100",
        "BALLAST_GAIN_X100",
        "TANK1_FULL",
        "TANK2_FULL",
    }
    key = name.strip().upper()
    if key not in allowed:
        raise ProtocolError(f"Bilinmeyen STM32 ayarı: {name}")
    text = str(value).strip().upper()
    if not text or len(text) > 20 or not re.fullmatch(r"[-A-Z0-9.]+", text):
        raise ProtocolError("Geçersiz STM32 ayar değeri")
    return f"CFG {key} {text}"


def turn_relative(degrees: float) -> str:
    if not -180.0 <= degrees <= 180.0 or abs(degrees) < 1.0:
        raise ProtocolError("Dönüş açısı -180 ile 180 derece arasında olmalı")
    return f"TURN {int(round(degrees))}"
