"""Local-only HTTP API and static file server (standard library)."""

from __future__ import annotations

from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

from .controller import StationController, StationError


class StationHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], controller: StationController, static_root: Path):
        super().__init__(address, StationRequestHandler)
        self.controller = controller
        self.static_root = static_root.resolve()


class StationRequestHandler(BaseHTTPRequestHandler):
    server: StationHTTPServer
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        try:
            if parsed.path == "/api/status":
                self._json(self.server.controller.snapshot())
            elif parsed.path == "/api/ports":
                controller = self.server.controller
                self._json(
                    {
                        "ports": controller.list_ports(),
                        "connected": controller.transport.port,
                        "error": controller.snapshot()["connection_error"],
                    }
                )
            elif parsed.path == "/api/log":
                raw = parse_qs(parsed.query).get("since", ["0"])[0]
                self._json(self.server.controller.logs_since(max(0, int(raw))))
            elif parsed.path == "/api/configuration":
                self._json(self.server.controller.configuration())
            else:
                self._static(parsed.path)
        except (ValueError, StationError) as exc:
            self._error(exc)
        except Exception as exc:
            self._error(StationError(f"Sunucu hatası: {exc}", "internal_error", 500))

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        try:
            body = self._read_json()
            result = self._dispatch_post(parsed.path, body)
            self._json({"ok": True, **(result or {})})
        except StationError as exc:
            self._error(exc)
        except (TypeError, ValueError, KeyError, json.JSONDecodeError) as exc:
            self._error(StationError(f"Geçersiz istek: {exc}", "invalid_request", 400))
        except Exception as exc:
            self._error(StationError(f"Sunucu hatası: {exc}", "internal_error", 500))

    def _dispatch_post(self, path: str, body: dict[str, Any]) -> dict[str, Any] | None:
        controller = self.server.controller
        if path == "/api/connect":
            controller.connect(str(body["port"]))
            return None
        if path == "/api/disconnect":
            controller.disconnect()
            return None
        if path == "/api/heartbeat":
            controller.heartbeat()
            return None
        if path == "/api/configuration":
            return {"configuration": controller.save_configuration(body)}
        if path == "/api/calibration/depth-zero":
            controller.calibrate_depth_surface()
            return None
        if path == "/api/calibration/heading-zero":
            return {"heading_offset_deg": controller.calibrate_heading_zero()}
        if path == "/api/mission/start":
            controller.start_mission()
            return None
        if path == "/api/mission/schedule":
            controller.schedule_mission(rearm=True)
            return None
        if path == "/api/mission/abort":
            controller.abort_mission()
            return None
        if path == "/api/maneuver/turn":
            controller.turn_relative(float(body["degrees"]))
            return None
        if path == "/api/esc/arm":
            controller.arm()
            return None
        if path == "/api/esc/disarm":
            controller.disarm()
            return None
        if path == "/api/esc/both":
            controller.set_both_thrusters(float(body["percent"]))
            return None
        if path == "/api/esc/pair":
            controller.set_thruster_pair(float(body["left"]), float(body["right"]))
            return None
        if path == "/api/estop":
            controller.emergency_stop()
            return None
        if path == "/api/estop/reset":
            controller.clear_estop()
            return None
        if path == "/api/tanks/goto":
            controller.goto_both_tanks(float(body["percent"]))
            return None
        if path == "/api/cmd":
            controller.raw_command(str(body["cmd"]))
            return None

        parts = [part for part in path.split("/") if part]
        if len(parts) == 4 and parts[:2] == ["api", "tank"]:
            index, action = int(parts[2]), parts[3]
            if action == "goto":
                controller.goto_tank(index, float(body["percent"]))
            elif action == "jog":
                controller.jog_tank(index, int(body["steps"]))
            elif action == "speed":
                controller.set_tank_speed(index, int(body["sps"]))
            elif action == "zero":
                controller.zero_tank(index)
            elif action == "setfull":
                return {"full_stroke": controller.set_full_stroke(index)}
            elif action == "manualfull":
                return {"full_stroke": controller.set_full_stroke(index, int(body["val"]))}
            else:
                raise StationError(f"Bilinmeyen tank işlemi: {action}", "not_found", 404)
            return None
        if len(parts) == 4 and parts[:2] == ["api", "esc"] and parts[3] == "set":
            controller.set_thruster(int(parts[2]), float(body["percent"]))
            return None
        raise StationError(f"Bilinmeyen uç nokta: {path}", "not_found", 404)

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length > 16_384:
            raise StationError("İstek gövdesi çok büyük", "payload_too_large", 413)
        if length == 0:
            return {}
        value = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("JSON nesnesi bekleniyor")
        return value

    def _static(self, request_path: str) -> None:
        relative = "index.html" if request_path == "/" else unquote(request_path).lstrip("/")
        target = (self.server.static_root / relative).resolve()
        if self.server.static_root not in target.parents and target != self.server.static_root:
            self._json({"ok": False, "error": "not_found"}, 404)
            return
        if not target.is_file():
            self._json({"ok": False, "error": "not_found"}, 404)
            return
        payload = target.read_bytes()
        mime = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        if mime.startswith("text/") or mime in ("application/javascript", "application/json"):
            mime += "; charset=utf-8"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(payload)

    def _json(self, value: dict[str, Any], status: int = 200) -> None:
        payload = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(payload)

    def _error(self, error: Exception) -> None:
        if isinstance(error, StationError):
            self._json({"ok": False, "error": error.code, "message": str(error)}, error.status)
        else:
            self._json({"ok": False, "error": "invalid_request", "message": str(error)}, 400)

    def log_message(self, format: str, *args: object) -> None:
        # Keep the operator terminal quiet; operational events live in the app log.
        return
