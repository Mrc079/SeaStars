"""Small, atomic persistence layer for calibration and autonomy data."""

from __future__ import annotations

import json
import os
from pathlib import Path
import threading
from typing import Any


DEFAULT_STATE: dict[str, Any] = {
    "version": 4,
    "tanks": {
        "1": {"full_stroke": 0},
        "2": {"full_stroke": 0},
    },
    # Kept only for optional D300 diagnostics and migration from v2 files.
    # The autonomous mission does not use depth feedback.
    "depth": {
        "sensor_offset_cm": 0.0,
        "fluid_density_kg_m3": 997.0,
    },
    "imu": {
        "heading_offset_deg": 0.0,
        "heading_sign": 1,
        "balance_axis": "roll",
        "balance_sign": 1,
        "level_kp_pct_per_degree": 0.5,
        "maximum_balance_pct": 12.0,
    },
    "autonomy": {
        "route": "test",
        "auto_start_enabled": False,
        "auto_start_delay_seconds": 60,
        "auto_zero_tanks": False,
        "dive_ballast_pct": 30.0,
        "dive_seconds": 5.0,
        "hover_ballast_pct": 20.0,
        "hover_settle_seconds": 3.0,
        "test_forward_seconds": 5.0,
        "test_forward_power_pct": 15.0,
    },
    "mission": {
        "straight_seconds": 16.0,
        "forward_power_pct": 30.0,
        "circle_forward_power_pct": 30.0,
        "circle_turn_power_pct": 12.0,
        "heading_kp": 0.8,
    },
}


class StateStore:
    def __init__(self, path: Path):
        self.path = path
        self._lock = threading.RLock()
        self._data = self._load()

    def _load(self) -> dict[str, Any]:
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
            tanks = raw.get("tanks", {})
            depth = raw.get("depth", {})
            imu = raw.get("imu", {})
            autonomy = raw.get("autonomy", {})
            mission = raw.get("mission", {})
            legacy_auto_delay = int(autonomy.get("auto_start_delay_seconds", 0))
            loaded = {
                "version": 4,
                "tanks": {
                    "1": {"full_stroke": max(0, int(tanks.get("1", {}).get("full_stroke", 0)))},
                    "2": {"full_stroke": max(0, int(tanks.get("2", {}).get("full_stroke", 0)))},
                },
                "depth": {
                    "sensor_offset_cm": float(depth.get("sensor_offset_cm", 0.0)),
                    "fluid_density_kg_m3": float(depth.get("fluid_density_kg_m3", 997.0)),
                },
                "imu": {
                    "heading_offset_deg": float(imu.get("heading_offset_deg", 0.0)),
                    "heading_sign": int(imu.get("heading_sign", 1)),
                    "balance_axis": str(imu.get("balance_axis", "roll")),
                    "balance_sign": int(imu.get("balance_sign", 1)),
                    "level_kp_pct_per_degree": float(imu.get("level_kp_pct_per_degree", 0.5)),
                    "maximum_balance_pct": float(imu.get("maximum_balance_pct", 12.0)),
                },
                "autonomy": {
                    "route": str(autonomy.get("route", "test")),
                    "auto_start_enabled": (
                        autonomy.get("auto_start_enabled") is True
                        if "auto_start_enabled" in autonomy
                        else legacy_auto_delay > 0
                    ),
                    "auto_start_delay_seconds": legacy_auto_delay if legacy_auto_delay > 0 else 60,
                    "auto_zero_tanks": autonomy.get("auto_zero_tanks", False) is True,
                    **{
                        key: float(autonomy.get(key, default))
                        for key, default in DEFAULT_STATE["autonomy"].items()
                        if key not in {
                            "route", "auto_start_enabled", "auto_start_delay_seconds",
                            "auto_zero_tanks"
                        }
                    },
                },
                "mission": {
                    key: float(mission.get(key, default))
                    for key, default in DEFAULT_STATE["mission"].items()
                },
            }
            self._normalize_configuration(loaded)
            self._validate_configuration(loaded)
            return loaded
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            return json.loads(json.dumps(DEFAULT_STATE))

    def full_stroke(self, index: int) -> int:
        with self._lock:
            return int(self._data["tanks"][str(index)]["full_stroke"])

    def set_full_stroke(self, index: int, value: int) -> None:
        if index not in (1, 2) or not 1 <= value <= 1_000_000:
            raise ValueError("Tam strok 1–1.000.000 adım aralığında olmalı")
        with self._lock:
            self._data["tanks"][str(index)]["full_stroke"] = value
            self._write_atomic()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return json.loads(json.dumps(self._data))

    def configuration(self) -> dict[str, Any]:
        return self.snapshot()

    def update_configuration(self, changes: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(changes, dict):
            raise ValueError("Ayarlar JSON nesnesi olmalı")
        allowed_sections = {"tanks", "depth", "imu", "autonomy", "mission"}
        unknown_sections = set(changes) - allowed_sections
        if unknown_sections:
            raise ValueError(f"Bilinmeyen ayar bölümü: {', '.join(sorted(unknown_sections))}")
        with self._lock:
            candidate = json.loads(json.dumps(self._data))
            for section in ("depth", "imu", "autonomy", "mission"):
                incoming = changes.get(section)
                if incoming is not None:
                    if not isinstance(incoming, dict):
                        raise ValueError(f"{section} ayarları nesne olmalı")
                    unknown = set(incoming) - set(candidate[section])
                    if unknown:
                        raise ValueError(
                            f"Bilinmeyen {section} ayarı: {', '.join(sorted(unknown))}"
                        )
                    candidate[section].update(incoming)
            incoming_tanks = changes.get("tanks")
            if incoming_tanks is not None:
                if not isinstance(incoming_tanks, dict):
                    raise ValueError("Tank ayarları nesne olmalı")
                for index in ("1", "2"):
                    if index in incoming_tanks:
                        value = incoming_tanks[index]
                        if isinstance(value, dict):
                            value = value.get("full_stroke")
                        candidate["tanks"][index]["full_stroke"] = int(value)
            self._normalize_configuration(candidate)
            self._validate_configuration(candidate)
            self._data = candidate
            self._write_atomic()
            return json.loads(json.dumps(self._data))

    @staticmethod
    def _normalize_configuration(data: dict[str, Any]) -> None:
        for index in ("1", "2"):
            data["tanks"][index]["full_stroke"] = int(data["tanks"][index]["full_stroke"])
        for key in ("sensor_offset_cm", "fluid_density_kg_m3"):
            data["depth"][key] = float(data["depth"][key])
        data["imu"]["heading_offset_deg"] = float(data["imu"]["heading_offset_deg"])
        data["imu"]["heading_sign"] = int(data["imu"]["heading_sign"])
        data["imu"]["balance_sign"] = int(data["imu"]["balance_sign"])
        data["imu"]["balance_axis"] = str(data["imu"]["balance_axis"]).lower()
        data["imu"]["level_kp_pct_per_degree"] = float(
            data["imu"]["level_kp_pct_per_degree"]
        )
        data["imu"]["maximum_balance_pct"] = float(data["imu"]["maximum_balance_pct"])
        data["autonomy"]["route"] = str(data["autonomy"]["route"]).lower()
        if not isinstance(data["autonomy"]["auto_start_enabled"], bool):
            raise ValueError("Otomatik başlatma değeri doğru/yanlış olmalı")
        data["autonomy"]["auto_start_delay_seconds"] = int(
            data["autonomy"]["auto_start_delay_seconds"]
        )
        if not isinstance(data["autonomy"]["auto_zero_tanks"], bool):
            raise ValueError("Otomatik tank sıfırlama değeri doğru/yanlış olmalı")
        for key in (
            "dive_ballast_pct",
            "dive_seconds",
            "hover_ballast_pct",
            "hover_settle_seconds",
            "test_forward_seconds",
            "test_forward_power_pct",
        ):
            data["autonomy"][key] = float(data["autonomy"][key])
        for key in data["mission"]:
            data["mission"][key] = float(data["mission"][key])

    @staticmethod
    def _validate_configuration(data: dict[str, Any]) -> None:
        for index in ("1", "2"):
            full = int(data["tanks"][index]["full_stroke"])
            if not 0 <= full <= 1_000_000:
                raise ValueError(f"Tank {index} tam strok 0–1.000.000 adım aralığında olmalı")
        depth = data["depth"]
        if not -100.0 <= float(depth["sensor_offset_cm"]) <= 100.0:
            raise ValueError("Sensör montaj ofseti -100 ile 100 cm arasında olmalı")
        if not 950.0 <= float(depth["fluid_density_kg_m3"]) <= 1100.0:
            raise ValueError("Sıvı yoğunluğu 950–1100 kg/m³ aralığında olmalı")
        imu = data["imu"]
        if not -180.0 <= float(imu["heading_offset_deg"]) <= 180.0:
            raise ValueError("Yön ofseti -180 ile 180 derece arasında olmalı")
        if int(imu["heading_sign"]) not in (-1, 1) or int(imu["balance_sign"]) not in (-1, 1):
            raise ValueError("IMU yön işaretleri -1 veya 1 olmalı")
        if str(imu["balance_axis"]) not in ("roll", "pitch", "disabled"):
            raise ValueError("Denge ekseni roll, pitch veya disabled olmalı")
        if not 0.0 <= float(imu["level_kp_pct_per_degree"]) <= 5.0:
            raise ValueError("Denge Kp 0–5 %/derece aralığında olmalı")
        if not 0.0 <= float(imu["maximum_balance_pct"]) <= 40.0:
            raise ValueError("Maksimum denge farkı 0–40% aralığında olmalı")
        autonomy = data["autonomy"]
        if autonomy["route"] not in ("test", "competition"):
            raise ValueError("Otonomi rotası test veya competition olmalı")
        if not 1 <= int(autonomy["auto_start_delay_seconds"]) <= 86_400:
            raise ValueError("Otomatik başlatma süresi 1–86.400 saniye aralığında olmalı")
        for key in ("dive_ballast_pct", "hover_ballast_pct", "test_forward_power_pct"):
            if not 0.0 <= float(autonomy[key]) <= 100.0:
                raise ValueError(f"{key} 0–100 aralığında olmalı")
        if not 0.5 <= float(autonomy["dive_seconds"]) <= 120.0:
            raise ValueError("Dalış süresi 0.5–120 saniye aralığında olmalı")
        if not 0.0 <= float(autonomy["hover_settle_seconds"]) <= 60.0:
            raise ValueError("Askıda dengeleme süresi 0–60 saniye aralığında olmalı")
        if not 1.0 <= float(autonomy["test_forward_seconds"]) <= 120.0:
            raise ValueError("Test ileri gidiş süresi 1–120 saniye aralığında olmalı")
        mission = data["mission"]
        if not 15.0 <= float(mission["straight_seconds"]) <= 120.0:
            raise ValueError("Yarışma için düz gidiş 15–120 saniye olmalı")
        for key in ("forward_power_pct", "circle_forward_power_pct", "circle_turn_power_pct"):
            if not 1.0 <= float(mission[key]) <= 100.0:
                raise ValueError(f"{key} 1–100 aralığında olmalı")
        if not 0.05 <= float(mission["heading_kp"]) <= 5.0:
            raise ValueError("Heading Kp 0.05–5 aralığında olmalı")

    def _write_atomic(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(self.path.suffix + ".tmp")
        temporary.write_text(
            json.dumps(self._data, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, self.path)
