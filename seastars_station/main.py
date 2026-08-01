"""Application composition and CLI."""

from __future__ import annotations

import argparse
from pathlib import Path
import threading
import webbrowser

from .controller import StationController
from .state_store import StateStore
from .transport import SerialTransport, SimulatorTransport
from .web import StationHTTPServer


def build_application(*, simulate: bool, developer_mode: bool, state_path: Path) -> StationController:
    holder: dict[str, StationController] = {}

    def on_line(line: str) -> None:
        holder["controller"].on_line(line)

    def on_state(connected: bool, error: str | None) -> None:
        holder["controller"].on_transport_state(connected, error)

    transport = SimulatorTransport(on_line, on_state) if simulate else SerialTransport(on_line, on_state)
    controller = StationController(
        transport,
        StateStore(state_path),
        developer_mode=developer_mode,
    )
    holder["controller"] = controller
    return controller


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="SEA STARS görev kontrol istasyonu")
    parser.add_argument("--sim", action="store_true", help="STM32 yerine güvenli simülatörü kullan")
    parser.add_argument("--developer-mode", action="store_true", help="Ham seri komut konsolunu aç")
    parser.add_argument("--no-browser", action="store_true", help="Tarayıcıyı otomatik açma")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP dinleme adresi (varsayılan: yalnızca yerel)")
    parser.add_argument("--port", type=int, default=5000, help="HTTP portu")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    project_root = Path(__file__).resolve().parent.parent
    state_name = "station_state.sim.json" if args.sim else "station_state.hardware.json"
    controller = build_application(
        simulate=args.sim,
        developer_mode=args.developer_mode,
        state_path=project_root / "runtime" / state_name,
    )
    server = StationHTTPServer((args.host, args.port), controller, Path(__file__).parent / "static")
    url = f"http://{args.host}:{args.port}"
    print(f"SEA STARS Görev Kontrol v3 — {url}")
    print("MOD: SİMÜLATÖR" if args.sim else "MOD: GERÇEK DONANIM")
    print("Kapatmak için Ctrl+C")
    if not args.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        print("\nGüvenli kapatma yapılıyor…")
    finally:
        server.server_close()
        controller.close()
