"""Domain controller: validation, safety interlocks and telemetry state."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import threading
import time
from typing import Any

from . import protocol
from .protocol import (
    DepthEvent,
    ImuEvent,
    ImuStatusEvent,
    MessageEvent,
    MissionEvent,
    PositionEvent,
)
from .state_store import StateStore
from .transport import Transport


class StationError(RuntimeError):
    def __init__(self, message: str, code: str = "station_error", status: int = 400):
        super().__init__(message)
        self.code = code
        self.status = status


@dataclass
class TankState:
    live_position: int = 0
    target_position: int = 0
    speed: int = 800
    telemetry_at: float = 0.0
    zeroed: bool = False


class StationController:
    def __init__(
        self,
        transport: Transport,
        store: StateStore,
        *,
        heartbeat_timeout: float = 1.2,
        telemetry_timeout: float = 1.5,
        power_limit: float = 100.0,
        developer_mode: bool = False,
    ):
        self.transport = transport
        self.store = store
        self.heartbeat_timeout = heartbeat_timeout
        self.telemetry_timeout = telemetry_timeout
        self.power_limit = power_limit
        self.developer_mode = developer_mode
        self._lock = threading.RLock()
        self._tanks = [TankState(), TankState()]
        self._imu: dict[str, Any] = {
            "model": None,
            "address": None,
            "detected": False,
            "heading": 0.0,
            "roll": 0.0,
            "pitch": 0.0,
            "calibration": 0,
            "updated_at": 0.0,
        }
        self._depth: dict[str, Any] = {
            "depth_m": 0.0,
            "sensor_depth_m": 0.0,
            "temperature_c": 0.0,
            "pressure_mbar": 0.0,
            "surface_calibrated": False,
            "updated_at": 0.0,
        }
        self._mission: dict[str, Any] = {
            "state": "IDLE",
            "elapsed_ms": 0,
            "fault": "NONE",
            "active": False,
            "updated_at": 0.0,
        }
        self._connected = False
        self._connection_error: str | None = None
        self._armed = False
        self._estop_latched = False
        self._thrusters = [0.0, 0.0]
        self._last_heartbeat = 0.0
        self._configuration_pushed = False
        self._auto_schedule_sent = False
        self._auto_rearm_pending = False
        self._log_sequence = 0
        self._logs: deque[dict[str, Any]] = deque(maxlen=500)
        self._closed = threading.Event()
        self._watchdog = threading.Thread(target=self._watchdog_loop, name="safety-watchdog", daemon=True)
        self._watchdog.start()

    def close(self) -> None:
        self._closed.set()
        if self.transport.connected:
            self.emergency_stop("Uygulama kapatılıyor")
            self.transport.disconnect()
        self._watchdog.join(timeout=0.5)

    def on_transport_state(self, connected: bool, error: str | None) -> None:
        with self._lock:
            self._connected = connected
            self._connection_error = error
            self._configuration_pushed = False
            self._auto_schedule_sent = False
            self._auto_rearm_pending = False
            if not connected:
                self._armed = False
                self._thrusters = [0.0, 0.0]
                self._imu["detected"] = False
                for tank in self._tanks:
                    tank.zeroed = False
            self._append_log("system", "Bağlandı" if connected else (error or "Bağlantı kesildi"))

    def on_line(self, line: str) -> None:
        event = protocol.parse_line(line)
        now = time.monotonic()
        with self._lock:
            if isinstance(event, PositionEvent):
                zeroed = (event.zeroed1, event.zeroed2)
                for tank, position, zero_state in zip(
                        self._tanks, (event.tank1, event.tank2), zeroed):
                    tank.live_position = position
                    tank.telemetry_at = now
                    if zero_state is not None:
                        tank.zeroed = zero_state
                self._try_auto_schedule_locked()
            elif isinstance(event, ImuEvent):
                self._imu.update(
                    heading=event.heading % 360,
                    roll=event.roll,
                    pitch=event.pitch,
                    calibration=event.calibration,
                    updated_at=now,
                )
                self._imu["detected"] = True
            elif isinstance(event, ImuStatusEvent):
                self._imu["detected"] = event.online
                if event.model is not None:
                    self._imu["model"] = event.model
                if event.address is not None:
                    self._imu["address"] = event.address
                self._append_log(
                    "system",
                    (f"IMU algılandı: {event.model} (0x{event.address:02X})"
                     if event.online and event.address is not None
                     else "IMU bağlantısı bulunamadı"),
                )
            elif isinstance(event, DepthEvent):
                self._depth.update(
                    depth_m=event.depth_m,
                    sensor_depth_m=(event.sensor_depth_m
                                    if event.sensor_depth_m is not None else event.depth_m),
                    temperature_c=event.temperature_c,
                    pressure_mbar=event.pressure_mbar,
                    surface_calibrated=event.surface_calibrated,
                    updated_at=now,
                )
            elif isinstance(event, MissionEvent):
                self._mission.update(
                    state=event.state,
                    elapsed_ms=event.elapsed_ms,
                    fault=event.fault,
                    active=event.active,
                    updated_at=now,
                )
                if event.active and event.state != "COUNTDOWN":
                    self._armed = True
                elif event.state == "COUNTDOWN":
                    self._armed = False
                    self._thrusters = [0.0, 0.0]
                elif event.state in ("COMPLETE", "ABORTED", "FAULT_SURFACE", "IDLE"):
                    self._armed = False
                    self._thrusters = [0.0, 0.0]
            elif isinstance(event, MessageEvent):
                upper = event.text.upper()
                if "DISARMED" in upper or "STOPPED" in upper:
                    self._armed = False
                    self._thrusters = [0.0, 0.0]
                elif "ARMED" in upper:
                    self._armed = True
                    self._last_heartbeat = now
                self._append_log("rx", event.text)
                if "READY" in upper and self._connected:
                    try:
                        self._push_configuration_locked(self.store.configuration())
                        self._configuration_pushed = True
                        self._try_auto_schedule_locked()
                        self._append_log("system", "Kayıtlı tarayıcı ayarları STM32'ye aktarıldı")
                    except StationError as exc:
                        self._connection_error = f"Başlangıç ayarları gönderilemedi: {exc}"
                        self._append_log("safety", self._connection_error)

    def list_ports(self) -> list[str]:
        try:
            return self.transport.list_ports()
        except Exception as exc:
            with self._lock:
                self._connection_error = str(exc)
            return []

    def connect(self, port: str) -> None:
        try:
            self.transport.connect(port)
        except Exception as exc:
            raise StationError(str(exc), "connection_failed", 503) from exc

    def disconnect(self) -> None:
        if self.transport.connected:
            self.transport.send("STOP")
        self.transport.disconnect()

    def configuration(self) -> dict[str, Any]:
        return self.store.configuration()

    def save_configuration(self, changes: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            if self._mission["active"]:
                raise StationError("Aktif görev sırasında ayarlar değiştirilemez", "mission_active", 409)
            try:
                saved = self.store.update_configuration(changes)
            except (TypeError, ValueError) as exc:
                raise StationError(str(exc), "invalid_configuration") from exc
            if self.transport.connected and self._connected:
                self._push_configuration_locked(saved)
                self._configuration_pushed = True
                if "autonomy" in changes:
                    self._auto_schedule_sent = False
                    self._auto_rearm_pending = bool(saved["autonomy"]["auto_start_enabled"])
                self._try_auto_schedule_locked()
            self._append_log("system", "Kalibrasyon ve görev ayarları tarayıcıdan kaydedildi")
            return saved

    def calibrate_depth_surface(self) -> None:
        with self._lock:
            self._require_connected()
            if self._mission["active"]:
                raise StationError("Aktif görev sırasında derinlik sıfırlanamaz", "mission_active", 409)
            if (self._depth["updated_at"] <= 0 or
                    time.monotonic() - self._depth["updated_at"] > self.telemetry_timeout):
                raise StationError("Yüzey sıfırı için güncel D300 verisi gerekli", "depth_stale", 409)
            self._send_locked("DEPTH_ZERO")
            self._depth["surface_calibrated"] = False
            self._append_log("system", "D300 yüzey basıncı kalibrasyonu başlatıldı")

    def calibrate_heading_zero(self) -> float:
        with self._lock:
            self._require_connected()
            if self._mission["active"]:
                raise StationError("Aktif görev sırasında yön sıfırlanamaz", "mission_active", 409)
            if self._imu["updated_at"] <= 0 or time.monotonic() - self._imu["updated_at"] > self.telemetry_timeout:
                raise StationError("Güncel IMU verisi yok", "telemetry_stale", 409)
            previous_heading = float(self._imu["heading"])
            current_offset = float(self.store.configuration()["imu"]["heading_offset_deg"])
            new_offset = (current_offset - previous_heading + 180.0) % 360.0 - 180.0
            self._send_locked(protocol.configure("HEADING_OFFSET_CDEG", round(new_offset * 100.0)))
            self.store.update_configuration({"imu": {"heading_offset_deg": new_offset}})
            self._append_log("system", f"IMU mevcut yönü 0° kabul edildi; ofset {new_offset:.2f}°")
            return new_offset

    def start_mission(self) -> None:
        with self._lock:
            self._require_motion_ready()
            if self._mission["active"]:
                raise StationError("Otonom görev zaten çalışıyor", "mission_active", 409)
            now = time.monotonic()
            if self._imu["updated_at"] <= 0 or now - self._imu["updated_at"] > self.telemetry_timeout:
                raise StationError("Güncel IMU verisi olmadan görev başlatılamaz", "imu_stale", 409)
            self._require_fresh_tank(1)
            self._require_fresh_tank(2)
            if self.store.full_stroke(1) <= 0 or self.store.full_stroke(2) <= 0:
                raise StationError("Her iki balast tankı da kalibre edilmeli", "not_calibrated", 409)
            if not self._tanks[0].zeroed or not self._tanks[1].zeroed:
                raise StationError("Her iki balast tankı boş konumda sıfırlanmalı", "not_zeroed", 409)
            for index, tank in enumerate(self._tanks, start=1):
                if tank.live_position > max(2, self.store.full_stroke(index) // 100):
                    raise StationError(
                        "Görevden önce iki balast tankı da %0 konumunda olmalı",
                        "tank_not_empty",
                        409,
                    )
            self._push_configuration_locked(self.store.configuration())
            self._send_locked("AUTO START")
            self._mission.update(state="STARTING", elapsed_ms=0, fault="NONE", active=True, updated_at=now)
            self._armed = True
            route = self.store.configuration()["autonomy"]["route"]
            self._append_log("mission", f"{route} rotası hemen başlatıldı")

    def schedule_mission(self, *, rearm: bool = True) -> None:
        """Deliberately (re)start the configured autonomous countdown."""
        with self._lock:
            self._require_motion_ready()
            config = self.store.configuration()
            if not config["autonomy"]["auto_start_enabled"]:
                raise StationError(
                    "Otonomi sekmesinden otomatik başlatmayı Açık yapın",
                    "auto_start_disabled",
                    409,
                )
            self._require_auto_prerequisites_locked(config)
            self._push_configuration_locked(config)
            self._configuration_pushed = True
            self._send_locked("AUTO PREPARE" if rearm else "AUTO SCHEDULE")
            now = time.monotonic()
            self._mission.update(
                state="COUNTDOWN", elapsed_ms=0, fault="NONE", active=True, updated_at=now
            )
            self._armed = False
            self._thrusters = [0.0, 0.0]
            self._auto_schedule_sent = True
            self._auto_rearm_pending = False
            self._append_log("mission", "Otomatik başlatma sayacı hazırlandı")

    def abort_mission(self) -> None:
        with self._lock:
            self._require_connected()
            self._send_locked("AUTO ABORT")
            self._mission.update(state="ABORTING", active=False, updated_at=time.monotonic())
            self._armed = False
            self._thrusters = [0.0, 0.0]
            self._append_log("mission", "Görev iptal edildi; balast boşaltma istendi")

    def turn_relative(self, degrees: float) -> None:
        with self._lock:
            self._prepare_manual_propulsion_locked(True)
            self._send_locked(protocol.turn_relative(degrees))

    def heartbeat(self) -> None:
        with self._lock:
            self._require_connected()
            if self._armed and not self._mission["active"]:
                if not self.transport.send("PING"):
                    raise StationError("Heartbeat STM32'ye gönderilemedi", "send_failed", 503)
            self._last_heartbeat = time.monotonic()

    def arm(self) -> None:
        with self._lock:
            self._require_connected()
            if self._estop_latched:
                raise StationError("Acil stop kilidi kaldırılmadan ARM yapılamaz", "estop_latched", 409)
            if not self._telemetry_fresh_locked():
                raise StationError("Güncel STM32 telemetrisi alınmadan ARM yapılamaz", "telemetry_stale", 409)
            self._send_locked("ARM")
            # Eski firmware ACK göndermeyebildiği için istek kabul edildi olarak işaretlenir.
            self._armed = True
            self._last_heartbeat = time.monotonic()

    def disarm(self) -> None:
        with self._lock:
            self._require_connected()
            self._send_locked("T1 0")
            self._send_locked("T2 0")
            self._send_locked("DISARM")
            self._armed = False
            self._thrusters = [0.0, 0.0]

    def emergency_stop(self, reason: str = "Operatör acil stop verdi") -> None:
        with self._lock:
            if self.transport.connected:
                self.transport.send("STOP")
            self._armed = False
            self._thrusters = [0.0, 0.0]
            self._estop_latched = True
            self._mission.update(state="ESTOP", active=False, updated_at=time.monotonic())
            self._append_log("safety", reason)

    def clear_estop(self) -> None:
        with self._lock:
            self._require_connected()
            self._send_locked("DISARM")
            self._armed = False
            self._thrusters = [0.0, 0.0]
            self._estop_latched = False
            self._append_log("safety", "Acil stop kilidi operatör tarafından kaldırıldı")

    def set_thruster(self, index: int, percent: float) -> None:
        with self._lock:
            command = protocol.set_thruster(index, percent, self.power_limit)
            any_nonzero = abs(float(percent)) > 0.01 or any(
                abs(value) > 0.01 for i, value in enumerate(self._thrusters) if i != index - 1
            )
            if not self._prepare_manual_propulsion_locked(any_nonzero):
                self._thrusters[index - 1] = 0.0
                return
            self._send_locked(command)
            self._thrusters[index - 1] = max(-self.power_limit, min(self.power_limit, percent))

    def set_both_thrusters(self, percent: float) -> None:
        self.set_thruster_pair(percent, percent)

    def set_thruster_pair(self, left_percent: float, right_percent: float) -> None:
        with self._lock:
            command1 = protocol.set_thruster(1, left_percent, self.power_limit)
            command2 = protocol.set_thruster(2, right_percent, self.power_limit)
            any_nonzero = abs(float(left_percent)) > 0.01 or abs(float(right_percent)) > 0.01
            if not self._prepare_manual_propulsion_locked(any_nonzero):
                self._thrusters = [0.0, 0.0]
                return
            self._send_locked(command1)
            if not self.transport.send(command2):
                self.transport.send("STOP")
                self._armed = False
                self._thrusters = [0.0, 0.0]
                self._estop_latched = True
                raise StationError("İkinci itici komutu gönderilemedi; STOP uygulandı", "partial_command", 503)
            self._append_log("tx", command2)
            self._thrusters = [
                max(-self.power_limit, min(self.power_limit, float(left_percent))),
                max(-self.power_limit, min(self.power_limit, float(right_percent))),
            ]

    def set_tank_speed(self, index: int, speed: int) -> None:
        command = protocol.set_tank_speed(index, speed)
        with self._lock:
            self._require_motion_ready()
            self._send_locked(command)
            self._tanks[index - 1].speed = speed

    def jog_tank(self, index: int, steps: int) -> None:
        if abs(steps) > 10_000:
            raise StationError("Tek seferde en fazla 10.000 adım jog yapılabilir", "invalid_steps")
        with self._lock:
            self._require_motion_ready()
            self._require_fresh_tank(index)
            tank = self._tank(index)
            full = self.store.full_stroke(index)
            target = tank.live_position + steps
            if full > 0 and not 0 <= target <= full:
                raise StationError("Hareket kalibre edilmiş tank sınırlarının dışında", "tank_limit", 409)
            self._send_locked(protocol.move_tank(index, steps))
            tank.target_position = target

    def goto_tank(self, index: int, percent: float) -> None:
        if not 0 <= percent <= 100:
            raise StationError("Tank yüzdesi 0–100 aralığında olmalı", "invalid_percent")
        with self._lock:
            self._require_motion_ready()
            self._require_fresh_tank(index)
            full = self.store.full_stroke(index)
            if full <= 0:
                raise StationError(f"Tank {index} önce kalibre edilmeli", "not_calibrated", 409)
            tank = self._tank(index)
            if not tank.zeroed:
                raise StationError(
                    f"Tank {index} yüzde hedefinden önce boş konumda sıfırlanmalı",
                    "not_zeroed",
                    409,
                )
            target = round(full * percent / 100)
            self._send_locked(protocol.move_tank(index, target - tank.live_position))
            tank.target_position = target

    def goto_both_tanks(self, percent: float) -> None:
        if not 0 <= percent <= 100:
            raise StationError("Tank yüzdesi 0–100 aralığında olmalı", "invalid_percent")
        with self._lock:
            self._require_motion_ready()
            self._require_fresh_tank(1)
            self._require_fresh_tank(2)
            fulls = [self.store.full_stroke(1), self.store.full_stroke(2)]
            if any(value <= 0 for value in fulls):
                raise StationError("Her iki tank da önce kalibre edilmeli", "not_calibrated", 409)
            if any(not tank.zeroed for tank in self._tanks):
                raise StationError(
                    "İki tank da yüzde hedefinden önce boş konumda sıfırlanmalı",
                    "not_zeroed",
                    409,
                )
            targets = [round(full * percent / 100) for full in fulls]
            commands = [
                protocol.move_tank(index + 1, targets[index] - self._tanks[index].live_position)
                for index in range(2)
            ]
            self._send_locked(commands[0])
            if not self.transport.send(commands[1]):
                self.transport.send("STOP")
                self._estop_latched = True
                raise StationError("İkinci tank komutu gönderilemedi; STOP uygulandı", "partial_command", 503)
            self._append_log("tx", commands[1])
            for tank, target in zip(self._tanks, targets):
                tank.target_position = target

    def zero_tank(self, index: int) -> None:
        with self._lock:
            self._require_motion_ready()
            self._send_locked(protocol.zero_tank(index))
            tank = self._tank(index)
            tank.live_position = 0
            tank.target_position = 0
            tank.telemetry_at = time.monotonic()
            tank.zeroed = True
            self._try_auto_schedule_locked()

    def set_full_stroke(self, index: int, value: int | None = None) -> int:
        with self._lock:
            if value is None:
                self._require_fresh_tank(index)
                value = self._tank(index).live_position
            try:
                self.store.set_full_stroke(index, int(value))
            except (TypeError, ValueError) as exc:
                raise StationError(str(exc), "invalid_calibration") from exc
            if self.transport.connected and self._connected:
                self._send_locked(protocol.configure(f"TANK{index}_FULL", int(value)))
            self._append_log("system", f"Tank {index} tam strok: {value} adım")
            self._try_auto_schedule_locked()
            return int(value)

    def raw_command(self, command: str) -> None:
        with self._lock:
            if not self.developer_mode:
                raise StationError("Ham komut konsolu geliştirici modunda kapalı", "developer_mode_required", 403)
            if self._armed:
                raise StationError("ARM durumundayken ham komut gönderilemez", "unsafe_raw_command", 409)
            if not command or len(command) > 100 or "\n" in command or "\r" in command:
                raise StationError("Geçersiz ham komut", "invalid_command")
            self._require_connected()
            self._send_locked(command.strip())

    def logs_since(self, sequence: int) -> dict[str, Any]:
        with self._lock:
            items = [item.copy() for item in self._logs if item["sequence"] > sequence]
            return {"items": items, "latest_sequence": self._log_sequence}

    def snapshot(self) -> dict[str, Any]:
        now = time.monotonic()
        with self._lock:
            config = self.store.configuration()
            tanks = []
            for index, tank in enumerate(self._tanks, start=1):
                full = self.store.full_stroke(index)
                fresh = tank.telemetry_at > 0 and now - tank.telemetry_at <= self.telemetry_timeout
                tanks.append(
                    {
                        "id": index,
                        "live_position": tank.live_position,
                        "target_position": tank.target_position,
                        "full_stroke": full,
                        "percent": self._percent(tank.live_position, full) if fresh else None,
                        "target_percent": self._percent(tank.target_position, full),
                        "speed": tank.speed,
                        "zeroed": tank.zeroed,
                        "fresh": fresh,
                        "age_ms": round((now - tank.telemetry_at) * 1000) if tank.telemetry_at else None,
                    }
                )
            imu_fresh = self._imu["updated_at"] > 0 and now - self._imu["updated_at"] <= self.telemetry_timeout
            depth_fresh = self._depth["updated_at"] > 0 and now - self._depth["updated_at"] <= self.telemetry_timeout
            mission_fresh = self._mission["updated_at"] > 0 and now - self._mission["updated_at"] <= 2.0
            calibration = int(self._imu["calibration"])
            return {
                "version": "3.4.1",
                "connected": self.transport.connected and self._connected,
                "port": self.transport.port,
                "connection_error": self._connection_error,
                "safety": {
                    "armed": self._armed,
                    "estop_latched": self._estop_latched,
                    "heartbeat_ok": (not self._armed) or now - self._last_heartbeat <= self.heartbeat_timeout,
                    "power_limit": self.power_limit,
                },
                "thrusters": [
                    {"id": index + 1, "percent": value} for index, value in enumerate(self._thrusters)
                ],
                "tanks": tanks,
                "imu": {
                    "model": self._imu["model"],
                    "address": self._imu["address"],
                    "detected": self._imu["detected"],
                    "heading": self._imu["heading"],
                    "roll": self._imu["roll"],
                    "pitch": self._imu["pitch"],
                    "calibration": calibration,
                    "calibration_parts": {
                        "system": (calibration >> 6) & 3,
                        "gyro": (calibration >> 4) & 3,
                        "accelerometer": (calibration >> 2) & 3,
                        "magnetometer": calibration & 3,
                    },
                    "fresh": imu_fresh,
                    "age_ms": round((now - self._imu["updated_at"]) * 1000) if self._imu["updated_at"] else None,
                },
                "depth": {
                    "depth_m": self._depth["depth_m"],
                    "depth_cm": round(self._depth["depth_m"] * 100.0, 1),
                    "sensor_depth_m": self._depth["sensor_depth_m"],
                    "temperature_c": self._depth["temperature_c"],
                    "pressure_mbar": self._depth["pressure_mbar"],
                    "surface_calibrated": self._depth["surface_calibrated"],
                    "fresh": depth_fresh,
                    "age_ms": round((now - self._depth["updated_at"]) * 1000) if self._depth["updated_at"] else None,
                },
                "mission": {
                    **self._mission,
                    "fresh": mission_fresh,
                    "route": config["autonomy"]["route"],
                    "countdown_remaining_ms": max(
                        0,
                        int(config["autonomy"]["auto_start_delay_seconds"] * 1000)
                        - int(self._mission["elapsed_ms"]),
                    ) if self._mission["state"] == "COUNTDOWN" else 0,
                },
                "capabilities": {"developer_mode": self.developer_mode},
            }

    def _watchdog_loop(self) -> None:
        while not self._closed.wait(0.1):
            with self._lock:
                expired = (self._armed and not self._mission["active"] and
                           time.monotonic() - self._last_heartbeat > self.heartbeat_timeout)
                if expired:
                    self.transport.send("T1 0")
                    self.transport.send("T2 0")
                    self.transport.send("DISARM")
                    self._armed = False
                    self._thrusters = [0.0, 0.0]
                    self._append_log("safety", "Operatör heartbeat kayboldu; iticiler durduruldu")

    def _send_locked(self, command: str) -> None:
        if not self.transport.send(command):
            raise StationError("Komut STM32'ye gönderilemedi", "send_failed", 503)
        self._append_log("tx", command)

    def _append_log(self, category: str, message: str) -> None:
        self._log_sequence += 1
        self._logs.append(
            {
                "sequence": self._log_sequence,
                "time": time.strftime("%H:%M:%S"),
                "category": category,
                "message": message,
            }
        )

    def _require_connected(self) -> None:
        if not self.transport.connected or not self._connected:
            raise StationError("STM32 bağlantısı yok", "not_connected", 409)

    def _require_motion_ready(self) -> None:
        self._require_connected()
        if self._estop_latched:
            raise StationError("Acil stop kilidi aktif", "estop_latched", 409)
        if self._mission["active"]:
            raise StationError("Otonom görev sırasında manuel hareket kapalı", "mission_active", 409)

    def _require_propulsion_ready(self) -> None:
        self._prepare_manual_propulsion_locked(True)

    def _prepare_manual_propulsion_locked(self, requested_nonzero: bool) -> bool:
        self._require_motion_ready()
        if not requested_nonzero and not self._armed:
            return False
        if not self._telemetry_fresh_locked():
            raise StationError("Güncel STM32 telemetrisi yok", "telemetry_stale", 409)
        if not self._armed:
            self._send_locked("ARM")
            self._armed = True
            self._append_log("safety", "İlk manuel itici komutuyla ESC otomatik etkinleştirildi")
        self._last_heartbeat = time.monotonic()
        return True

    def _require_fresh_tank(self, index: int) -> None:
        tank = self._tank(index)
        if tank.telemetry_at <= 0 or time.monotonic() - tank.telemetry_at > self.telemetry_timeout:
            raise StationError("Güncel tank telemetrisi yok", "telemetry_stale", 409)

    def _telemetry_fresh_locked(self) -> bool:
        now = time.monotonic()
        return all(tank.telemetry_at > 0 and now - tank.telemetry_at <= self.telemetry_timeout for tank in self._tanks)

    def _tank(self, index: int) -> TankState:
        if index not in (1, 2):
            raise StationError("Tank numarası 1 veya 2 olmalı", "invalid_tank")
        return self._tanks[index - 1]

    def _push_configuration_locked(self, config: dict[str, Any]) -> None:
        imu = config["imu"]
        autonomy = config["autonomy"]
        mission = config["mission"]
        balance_axis = {"disabled": 0, "roll": 1, "pitch": 2}[imu["balance_axis"]]
        commands = [
            protocol.configure("TANK1_FULL", config["tanks"]["1"]["full_stroke"]),
            protocol.configure("TANK2_FULL", config["tanks"]["2"]["full_stroke"]),
            protocol.configure("HEADING_OFFSET_CDEG", round(imu["heading_offset_deg"] * 100)),
            protocol.configure("HEADING_SIGN", imu["heading_sign"]),
            protocol.configure("BALANCE_AXIS", balance_axis),
            protocol.configure("BALANCE_SIGN", imu["balance_sign"]),
            protocol.configure("LEVEL_KP_X100", round(imu["level_kp_pct_per_degree"] * 100)),
            protocol.configure("MAX_BALANCE_PCT", round(imu["maximum_balance_pct"])),
            protocol.configure("ROUTE", 0 if autonomy["route"] == "test" else 1),
            protocol.configure("DIVE_BALLAST_PCT", round(autonomy["dive_ballast_pct"])),
            protocol.configure("HOVER_BALLAST_PCT", round(autonomy["hover_ballast_pct"])),
            protocol.configure("DIVE_MS", round(autonomy["dive_seconds"] * 1000)),
            protocol.configure(
                "HOVER_SETTLE_MS", round(autonomy["hover_settle_seconds"] * 1000)
            ),
            protocol.configure(
                "TEST_FORWARD_MS", round(autonomy["test_forward_seconds"] * 1000)
            ),
            protocol.configure("TEST_FORWARD_PCT", round(autonomy["test_forward_power_pct"])),
            protocol.configure(
                "AUTO_DELAY_MS",
                autonomy["auto_start_delay_seconds"] * 1000
                if autonomy["auto_start_enabled"] else 0,
            ),
            protocol.configure("AUTO_ZERO_TANKS", 1 if autonomy["auto_zero_tanks"] else 0),
            protocol.configure("STRAIGHT_MS", round(mission["straight_seconds"] * 1000)),
            protocol.configure("FORWARD_PCT", round(mission["forward_power_pct"])),
            protocol.configure("CIRCLE_FORWARD_PCT", round(mission["circle_forward_power_pct"])),
            protocol.configure("CIRCLE_TURN_PCT", round(mission["circle_turn_power_pct"])),
            protocol.configure("HEADING_KP_X100", round(mission["heading_kp"] * 100)),
        ]
        for command in commands:
            self._send_locked(command)

    def _require_auto_prerequisites_locked(self, config: dict[str, Any]) -> None:
        if self.store.full_stroke(1) <= 0 or self.store.full_stroke(2) <= 0:
            raise StationError("Her iki balast tankı da kalibre edilmeli", "not_calibrated", 409)
        if not config["autonomy"]["auto_zero_tanks"]:
            self._require_fresh_tank(1)
            self._require_fresh_tank(2)
            if not self._tanks[0].zeroed or not self._tanks[1].zeroed:
                raise StationError(
                    "Her iki balast tankı boş konumda sıfırlanmalı",
                    "not_zeroed",
                    409,
                )
            for index, tank in enumerate(self._tanks, start=1):
                empty_limit = max(2, self.store.full_stroke(index) // 100)
                if tank.live_position > empty_limit:
                    raise StationError(
                        "Otomatik sayaç için iki tank da önce %0 konumuna getirilmeli",
                        "tank_not_empty",
                        409,
                    )

    def _try_auto_schedule_locked(self) -> bool:
        if (
            not self._connected
            or not self.transport.connected
            or not self._configuration_pushed
            or self._auto_schedule_sent
            or self._mission["active"]
            or self._estop_latched
        ):
            return False
        config = self.store.configuration()
        if not config["autonomy"]["auto_start_enabled"]:
            return False
        try:
            self._require_auto_prerequisites_locked(config)
        except StationError:
            return False
        self._send_locked("AUTO PREPARE" if self._auto_rearm_pending else "AUTO SCHEDULE")
        now = time.monotonic()
        self._mission.update(
            state="COUNTDOWN", elapsed_ms=0, fault="NONE", active=True, updated_at=now
        )
        self._armed = False
        self._thrusters = [0.0, 0.0]
        self._auto_schedule_sent = True
        self._auto_rearm_pending = False
        self._append_log("mission", "Kayıtlı otomatik başlatma sayacı başladı")
        return True

    @staticmethod
    def _percent(position: int, full: int) -> float | None:
        if full <= 0:
            return None
        return round(max(0.0, min(100.0, position * 100.0 / full)), 1)
