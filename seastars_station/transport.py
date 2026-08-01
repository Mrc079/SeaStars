"""Thread-safe hardware and simulator transports."""

from __future__ import annotations

from collections.abc import Callable
import math
import threading
import time
from typing import Protocol


LineCallback = Callable[[str], None]
StateCallback = Callable[[bool, str | None], None]


class Transport(Protocol):
    @property
    def connected(self) -> bool: ...

    @property
    def port(self) -> str | None: ...

    def list_ports(self) -> list[str]: ...

    def connect(self, port: str) -> None: ...

    def disconnect(self) -> None: ...

    def send(self, command: str) -> bool: ...


class SerialTransport:
    """pyserial adapter. pyserial is imported only when hardware is used."""

    def __init__(self, on_line: LineCallback, on_state: StateCallback, baud: int = 115_200):
        self._on_line = on_line
        self._on_state = on_state
        self._baud = baud
        self._serial = None
        self._port: str | None = None
        self._stop = threading.Event()
        self._reader: threading.Thread | None = None
        self._io_lock = threading.RLock()

    @staticmethod
    def _module():
        try:
            import serial  # type: ignore
            import serial.tools.list_ports  # type: ignore
        except ImportError as exc:
            raise RuntimeError("pyserial kurulu değil; `pip install -r requirements.txt` çalıştırın") from exc
        return serial

    @property
    def connected(self) -> bool:
        with self._io_lock:
            return bool(self._serial is not None and self._serial.is_open)

    @property
    def port(self) -> str | None:
        return self._port

    def list_ports(self) -> list[str]:
        serial = self._module()
        return [item.device for item in serial.tools.list_ports.comports()]

    def connect(self, port: str) -> None:
        if not port or len(port) > 100:
            raise ValueError("Geçerli bir seri port seçin")
        self.disconnect()
        serial = self._module()
        try:
            device = serial.Serial(port, self._baud, timeout=0.1, write_timeout=0.5)
        except Exception as exc:
            raise RuntimeError(f"{port} açılamadı: {exc}") from exc
        with self._io_lock:
            self._serial = device
            self._port = port
            self._stop.clear()
            self._reader = threading.Thread(target=self._read_loop, name="serial-reader", daemon=True)
            self._reader.start()
        self._on_state(True, None)
        # Ask an already-running STM32 to repeat its READY banner. Relying on a
        # reset-on-open side effect is unreliable with the ST-Link virtual COM port.
        self.send("HELLO")

    def disconnect(self) -> None:
        self._stop.set()
        with self._io_lock:
            device, self._serial = self._serial, None
            self._port = None
        if device is not None:
            try:
                if device.is_open:
                    device.close()
            except Exception:
                pass
        reader, self._reader = self._reader, None
        if reader and reader is not threading.current_thread():
            reader.join(timeout=0.4)
        if device is not None:
            self._on_state(False, None)

    def send(self, command: str) -> bool:
        if "\n" in command or "\r" in command:
            return False
        failure: str | None = None
        failed_device = None
        with self._io_lock:
            if self._serial is None or not self._serial.is_open:
                return False
            try:
                self._serial.write((command + "\n").encode("ascii", errors="strict"))
                self._serial.flush()
                return True
            except Exception as exc:
                failure = f"Seri gönderim hatası: {exc}"
                failed_device, self._serial = self._serial, None
                self._port = None
                self._stop.set()
        try:
            if failed_device is not None:
                failed_device.close()
        except Exception:
            pass
        self._on_state(False, failure)
        return False

    def _read_loop(self) -> None:
        failure: str | None = None
        while not self._stop.is_set():
            try:
                with self._io_lock:
                    device = self._serial
                if device is None or not device.is_open:
                    break
                payload = device.readline()
                if payload:
                    self._on_line(payload.decode("utf-8", errors="replace").strip())
            except Exception as exc:
                if not self._stop.is_set():
                    failure = f"Seri okuma hatası: {exc}"
                break
        if failure:
            with self._io_lock:
                device, self._serial = self._serial, None
                self._port = None
            try:
                if device is not None:
                    device.close()
            except Exception:
                pass
            self._on_state(False, failure)


class SimulatorTransport:
    """Deterministic in-process STM32 simulator for dry runs and training."""

    def __init__(self, on_line: LineCallback, on_state: StateCallback):
        self._on_line = on_line
        self._on_state = on_state
        self._connected = False
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._lock = threading.RLock()
        self._positions = [0.0, 0.0]
        self._zeroed = [True, True]
        self._targets = [0.0, 0.0]
        self._speeds = [800.0, 800.0]
        self._thrusters = [0, 0]
        self._armed = False
        self._started = time.monotonic()
        self._config = {
            "TANK1_FULL": 1000.0,
            "TANK2_FULL": 1000.0,
            "OFFSET_MM": 0.0,
            "DENSITY": 997.0,
            "DEPTH_MM": 300.0,
            "MAX_DEPTH_MM": 1500.0,
            "DEPTH_TOL_MM": 20.0,
            "STRAIGHT_MS": 16000.0,
            "FORWARD_PCT": 30.0,
            "CIRCLE_FORWARD_PCT": 30.0,
            "CIRCLE_TURN_PCT": 12.0,
            "HEADING_KP_X100": 80.0,
            "HEADING_OFFSET_CDEG": 0.0,
            "HEADING_SIGN": 1.0,
            "BALANCE_AXIS": 1.0,
            "BALANCE_SIGN": 1.0,
            "LEVEL_KP_X100": 50.0,
            "MAX_BALANCE_PCT": 12.0,
            "BALLAST_GAIN_X100": 2800.0,
            "ROUTE": 0.0,
            "DIVE_BALLAST_PCT": 30.0,
            "HOVER_BALLAST_PCT": 20.0,
            "DIVE_MS": 5000.0,
            "HOVER_SETTLE_MS": 3000.0,
            "TEST_FORWARD_MS": 5000.0,
            "TEST_FORWARD_PCT": 15.0,
            "AUTO_DELAY_MS": 0.0,
            "AUTO_ZERO_TANKS": 0.0,
        }
        self._surface_calibrated = False
        self._sensor_depth_m = 0.0
        self._heading_deg = 0.0
        self._heading_zero_deg = 0.0
        self._mission_state = "IDLE"
        self._mission_active = False
        self._mission_fault = "NONE"
        self._mission_state_at = time.monotonic()
        self._mission_stable_at = 0.0
        self._turn_target_deg = 0.0
        self._circle_degrees = 0.0
        self._previous_heading_deg = 0.0
        self._manual_turn_target: float | None = None
        self._auto_consumed = False

    @property
    def connected(self) -> bool:
        return self._connected

    @property
    def port(self) -> str | None:
        return "SIMULATOR" if self._connected else None

    def list_ports(self) -> list[str]:
        return ["SIMULATOR"]

    def connect(self, port: str) -> None:
        if port != "SIMULATOR":
            raise ValueError("Simülatör için SIMULATOR portunu seçin")
        self.disconnect()
        self._stop.clear()
        self._connected = True
        self._thread = threading.Thread(target=self._run, name="stm32-simulator", daemon=True)
        self._thread.start()
        self._on_state(True, None)
        self._on_line("READY SEA_STARS_SIM")
        self._on_line("SENSOR IMU:BNO085_SIM ADDR:0x4A")

    def disconnect(self) -> None:
        was_connected = self._connected
        self._stop.set()
        self._connected = False
        thread, self._thread = self._thread, None
        if thread and thread is not threading.current_thread():
            thread.join(timeout=0.4)
        if was_connected:
            self._on_state(False, None)

    def send(self, command: str) -> bool:
        if not self._connected or "\n" in command or "\r" in command:
            return False
        try:
            with self._lock:
                pieces = command.strip().upper().split()
                head = pieces[0]
                if head in ("PING", "STATUS", "POS"):
                    pass
                elif head in ("S1", "S2"):
                    index = int(head[1]) - 1
                    self._targets[index] += int(pieces[1])
                elif head in ("V1", "V2"):
                    self._speeds[int(head[1]) - 1] = float(pieces[1])
                elif head in ("ZERO1", "ZERO2"):
                    index = int(head[-1]) - 1
                    self._positions[index] = self._targets[index] = 0
                    self._zeroed[index] = True
                    self._on_line(f"ZERO{index + 1} OK")
                elif head in ("SETPOS1", "SETPOS2"):
                    index = int(head[-1]) - 1
                    self._positions[index] = self._targets[index] = int(pieces[1])
                elif head == "ARM":
                    self._armed = True
                    self._on_line("ARMED")
                elif head == "DISARM":
                    self._armed = False
                    self._thrusters = [0, 0]
                    if self._mission_active:
                        self._mission_active = False
                        self._targets = list(self._positions)
                        self._enter_mission_state("ABORTED")
                    self._on_line("DISARMED")
                elif head == "STOP":
                    self._armed = False
                    self._thrusters = [0, 0]
                    self._targets = list(self._positions)
                    self._mission_active = False
                    self._manual_turn_target = None
                    self._enter_mission_state("ABORTED")
                    self._on_line("STOPPED")
                elif head in ("T1", "T2"):
                    index = int(head[1]) - 1
                    self._thrusters[index] = int(pieces[1]) if self._armed else 0
                elif head == "CFG":
                    key = pieces[1]
                    if key not in self._config:
                        self._on_line("ERR UNKNOWN_CONFIG")
                        return False
                    self._config[key] = float(pieces[2])
                    self._on_line(f"OK CFG {key}")
                elif head == "DEPTH_ZERO":
                    self._surface_calibrated = True
                    self._on_line("OK DEPTH_ZERO")
                elif head == "IMU_ZERO":
                    self._heading_zero_deg = self._heading_deg
                    self._on_line("OK IMU_ZERO")
                elif head == "AUTO" and pieces[1] == "START":
                    self._armed = True
                    self._mission_active = True
                    self._auto_consumed = True
                    self._mission_fault = "NONE"
                    self._enter_mission_state("DIVE")
                    self._on_line("OK AUTO START")
                elif head == "AUTO" and pieces[1] in ("SCHEDULE", "PREPARE"):
                    if self._config["AUTO_DELAY_MS"] <= 0:
                        self._on_line("ERR AUTO DISABLED")
                        return False
                    if pieces[1] == "SCHEDULE" and self._auto_consumed:
                        self._on_line("ERR AUTO ALREADY_CONSUMED")
                        return False
                    if self._config["AUTO_ZERO_TANKS"] >= 0.5:
                        self._positions = [0.0, 0.0]
                        self._targets = [0.0, 0.0]
                        self._zeroed = [True, True]
                    self._armed = False
                    self._thrusters = [0, 0]
                    self._mission_active = True
                    self._auto_consumed = True
                    self._mission_fault = "NONE"
                    self._enter_mission_state("COUNTDOWN")
                    self._on_line(f"OK AUTO COUNTDOWN {int(self._config['AUTO_DELAY_MS'])}")
                elif head == "AUTO" and pieces[1] == "ABORT":
                    self._mission_active = False
                    self._armed = False
                    self._thrusters = [0, 0]
                    self._targets = [0.0, 0.0]
                    self._enter_mission_state("ABORTED")
                    self._on_line("OK AUTO ABORT")
                elif head == "TURN":
                    if not self._armed:
                        self._on_line("ERR NOT_ARMED")
                        return False
                    degrees = float(pieces[1])
                    self._manual_turn_target = self._wrap_heading(self._heading_deg + degrees)
                else:
                    self._on_line("ERR UNKNOWN_COMMAND")
                    return False
            return True
        except (IndexError, ValueError):
            self._on_line("ERR INVALID_COMMAND")
            return False

    def _run(self) -> None:
        previous = time.monotonic()
        while not self._stop.wait(0.1):
            now = time.monotonic()
            elapsed = now - previous
            previous = now
            with self._lock:
                self._update_mission(now, elapsed)
                self._update_heading(elapsed)
                for index in range(2):
                    difference = self._targets[index] - self._positions[index]
                    movement = min(abs(difference), self._speeds[index] * elapsed)
                    self._positions[index] += math.copysign(movement, difference) if difference else 0
                p1, p2 = (round(value) for value in self._positions)
                full1 = max(1.0, self._config["TANK1_FULL"])
                full2 = max(1.0, self._config["TANK2_FULL"])
                fill = max(0.0, min(1.0, (self._positions[0] / full1 + self._positions[1] / full2) / 2.0))
                target_sensor_depth = fill * 0.60
                self._sensor_depth_m += (target_sensor_depth - self._sensor_depth_m) * min(1.0, elapsed * 0.9)
                vehicle_depth = max(0.0, self._sensor_depth_m + self._config["OFFSET_MM"] / 1000.0)
                heading = self._wrap_heading(
                    (self._heading_deg - self._heading_zero_deg) * self._config["HEADING_SIGN"] +
                    self._config["HEADING_OFFSET_CDEG"] / 100.0)
                density = self._config["DENSITY"]
                pressure = 1013.25 + self._sensor_depth_m * density * 9.80665 / 100.0
                mission_state = self._mission_state
                mission_elapsed = round((now - self._mission_state_at) * 1000)
                mission_fault = self._mission_fault
                mission_active = self._mission_active
            phase = now - self._started
            roll = math.sin(phase / 3) * 1.5
            pitch = math.sin(phase / 4) * 1.0
            self._on_line(
                f"POS1:{p1} POS2:{p2} Z1:{1 if self._zeroed[0] else 0} "
                f"Z2:{1 if self._zeroed[1] else 0}"
            )
            self._on_line(f"IMU H:{heading:.1f} R:{roll:.1f} P:{pitch:.1f} C:255")
            self._on_line(
                f"DEPTH M:{vehicle_depth:.3f} T:22.5 P:{pressure:.2f} "
                f"S:{self._sensor_depth_m:.3f} Z:{1 if self._surface_calibrated else 0}"
            )
            self._on_line(
                f"AUTO STATE:{mission_state} ELAPSED:{mission_elapsed} "
                f"FAULT:{mission_fault} ACTIVE:{1 if mission_active else 0}"
            )

    @staticmethod
    def _wrap_heading(value: float) -> float:
        return value % 360.0

    @staticmethod
    def _heading_error(target: float, current: float) -> float:
        return (target - current + 180.0) % 360.0 - 180.0

    def _enter_mission_state(self, state: str) -> None:
        self._mission_state = state
        self._mission_state_at = time.monotonic()
        self._mission_stable_at = 0.0

    def _set_mission_thrusters(self, left_pct: float, right_pct: float) -> None:
        self._thrusters = [round(max(-100.0, min(100.0, left_pct)) * 2),
                           round(max(-100.0, min(100.0, right_pct)) * 2)]

    def _target_ballast_percent(self, percent: float) -> None:
        fill = max(0.0, min(1.0, percent / 100.0))
        self._targets = [fill * self._config["TANK1_FULL"], fill * self._config["TANK2_FULL"]]

    def _update_heading(self, elapsed: float) -> None:
        if self._manual_turn_target is not None:
            error = self._heading_error(self._manual_turn_target, self._heading_deg)
            if abs(error) <= 2.0:
                self._thrusters = [0, 0]
                self._manual_turn_target = None
            else:
                command = math.copysign(50, error)
                self._thrusters = [round(command), round(-command)]
        if not self._armed:
            return
        left_pct = self._thrusters[0] / 2.0
        right_pct = self._thrusters[1] / 2.0
        yaw_rate = (left_pct - right_pct) * 1.2
        self._heading_deg = self._wrap_heading(self._heading_deg + yaw_rate * elapsed)

    def _update_mission(self, now: float, elapsed: float) -> None:
        del elapsed
        if not self._mission_active:
            return
        state_elapsed = now - self._mission_state_at
        straight_seconds = self._config["STRAIGHT_MS"] / 1000.0
        forward = self._config["FORWARD_PCT"]
        if self._mission_state == "COUNTDOWN":
            self._armed = False
            self._thrusters = [0, 0]
            if state_elapsed * 1000.0 >= self._config["AUTO_DELAY_MS"]:
                self._armed = True
                self._enter_mission_state("DIVE")
        elif self._mission_state == "DIVE":
            self._target_ballast_percent(self._config["DIVE_BALLAST_PCT"])
            self._set_mission_thrusters(0, 0)
            if state_elapsed * 1000.0 >= self._config["DIVE_MS"]:
                self._enter_mission_state("HOVER_SETTLE")
        elif self._mission_state == "HOVER_SETTLE":
            self._target_ballast_percent(self._config["HOVER_BALLAST_PCT"])
            self._set_mission_thrusters(0, 0)
            if state_elapsed * 1000.0 >= self._config["HOVER_SETTLE_MS"]:
                self._enter_mission_state(
                    "TEST_FORWARD" if self._config["ROUTE"] < 0.5 else "STRAIGHT_1"
                )
        elif self._mission_state == "TEST_FORWARD":
            self._target_ballast_percent(self._config["HOVER_BALLAST_PCT"])
            power = self._config["TEST_FORWARD_PCT"]
            self._set_mission_thrusters(power, power)
            if state_elapsed * 1000.0 >= self._config["TEST_FORWARD_MS"]:
                self._enter_mission_state("SURFACE")
        elif self._mission_state in ("STRAIGHT_1", "STRAIGHT_2", "STRAIGHT_3", "STRAIGHT_4"):
            self._target_ballast_percent(self._config["HOVER_BALLAST_PCT"])
            self._set_mission_thrusters(forward, forward)
            if state_elapsed >= straight_seconds:
                if self._mission_state in ("STRAIGHT_1", "STRAIGHT_3"):
                    self._turn_target_deg = self._wrap_heading(self._heading_deg + 90.0)
                    self._enter_mission_state("TURN_1" if self._mission_state == "STRAIGHT_1" else "TURN_2")
                elif self._mission_state == "STRAIGHT_2":
                    self._circle_degrees = 0.0
                    self._previous_heading_deg = self._heading_deg
                    self._enter_mission_state("CIRCLE")
                else:
                    self._enter_mission_state("SURFACE")
        elif self._mission_state in ("TURN_1", "TURN_2"):
            self._target_ballast_percent(self._config["HOVER_BALLAST_PCT"])
            error = self._heading_error(self._turn_target_deg, self._heading_deg)
            turn = max(12.0, min(30.0, abs(error) * self._config["HEADING_KP_X100"] / 100.0))
            self._set_mission_thrusters(math.copysign(turn, error), -math.copysign(turn, error))
            if abs(error) <= 3.0:
                if self._mission_stable_at == 0.0:
                    self._mission_stable_at = now
                elif now - self._mission_stable_at >= 0.8:
                    self._enter_mission_state("STRAIGHT_2" if self._mission_state == "TURN_1" else "STRAIGHT_4")
            else:
                self._mission_stable_at = 0.0
        elif self._mission_state == "CIRCLE":
            self._target_ballast_percent(self._config["HOVER_BALLAST_PCT"])
            current = self._heading_deg
            delta = self._heading_error(current, self._previous_heading_deg)
            self._previous_heading_deg = current
            if 0.0 < delta < 45.0:
                self._circle_degrees += delta
            circle_forward = self._config["CIRCLE_FORWARD_PCT"]
            circle_turn = self._config["CIRCLE_TURN_PCT"]
            self._set_mission_thrusters(circle_forward + circle_turn, circle_forward - circle_turn)
            if self._circle_degrees >= 360.0 and state_elapsed >= 10.0:
                self._enter_mission_state("STRAIGHT_3")
        elif self._mission_state == "SURFACE":
            self._targets = [0.0, 0.0]
            self._set_mission_thrusters(0, 0)
            if max(self._positions) <= 10.0:
                self._mission_active = False
                self._armed = False
                self._enter_mission_state("COMPLETE")
