#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SEA STARS — Web Kontrol & Balast Yonetimi (v2)
===============================================
Ozellikler:
- Yatay tank gorseli, GERCEK ANLIK dolum (STM32 POS yayinini okur)
- Manuel tam-strok girisi (elle adim sayisi yaz)
- Iki balast birlikte kontrol (senkron)
- Yuzde tabanli itici guc (ileri/geri)
- Kalibrasyon + dosya saklama
- ACIL STOP

GEREKSINIM:  pip install flask pyserial
CALISTIRMA:  python seastars_web.py  -> tarayici otomatik acilir
"""

import json, os, threading, time, webbrowser, re

try:
    import serial, serial.tools.list_ports
except ImportError:
    raise SystemExit("pyserial yok:  pip install pyserial")
try:
    from flask import Flask, request, jsonify, Response
except ImportError:
    raise SystemExit("flask yok:  pip install flask")

BAUD = 115200
STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seastars_state.json")

DEFAULT_STATE = {
    "tank1": {"pos": 0, "full": 0},
    "tank2": {"pos": 0, "full": 0},
}
state_lock = threading.Lock()

def load_state():
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as f:
            s = json.load(f)
            for k in ("tank1", "tank2"):
                if k not in s: s[k] = dict(DEFAULT_STATE[k])
                s[k].setdefault("pos", 0); s[k].setdefault("full", 0)
            return s
    except Exception:
        return json.loads(json.dumps(DEFAULT_STATE))

def save_state(s):
    try:
        with open(STATE_FILE, "w", encoding="utf-8") as f:
            json.dump(s, f, indent=2)
    except Exception as e:
        print("State kaydetme hatasi:", e)

state = load_state()

# Anlik (canli) pozisyon - STM32 yayinindan gelir, hareket sirasinda guncellenir
live_pos = {"tank1": state["tank1"]["pos"], "tank2": state["tank2"]["pos"]}
live_lock = threading.Lock()

# IMU verisi - STM32 "IMU H:.. R:.. P:.. C:.." yayinindan gelir
imu_data = {"heading": 0, "roll": 0, "pitch": 0, "calib": 0, "ok": False}
imu_lock = threading.Lock()

def tank_percent(pos, full):
    if full <= 0: return None
    return max(0, min(100, round(pos * 100.0 / full, 1)))

# ----------------------------------------------------------------------------
class SerialManager:
    def __init__(self):
        self.ser = None
        self.reader_running = False
        self.rx_lines = []
        self.rx_lock = threading.Lock()
        self.connected_port = None

    def list_ports(self):
        return [p.device for p in serial.tools.list_ports.comports()]

    def connect(self, port):
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.1)
            time.sleep(0.3)
            self.reader_running = True
            self.connected_port = port
            threading.Thread(target=self._reader, daemon=True).start()
            self._log(f">>> Baglandi: {port}")
            return True, port
        except Exception as e:
            return False, str(e)

    def disconnect(self):
        self.reader_running = False
        time.sleep(0.15)
        if self.ser and self.ser.is_open:
            try: self.ser.write(b"STOP\n"); time.sleep(0.1)
            except Exception: pass
            self.ser.close()
        self.connected_port = None
        self._log(">>> Baglanti kesildi")

    def is_connected(self):
        return self.ser is not None and self.ser.is_open

    def send(self, cmd):
        if self.is_connected():
            try:
                self.ser.write((cmd + "\n").encode())
                self._log("TX: " + cmd)
                return True
            except Exception as e:
                self._log("!! Gonderme hatasi: " + str(e)); return False
        else:
            self._log("!! Bagli degil: " + cmd); return False

    def _reader(self):
        buf = b""
        pos_re = re.compile(r"POS1:(-?\d+)\s+POS2:(-?\d+)")
        imu_re = re.compile(r"IMU H:(-?\d+) R:(-?\d+) P:(-?\d+) C:(\d+)")
        while self.reader_running and self.is_connected():
            try:
                d = self.ser.read(256)
                if d:
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        t = line.decode(errors="replace").strip()
                        if t:
                            # Anlik pozisyon yayini mi?
                            m = pos_re.search(t)
                            mi = imu_re.search(t)
                            if m:
                                with live_lock:
                                    live_pos["tank1"] = int(m.group(1))
                                    live_pos["tank2"] = int(m.group(2))
                                # POS yayinini monitore basma (spam olmasin)
                            elif mi:
                                with imu_lock:
                                    imu_data["heading"] = int(mi.group(1))
                                    imu_data["roll"] = int(mi.group(2))
                                    imu_data["pitch"] = int(mi.group(3))
                                    imu_data["calib"] = int(mi.group(4))
                                    imu_data["ok"] = True
                                # IMU yayinini da monitore basma (spam)
                            else:
                                self._log("RX: " + t)
            except Exception:
                break
            time.sleep(0.02)

    def _log(self, msg):
        with self.rx_lock:
            self.rx_lines.append(msg)
            if len(self.rx_lines) > 200:
                self.rx_lines = self.rx_lines[-200:]

    def get_log(self, since=0):
        with self.rx_lock:
            return self.rx_lines[since:], len(self.rx_lines)

sm = SerialManager()

# ----------------------------------------------------------------------------
# Balast mantigi
# ----------------------------------------------------------------------------
def _apply_move(tank_no, delta):
    """STM32'ye goreli adim gonder, hedef pozisyonu state'e yaz."""
    key = f"tank{tank_no}"
    with state_lock:
        state[key]["pos"] += delta
        save_state(state)
    sm.send(f"S{tank_no} {delta}")

def goto_percent(tank_no, pct):
    key = f"tank{tank_no}"
    with state_lock:
        full = state[key]["full"]
        cur = state[key]["pos"]
    if full <= 0:
        return False, f"Tank {tank_no}: once tam-strok gir/kalibre et"
    target = int(round(pct * full / 100.0))
    delta = target - cur
    _apply_move(tank_no, delta)
    return True, f"Tank {tank_no} -> %{pct} ({delta:+d} adim)"

def goto_both(pct):
    msgs = []
    for n in (1, 2):
        ok, m = goto_percent(n, pct)
        msgs.append(m)
    return True, " | ".join(msgs)

def jog(tank_no, steps):
    _apply_move(tank_no, steps)
    return True, f"Tank {tank_no} {steps:+d} adim"

def set_zero(tank_no):
    key = f"tank{tank_no}"
    with state_lock:
        state[key]["pos"] = 0
        save_state(state)
    with live_lock:
        live_pos[key] = 0
    sm.send(f"ZERO{tank_no}")
    return True, f"Tank {tank_no}: bu nokta = 0 (bos)"

def set_full_here(tank_no):
    """Mevcut pozisyonu tam dolu kabul et."""
    key = f"tank{tank_no}"
    with state_lock:
        if state[key]["pos"] <= 0:
            return False, "Once bos noktada 0 yap, doldur, sonra bas"
        state[key]["full"] = state[key]["pos"]
        save_state(state)
    return True, f"Tank {tank_no}: tam dolu = {state[key]['full']} adim"

def set_full_manual(tank_no, val):
    """Elle tam-strok adim sayisi gir."""
    key = f"tank{tank_no}"
    with state_lock:
        state[key]["full"] = int(val)
        save_state(state)
    return True, f"Tank {tank_no}: tam-strok = {int(val)} adim (manuel)"

def sync_to_stm():
    with state_lock:
        p1, p2 = state["tank1"]["pos"], state["tank2"]["pos"]
    sm.send(f"SETPOS1 {p1}"); time.sleep(0.05)
    sm.send(f"SETPOS2 {p2}")
    with live_lock:
        live_pos["tank1"] = p1; live_pos["tank2"] = p2

# ----------------------------------------------------------------------------
app = Flask(__name__)

@app.route("/")
def index(): return Response(HTML_PAGE, mimetype="text/html")

@app.route("/api/ports")
def api_ports(): return jsonify({"ports": sm.list_ports(), "connected": sm.connected_port})

@app.route("/api/connect", methods=["POST"])
def api_connect():
    ok, info = sm.connect(request.json.get("port"))
    if ok: time.sleep(0.3); sync_to_stm()
    return jsonify({"ok": ok, "info": info})

@app.route("/api/disconnect", methods=["POST"])
def api_disconnect(): sm.disconnect(); return jsonify({"ok": True})

@app.route("/api/status")
def api_status():
    with state_lock:
        t1f, t2f = state["tank1"]["full"], state["tank2"]["full"]
        t1t, t2t = state["tank1"]["pos"], state["tank2"]["pos"]
    with live_lock:
        l1, l2 = live_pos["tank1"], live_pos["tank2"]
    with imu_lock:
        imu = dict(imu_data)
    return jsonify({
        "connected": sm.is_connected(), "port": sm.connected_port,
        "tank1": {"live_pos": l1, "target_pos": t1t, "full": t1f,
                  "percent": tank_percent(l1, t1f), "target_percent": tank_percent(t1t, t1f)},
        "tank2": {"live_pos": l2, "target_pos": t2t, "full": t2f,
                  "percent": tank_percent(l2, t2f), "target_percent": tank_percent(t2t, t2f)},
        "imu": imu,
    })

@app.route("/api/log")
def api_log():
    since = int(request.args.get("since", 0))
    lines, total = sm.get_log(since)
    return jsonify({"lines": lines, "total": total})

@app.route("/api/cmd", methods=["POST"])
def api_cmd(): return jsonify({"ok": sm.send(request.json.get("cmd", ""))})

# Balast
@app.route("/api/tank/<int:n>/zero", methods=["POST"])
def api_zero(n): ok, m = set_zero(n); return jsonify({"ok": ok, "msg": m})
@app.route("/api/tank/<int:n>/setfull", methods=["POST"])
def api_setfull(n): ok, m = set_full_here(n); return jsonify({"ok": ok, "msg": m})
@app.route("/api/tank/<int:n>/manualfull", methods=["POST"])
def api_manualfull(n):
    ok, m = set_full_manual(n, request.json.get("val", 0)); return jsonify({"ok": ok, "msg": m})
@app.route("/api/tank/<int:n>/goto", methods=["POST"])
def api_goto(n):
    ok, m = goto_percent(n, float(request.json.get("percent", 0))); return jsonify({"ok": ok, "msg": m})
@app.route("/api/tank/<int:n>/jog", methods=["POST"])
def api_jog(n):
    ok, m = jog(n, int(request.json.get("steps", 0))); return jsonify({"ok": ok, "msg": m})
@app.route("/api/tank/<int:n>/speed", methods=["POST"])
def api_speed(n): sm.send(f"V{n} {int(request.json.get('sps',800))}"); return jsonify({"ok": True})
@app.route("/api/tanks/goto", methods=["POST"])
def api_goto_both():
    ok, m = goto_both(float(request.json.get("percent", 0))); return jsonify({"ok": ok, "msg": m})

# Iticiler (yuzde tabanli: %-100..+100 -> komut -200..+200)
@app.route("/api/esc/arm", methods=["POST"])
def api_arm(): sm.send("ARM"); return jsonify({"ok": True})
@app.route("/api/esc/disarm", methods=["POST"])
def api_disarm(): sm.send("DISARM"); return jsonify({"ok": True})
@app.route("/api/esc/<int:n>/set", methods=["POST"])
def api_esc_set(n):
    pct = float(request.json.get("percent", 0))  # -100..+100
    cmd = int(round(pct * 2))  # %100 -> komut 200 (guvenlik siniri)
    sm.send(f"T{n} {cmd}"); return jsonify({"ok": True})
@app.route("/api/esc/both", methods=["POST"])
def api_esc_both():
    pct = float(request.json.get("percent", 0))
    cmd = int(round(pct * 2))
    sm.send(f"T1 {cmd}"); time.sleep(0.03); sm.send(f"T2 {cmd}")
    return jsonify({"ok": True})

@app.route("/api/estop", methods=["POST"])
def api_estop(): sm.send("STOP"); return jsonify({"ok": True})

HTML_PAGE = r"""<!DOCTYPE html>
<html lang="tr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>SEA STARS — Görev Kontrol</title>
<style>
  :root{
    --bg:#eef2f6; --panel:#ffffff; --ink:#0d1f2d; --sub:#5a7184;
    --line:#dbe3ec; --line2:#c3cfdb;
    --deep:#0a3d5c; --deep2:#0d5580;
    --water:#2b9fd4; --waterTop:#5ec5ea; --waterDeep:#137aad;
    --live:#16a34a; --target:#f59e0b; --danger:#e11d48; --danger2:#be123c;
    --ok:#0f9d58; --steel:#64748b;
    --mono:'SF Mono','Cascadia Code',Consolas,monospace;
    --shadow:0 1px 2px rgba(13,31,45,.04),0 6px 20px rgba(13,31,45,.07);
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--ink);line-height:1.5;padding-bottom:40px}

  /* Ust bar */
  .bar{background:var(--deep);color:#fff;padding:0 20px;height:56px;display:flex;align-items:center;gap:16px;position:sticky;top:0;z-index:60;box-shadow:0 2px 12px rgba(10,61,92,.2)}
  .brand{font-weight:700;font-size:16px;letter-spacing:.5px;display:flex;align-items:center;gap:8px}
  .brand::before{content:"";width:10px;height:10px;border-radius:50%;background:var(--live);box-shadow:0 0 0 3px rgba(22,163,74,.3)}
  .brand small{font-weight:400;opacity:.7;font-size:13px;letter-spacing:0}
  .bar .conn{margin-left:auto;display:flex;align-items:center;gap:8px}
  .bar select{background:rgba(255,255,255,.12);color:#fff;border:1px solid rgba(255,255,255,.2);border-radius:7px;padding:6px 10px;font-size:13px;font-family:var(--mono)}
  .bar select option{color:#0d1f2d}
  .bar .st{font-size:13px;font-family:var(--mono);display:flex;align-items:center;gap:6px}
  .bar .st .d{width:8px;height:8px;border-radius:50%;background:var(--danger)}
  .bar .st .d.on{background:var(--live)}

  .btn{font-family:inherit;font-size:13px;font-weight:600;padding:7px 14px;border:none;border-radius:7px;cursor:pointer;transition:.14s;white-space:nowrap}
  .btn:active{transform:translateY(1px)}
  .btn:disabled{opacity:.4;cursor:not-allowed}
  .btn-p{background:var(--deep2);color:#fff}.btn-p:hover{background:var(--deep)}
  .btn-w{background:rgba(255,255,255,.15);color:#fff;border:1px solid rgba(255,255,255,.25)}.btn-w:hover{background:rgba(255,255,255,.25)}
  .btn-g{background:#eef2f6;color:var(--ink);border:1px solid var(--line)}.btn-g:hover{background:#e2e8f0}
  .btn-ok{background:var(--ok);color:#fff}.btn-ok:hover{filter:brightness(1.08)}
  .btn-xs{padding:5px 10px;font-size:12px}

  .wrap{max-width:1160px;margin:20px auto;padding:0 20px;display:flex;flex-direction:column;gap:18px}

  /* ACIL STOP */
  .estop{background:var(--danger);color:#fff;border:none;border-radius:12px;padding:15px;font-size:16px;font-weight:800;letter-spacing:.4px;cursor:pointer;box-shadow:0 4px 16px rgba(225,29,72,.28);transition:.14s;display:flex;align-items:center;justify-content:center;gap:10px}
  .estop:hover{background:var(--danger2)}.estop:active{transform:translateY(1px)}
  .estop::before{content:"⬛";font-size:14px}

  .panel{background:var(--panel);border:1px solid var(--line);border-radius:13px;box-shadow:var(--shadow);overflow:hidden}
  .ph{padding:13px 18px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:10px}
  .ph h2{font-size:14px;font-weight:700;letter-spacing:.2px;text-transform:uppercase;color:var(--deep)}
  .ph .tag{margin-left:auto;font-family:var(--mono);font-size:12px;padding:3px 9px;border-radius:6px;background:#eef2f6;color:var(--sub)}
  .pb{padding:18px}

  /* --- YATAY TANK --- */
  .tank-block{display:flex;flex-direction:column;gap:14px}
  .tank-unit{border:1px solid var(--line);border-radius:11px;padding:16px;background:linear-gradient(180deg,#fbfdff,#f4f8fb)}
  .tank-top{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}
  .tank-name{font-weight:700;font-size:14px;color:var(--deep);display:flex;align-items:center;gap:8px}
  .tank-name .id{font-family:var(--mono);font-size:11px;background:var(--deep);color:#fff;padding:2px 7px;border-radius:5px}
  .tank-readout{font-family:var(--mono);font-size:13px;color:var(--sub);display:flex;gap:14px;align-items:center}
  .tank-readout b{color:var(--ink);font-size:15px}
  .live-badge{display:inline-flex;align-items:center;gap:5px;font-size:12px;color:var(--live);font-weight:600}
  .live-badge::before{content:"";width:7px;height:7px;border-radius:50%;background:var(--live);animation:pulse 1.4s ease-in-out infinite}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}

  /* Yatay tank govdesi - su soldan saga */
  .tank-h{position:relative;height:72px;border:3px solid var(--line2);border-radius:12px;background:repeating-linear-gradient(90deg,#f0f5f9,#f0f5f9 39px,#e8eef4 39px,#e8eef4 40px);overflow:hidden}
  .tank-h .fill{position:absolute;top:0;bottom:0;left:0;background:linear-gradient(180deg,var(--waterTop),var(--water) 45%,var(--waterDeep));transition:width .25s linear;box-shadow:inset -12px 0 20px rgba(19,122,173,.35)}
  .tank-h .fill::after{content:"";position:absolute;top:0;bottom:0;right:0;width:5px;background:rgba(255,255,255,.4);animation:ripple 2.5s ease-in-out infinite}
  @keyframes ripple{0%,100%{opacity:.3;transform:scaleY(.85)}50%{opacity:.7;transform:scaleY(1)}}
  /* Hedef isareti */
  .tank-h .target-mark{position:absolute;top:-3px;bottom:-3px;width:3px;background:var(--target);z-index:3;transition:left .25s;box-shadow:0 0 6px rgba(245,158,11,.6)}
  .tank-h .target-mark::before{content:"HEDEF";position:absolute;top:-16px;left:50%;transform:translateX(-50%);font-size:9px;font-weight:700;color:var(--target);font-family:var(--mono);white-space:nowrap}
  .tank-h .pct-big{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-size:26px;font-weight:800;color:#fff;text-shadow:0 1px 4px rgba(10,61,92,.5);z-index:2;font-variant-numeric:tabular-nums}
  .tank-h .pct-big.empty{color:var(--sub);text-shadow:none}

  /* Preset + hedef */
  .presets{display:grid;grid-template-columns:repeat(5,1fr);gap:7px;margin-top:12px}
  .preset{padding:9px 4px;font-size:13px;font-weight:700;border:1px solid var(--line2);border-radius:8px;background:#fff;cursor:pointer;transition:.14s;text-align:center;line-height:1.2}
  .preset small{display:block;font-size:10px;font-weight:600;color:var(--sub);margin-top:1px}
  .preset:hover{background:var(--deep2);color:#fff;border-color:var(--deep2)}
  .preset:hover small{color:rgba(255,255,255,.8)}
  .tank-ctl{display:flex;gap:8px;align-items:center;margin-top:10px;flex-wrap:wrap}
  .tank-ctl .custom{display:flex;gap:6px;align-items:center}
  .tank-ctl input[type=number]{width:64px;padding:7px 8px;border:1px solid var(--line2);border-radius:7px;font-family:var(--mono);font-size:13px;text-align:center}

  /* Kalibrasyon satiri */
  .cal{margin-top:12px;padding-top:12px;border-top:1px dashed var(--line2);display:flex;gap:10px;align-items:center;flex-wrap:wrap}
  .cal .lbl{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.4px;color:var(--sub)}
  .cal input[type=number]{width:80px;padding:6px 8px;border:1px solid var(--line2);border-radius:7px;font-family:var(--mono);font-size:13px;text-align:center}
  .cal-state{font-size:12px;font-family:var(--mono)}
  .cal-state.ok{color:var(--ok)}.cal-state.no{color:var(--target)}

  .jog{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px;align-items:center}
  .jog .jl{font-size:11px;color:var(--sub);font-weight:600;text-transform:uppercase;letter-spacing:.3px}
  .spd{display:flex;gap:8px;align-items:center;margin-top:10px}
  .spd input[type=range]{flex:1;-webkit-appearance:none;height:5px;border-radius:3px;background:var(--line2)}
  .spd input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:16px;height:16px;border-radius:50%;background:var(--deep2);cursor:pointer}
  .spd .sv{font-family:var(--mono);font-size:12px;min-width:56px;color:var(--sub)}

  /* Senkron kontrol */
  .both-bar{background:linear-gradient(135deg,var(--deep),var(--deep2));color:#fff;border-radius:11px;padding:16px}
  .both-bar h3{font-size:13px;text-transform:uppercase;letter-spacing:.5px;margin-bottom:12px;opacity:.9;display:flex;align-items:center;gap:8px}
  .both-bar h3::before{content:"⇅";font-size:16px}
  .both-presets{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}
  .both-preset{padding:11px 4px;font-size:14px;font-weight:700;border:1px solid rgba(255,255,255,.25);border-radius:8px;background:rgba(255,255,255,.1);color:#fff;cursor:pointer;transition:.14s;text-align:center;line-height:1.2}
  .both-preset small{display:block;font-size:10px;opacity:.75;margin-top:1px}
  .both-preset:hover{background:rgba(255,255,255,.22)}
  .both-custom{display:flex;gap:8px;align-items:center;margin-top:10px}
  .both-custom input{width:70px;padding:8px;border:1px solid rgba(255,255,255,.3);border-radius:7px;background:rgba(255,255,255,.12);color:#fff;font-family:var(--mono);text-align:center}
  .both-custom input::placeholder{color:rgba(255,255,255,.5)}

  /* Iticiler */
  .esc-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
  @media(max-width:820px){.esc-grid{grid-template-columns:1fr}}
  .esc-unit{border:1px solid var(--line);border-radius:11px;padding:16px;background:linear-gradient(180deg,#fbfdff,#f4f8fb)}
  .esc-name{font-weight:700;font-size:13px;color:var(--deep);margin-bottom:14px;display:flex;align-items:center;gap:8px}
  .esc-name .id{font-family:var(--mono);font-size:11px;background:var(--steel);color:#fff;padding:2px 7px;border-radius:5px}
  .esc-power{text-align:center;margin-bottom:8px}
  .esc-power .v{font-size:30px;font-weight:800;font-variant-numeric:tabular-nums;line-height:1}
  .esc-power .v.fwd{color:var(--ok)}.esc-power .v.rev{color:var(--danger)}.esc-power .v.zero{color:var(--sub)}
  .esc-power .u{font-size:12px;color:var(--sub);font-weight:600}
  .esc-sl{width:100%;-webkit-appearance:none;height:8px;border-radius:4px;background:linear-gradient(90deg,var(--danger) 0%,#e2e8f0 42%,#e2e8f0 58%,var(--ok) 100%);margin:6px 0}
  .esc-sl::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#fff;border:3px solid var(--deep);cursor:pointer;box-shadow:0 2px 6px rgba(0,0,0,.25)}
  .esc-lbl{display:flex;justify-content:space-between;font-size:11px;color:var(--sub);font-family:var(--mono)}
  .esc-stop{margin-top:10px;width:100%}

  .arm-row{display:flex;gap:10px;align-items:center;margin-bottom:16px;flex-wrap:wrap}
  .arm-badge{font-family:var(--mono);font-size:12px;padding:4px 10px;border-radius:6px;font-weight:700}
  .arm-badge.on{background:#e8f6ee;color:var(--ok)}
  .arm-badge.off{background:#eef2f6;color:var(--sub)}
  .both-esc{margin-top:16px;padding-top:16px;border-top:1px dashed var(--line2)}
  .both-esc h4{font-size:12px;text-transform:uppercase;letter-spacing:.4px;color:var(--sub);margin-bottom:10px}

  /* Monitor */
  .mon{background:#08151f;color:#7fc9e8;font-family:var(--mono);font-size:12px;padding:12px;border-radius:9px;height:160px;overflow-y:auto;line-height:1.65}
  .mon .tx{color:#f0b429}.mon .rx{color:#7fc9e8}.mon .sys{color:#8399ab}
  .cmd-row{display:flex;gap:8px;margin-top:10px}
  .cmd-row input{flex:1;padding:8px 10px;border:1px solid var(--line);border-radius:7px;font-family:var(--mono);font-size:13px}

  .toast{position:fixed;bottom:22px;left:50%;transform:translateX(-50%) translateY(10px);background:var(--ink);color:#fff;padding:12px 20px;border-radius:9px;font-size:13px;font-weight:600;box-shadow:0 8px 24px rgba(0,0,0,.22);opacity:0;transition:.28s;z-index:100;pointer-events:none;font-family:var(--mono)}
  .toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
  .toast.err{background:var(--danger)}

  /* --- IMU PANELI --- */
  .imu-grid{display:grid;grid-template-columns:auto 1fr;gap:24px;align-items:center}
  @media(max-width:720px){.imu-grid{grid-template-columns:1fr;justify-items:center}}
  /* Pusula */
  .compass{position:relative;width:180px;height:180px;flex-shrink:0}
  .compass-ring{position:absolute;inset:0;border-radius:50%;border:3px solid var(--line2);background:radial-gradient(circle,#fff 55%,#f0f5f9);box-shadow:inset 0 2px 8px rgba(13,31,45,.08)}
  .compass-tick{position:absolute;left:50%;top:50%;font-family:var(--mono);font-size:12px;font-weight:700;color:var(--sub);transform-origin:0 0}
  .compass-tick.card{color:var(--deep);font-size:14px}
  .compass-needle{position:absolute;left:50%;top:50%;width:4px;height:72px;background:linear-gradient(180deg,var(--danger) 0%,var(--danger) 50%,var(--deep2) 50%,var(--deep2) 100%);transform-origin:50% 100%;transform:translate(-50%,-100%) rotate(0deg);transition:transform .25s ease-out;border-radius:2px;z-index:5}
  .compass-needle::before{content:"";position:absolute;top:-6px;left:50%;transform:translateX(-50%);border:5px solid transparent;border-bottom-color:var(--danger)}
  .compass-center{position:absolute;left:50%;top:50%;width:14px;height:14px;border-radius:50%;background:var(--deep);transform:translate(-50%,-50%);z-index:6;border:2px solid #fff}
  .compass-read{position:absolute;left:50%;bottom:-2px;transform:translateX(-50%);text-align:center}
  .compass-read .hv{font-size:28px;font-weight:800;color:var(--deep);font-variant-numeric:tabular-nums;line-height:1}
  .compass-read .hl{font-size:11px;color:var(--sub);font-weight:600}
  /* Aci gostergeleri */
  .imu-vals{display:flex;flex-direction:column;gap:14px;width:100%}
  .imu-row{display:flex;align-items:center;gap:12px}
  .imu-row .k{font-size:12px;font-weight:700;color:var(--sub);text-transform:uppercase;letter-spacing:.4px;width:64px}
  .imu-bar{flex:1;height:22px;background:#eef2f6;border-radius:6px;position:relative;overflow:hidden;border:1px solid var(--line)}
  .imu-bar .fillp{position:absolute;top:0;bottom:0;left:50%;background:var(--deep2);transition:.2s;opacity:.85}
  .imu-bar .mid{position:absolute;left:50%;top:0;bottom:0;width:1px;background:var(--line2)}
  .imu-row .val{font-family:var(--mono);font-size:15px;font-weight:700;width:64px;text-align:right;color:var(--ink)}
  /* Kalibrasyon */
  .calib-box{display:flex;gap:8px;margin-top:6px;flex-wrap:wrap}
  .calib-item{flex:1;min-width:70px;text-align:center;padding:8px 4px;border-radius:8px;border:1px solid var(--line);font-size:12px;background:#fff}
  .calib-item .cn{font-size:11px;color:var(--sub);font-weight:600;text-transform:uppercase;letter-spacing:.3px}
  .calib-item .cv{font-size:18px;font-weight:800;font-family:var(--mono);margin-top:2px}
  .calib-item .cv.c0{color:var(--danger)}.calib-item .cv.c1{color:var(--target)}.calib-item .cv.c2{color:#0891b2}.calib-item .cv.c3{color:var(--ok)}
  .imu-off{text-align:center;padding:30px;color:var(--sub);font-size:14px}
  .imu-off b{color:var(--target)}
</style>
</head>
<body>
<div class="bar">
  <div class="brand">SEA STARS <small>· görev kontrol</small></div>
  <div class="conn">
    <select id="portSel"><option>—</option></select>
    <button class="btn btn-w btn-xs" onclick="refreshPorts()">↻</button>
    <button class="btn btn-w btn-xs" id="connBtn" onclick="toggleConn()">Bağlan</button>
    <span class="st"><span class="d" id="cdot"></span><span id="ctxt">bağlı değil</span></span>
  </div>
</div>

<div class="wrap">
  <button class="estop" onclick="estop()">ACİL STOP — TÜM MOTORLAR DUR</button>

  <!-- IMU / YÖN -->
  <div class="panel">
    <div class="ph"><h2>Yön Sensörü (IMU)</h2><span class="tag" id="imuTag">bekleniyor</span></div>
    <div class="pb">
      <div id="imuOff" class="imu-off">Sensör verisi bekleniyor... <b>bağlan</b> ve sensörün takılı olduğundan emin ol.</div>
      <div id="imuOn" class="imu-grid" style="display:none">
        <div class="compass">
          <div class="compass-ring" id="compassRing"></div>
          <div class="compass-needle" id="needle"></div>
          <div class="compass-center"></div>
          <div class="compass-read"><div class="hv" id="hdgVal">—</div><div class="hl">HEADING °</div></div>
        </div>
        <div class="imu-vals">
          <div class="imu-row">
            <span class="k">Roll</span>
            <div class="imu-bar"><div class="mid"></div><div class="fillp" id="rollBar"></div></div>
            <span class="val" id="rollVal">0°</span>
          </div>
          <div class="imu-row">
            <span class="k">Pitch</span>
            <div class="imu-bar"><div class="mid"></div><div class="fillp" id="pitchBar"></div></div>
            <span class="val" id="pitchVal">0°</span>
          </div>
          <div style="margin-top:6px">
            <div style="font-size:12px;font-weight:700;color:var(--sub);text-transform:uppercase;letter-spacing:.4px;margin-bottom:6px">Kalibrasyon Durumu (3 = tam)</div>
            <div class="calib-box">
              <div class="calib-item"><div class="cn">Sistem</div><div class="cv c0" id="calSys">0</div></div>
              <div class="calib-item"><div class="cn">Jiro</div><div class="cv c0" id="calGyro">0</div></div>
              <div class="calib-item"><div class="cn">İvme</div><div class="cv c0" id="calAcc">0</div></div>
              <div class="calib-item"><div class="cn">Pusula</div><div class="cv c0" id="calMag">0</div></div>
            </div>
            <div style="font-size:12px;color:var(--sub);margin-top:8px">Pusula kalibrasyonu için aracı yatay düzlemde birkaç kez döndür. Sistem 3 olunca yön güvenilir.</div>
          </div>
        </div>
      </div>
    </div>
  </div>

  <!-- BALAST -->
  <div class="panel">
    <div class="ph"><h2>Balast Kontrol</h2><span class="tag" id="balTag">2 tank</span></div>
    <div class="pb">
      <div class="tank-block" id="tanks"></div>
      <!-- Senkron -->
      <div class="both-bar" style="margin-top:14px">
        <h3>İki Tankı Birlikte Ayarla</h3>
        <div class="both-presets">
          <div class="both-preset" onclick="gotoBoth(0)">Boşalt<small>%0</small></div>
          <div class="both-preset" onclick="gotoBoth(25)">%25</div>
          <div class="both-preset" onclick="gotoBoth(50)">%50</div>
          <div class="both-preset" onclick="gotoBoth(75)">%75</div>
          <div class="both-preset" onclick="gotoBoth(100)">Doldur<small>%100</small></div>
        </div>
        <div class="both-custom">
          <span style="font-size:13px;opacity:.85">Özel:</span>
          <input type="number" id="bothPct" placeholder="%" min="0" max="100">
          <button class="btn btn-w btn-xs" onclick="gotoBothCustom()">İkisini Gönder</button>
        </div>
      </div>
    </div>
  </div>

  <!-- ITICILER -->
  <div class="panel">
    <div class="ph"><h2>İticiler</h2><span class="tag" id="escTag">güç %</span></div>
    <div class="pb">
      <div class="arm-row">
        <button class="btn btn-ok btn-xs" onclick="arm()">ARM (hazırla)</button>
        <button class="btn btn-g btn-xs" onclick="disarm()">DISARM</button>
        <span class="arm-badge off" id="armBadge">DISARMED</span>
        <span style="font-size:12px;color:var(--sub)">Önce ARM → 2 sn bekle → güç ver. Test PERVANESİZ.</span>
      </div>
      <div class="esc-grid" id="escs"></div>
      <div class="both-esc">
        <h4>İki İticiyi Birlikte (düz git)</h4>
        <input type="range" class="esc-sl" id="ebSl" min="-100" max="100" value="0" step="5" oninput="ebLive(this.value)" onchange="ebSet(this.value)">
        <div class="esc-lbl"><span>◀ geri %100</span><span>dur</span><span>ileri %100 ▶</span></div>
        <div style="text-align:center;margin-top:8px"><span class="esc-power"><span class="v zero" id="ebVal" style="font-size:22px">0</span><span class="u"> %</span></span></div>
        <button class="btn btn-g btn-xs esc-stop" onclick="ebZero()">İkisini Durdur</button>
      </div>
    </div>
  </div>

  <!-- MONITOR -->
  <div class="panel">
    <div class="ph"><h2>Seri Monitör</h2></div>
    <div class="pb">
      <div class="mon" id="mon"></div>
      <div class="cmd-row">
        <input id="cmdIn" placeholder="komut (HELP, STATUS...)" onkeydown="if(event.key==='Enter')sendCmd()">
        <button class="btn btn-p btn-xs" onclick="sendCmd()">Gönder</button>
        <button class="btn btn-g btn-xs" onclick="document.getElementById('mon').innerHTML=''">Temizle</button>
      </div>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
let connected=false, logSince=0;
function toast(m,e){const t=document.getElementById('toast');t.textContent=m;t.className='toast show'+(e?' err':'');setTimeout(()=>t.className='toast',2200);}
async function api(p,b){const o={method:b?'POST':'GET',headers:{'Content-Type':'application/json'}};if(b)o.body=JSON.stringify(b);return (await fetch(p,o)).json();}

async function refreshPorts(){const d=await api('/api/ports');const s=document.getElementById('portSel');s.innerHTML='';if(!d.ports.length){s.innerHTML='<option>port yok</option>';}else{d.ports.forEach(p=>{const o=document.createElement('option');o.value=p;o.textContent=p;if(p===d.connected)o.selected=true;s.appendChild(o);});}}
async function toggleConn(){if(connected){await api('/api/disconnect',{});}else{const p=document.getElementById('portSel').value;const d=await api('/api/connect',{port:p});if(!d.ok){toast('bağlantı hatası: '+d.info,true);}else toast('bağlandı '+d.info);}}

// Tank kartı
function tankUnit(n){return `<div class="tank-unit">
  <div class="tank-top">
    <div class="tank-name"><span class="id">T${n}</span>Balast Tank ${n}</div>
    <div class="tank-readout">
      <span class="live-badge" id="t${n}live" style="display:none">canlı</span>
      <span>poz <b id="t${n}pos">0</b></span>
      <span>strok <b id="t${n}full">—</b></span>
    </div>
  </div>
  <div class="tank-h">
    <div class="fill" id="t${n}fill" style="width:0%"></div>
    <div class="target-mark" id="t${n}tgt" style="left:0%;display:none"></div>
    <div class="pct-big empty" id="t${n}pct">—</div>
  </div>
  <div class="presets">
    <div class="preset" onclick="goto(${n},0)">Boşalt<small>%0</small></div>
    <div class="preset" onclick="goto(${n},25)">%25</div>
    <div class="preset" onclick="goto(${n},50)">%50</div>
    <div class="preset" onclick="goto(${n},75)">%75</div>
    <div class="preset" onclick="goto(${n},100)">Doldur<small>%100</small></div>
  </div>
  <div class="tank-ctl">
    <div class="custom"><input type="number" id="t${n}goto" placeholder="%" min="0" max="100"><button class="btn btn-p btn-xs" onclick="gotoCustom(${n})">Git</button></div>
    <div class="spd" style="flex:1;margin-top:0">
      <span style="font-size:12px;color:var(--sub)">hız</span>
      <input type="range" id="t${n}spd" min="50" max="2000" step="50" value="800" onchange="setSpeed(${n},this.value)">
      <span class="sv" id="t${n}spdv">800/s</span>
    </div>
  </div>
  <div class="cal">
    <span class="lbl">Kalibrasyon:</span>
    <button class="btn btn-g btn-xs" onclick="zeroT(${n})">Bu nokta = 0</button>
    <span class="lbl">Tam strok</span>
    <input type="number" id="t${n}mf" placeholder="adım">
    <button class="btn btn-g btn-xs" onclick="manualFull(${n})">Kaydet</button>
    <button class="btn btn-g btn-xs" onclick="setFullHere(${n})">Buradası = %100</button>
    <span class="cal-state no" id="t${n}cs">kalibre değil</span>
  </div>
  <div class="jog">
    <span class="jl">manuel sür:</span>
    <button class="btn btn-g btn-xs" onclick="jog(${n},-400)">−400</button>
    <button class="btn btn-g btn-xs" onclick="jog(${n},-100)">−100</button>
    <button class="btn btn-g btn-xs" onclick="jog(${n},-20)">−20</button>
    <button class="btn btn-g btn-xs" onclick="jog(${n},20)">+20</button>
    <button class="btn btn-g btn-xs" onclick="jog(${n},100)">+100</button>
    <button class="btn btn-g btn-xs" onclick="jog(${n},400)">+400</button>
  </div>
</div>`;}

function escUnit(n){return `<div class="esc-unit">
  <div class="esc-name"><span class="id">E${n}</span>İtici ${n}</div>
  <div class="esc-power"><span class="v zero" id="e${n}v">0</span><span class="u"> % güç</span></div>
  <input type="range" class="esc-sl" id="e${n}sl" min="-100" max="100" value="0" step="5" oninput="escLive(${n},this.value)" onchange="escSet(${n},this.value)">
  <div class="esc-lbl"><span>◀ geri</span><span>dur</span><span>ileri ▶</span></div>
  <button class="btn btn-g btn-xs esc-stop" onclick="escZero(${n})">Durdur</button>
</div>`;}

document.getElementById('tanks').innerHTML=tankUnit(1)+tankUnit(2);
document.getElementById('escs').innerHTML=escUnit(1)+escUnit(2);

// Pusula tick'lerini olustur (N/E/S/W + aralar)
(function buildCompass(){
  const ring=document.getElementById('compassRing');
  const dirs=[['K',0,true],['30',30,false],['60',60,false],['D',90,true],['120',120,false],['150',150,false],['G',180,true],['210',210,false],['240',240,false],['B',270,true],['300',300,false],['330',330,false]];
  const R=78;
  dirs.forEach(([lbl,ang,card])=>{
    const t=document.createElement('div');
    t.className='compass-tick'+(card?' card':'');
    const rad=(ang-90)*Math.PI/180;
    const x=Math.cos(rad)*R, y=Math.sin(rad)*R;
    t.style.transform=`translate(${x}px,${y}px) translate(-50%,-50%)`;
    t.textContent=lbl;
    ring.appendChild(t);
  });
})();

// Tank islemleri
async function goto(n,p){const d=await api(`/api/tank/${n}/goto`,{percent:p});toast(d.msg,!d.ok);}
async function gotoCustom(n){const v=document.getElementById(`t${n}goto`).value;if(v==='')return;const d=await api(`/api/tank/${n}/goto`,{percent:parseFloat(v)});toast(d.msg,!d.ok);}
async function gotoBoth(p){const d=await api('/api/tanks/goto',{percent:p});toast(d.msg,!d.ok);}
async function gotoBothCustom(){const v=document.getElementById('bothPct').value;if(v==='')return;const d=await api('/api/tanks/goto',{percent:parseFloat(v)});toast(d.msg,!d.ok);}
async function zeroT(n){if(!confirm(`Tank ${n}: bu nokta BOŞ (0) olsun?`))return;const d=await api(`/api/tank/${n}/zero`,{});toast(d.msg,!d.ok);}
async function manualFull(n){const v=document.getElementById(`t${n}mf`).value;if(v==='')return;const d=await api(`/api/tank/${n}/manualfull`,{val:parseInt(v)});toast(d.msg,!d.ok);}
async function setFullHere(n){if(!confirm(`Tank ${n}: şu anki nokta TAM DOLU (%100) olsun?`))return;const d=await api(`/api/tank/${n}/setfull`,{});toast(d.msg,!d.ok);}
async function jog(n,s){await api(`/api/tank/${n}/jog`,{steps:s});}
async function setSpeed(n,v){document.getElementById(`t${n}spdv`).textContent=v+'/s';await api(`/api/tank/${n}/speed`,{sps:parseInt(v)});}

// ESC
function escLive(n,v){const e=document.getElementById(`e${n}v`);e.textContent=(v>0?'+':'')+v;e.className='v '+(v>0?'fwd':v<0?'rev':'zero');}
async function escSet(n,v){await api(`/api/esc/${n}/set`,{percent:parseInt(v)});}
async function escZero(n){document.getElementById(`e${n}sl`).value=0;escLive(n,0);await api(`/api/esc/${n}/set`,{percent:0});}
function ebLive(v){const e=document.getElementById('ebVal');e.textContent=(v>0?'+':'')+v;e.className='v '+(v>0?'fwd':v<0?'rev':'zero');}
async function ebSet(v){document.getElementById('e1sl').value=v;document.getElementById('e2sl').value=v;escLive(1,v);escLive(2,v);await api('/api/esc/both',{percent:parseInt(v)});}
async function ebZero(){document.getElementById('ebSl').value=0;ebLive(0);await ebSet(0);}
async function arm(){await api('/api/esc/arm',{});toast('ARM gönderildi');}
async function disarm(){await api('/api/esc/disarm',{});toast('DISARM gönderildi');}

async function estop(){await api('/api/estop',{});for(let n=1;n<=2;n++){document.getElementById(`e${n}sl`).value=0;escLive(n,0);}document.getElementById('ebSl').value=0;ebLive(0);toast('ACİL STOP',true);}
async function sendCmd(){const i=document.getElementById('cmdIn');if(!i.value)return;await api('/api/cmd',{cmd:i.value});i.value='';}

// Durum poll - GERCEK ANLIK (live_pos ile tank dolar)
async function poll(){
  try{
    const s=await api('/api/status');
    connected=s.connected;
    document.getElementById('cdot').className='d'+(connected?' on':'');
    document.getElementById('ctxt').textContent=connected?s.port:'bağlı değil';
    document.getElementById('connBtn').textContent=connected?'Kes':'Bağlan';
    [s.tank1,s.tank2].forEach((t,i)=>{
      const n=i+1;
      document.getElementById(`t${n}pos`).textContent=t.live_pos;
      document.getElementById(`t${n}full`).textContent=t.full>0?t.full:'—';
      const fill=document.getElementById(`t${n}fill`);
      const pctEl=document.getElementById(`t${n}pct`);
      const tgt=document.getElementById(`t${n}tgt`);
      const cs=document.getElementById(`t${n}cs`);
      const live=document.getElementById(`t${n}live`);
      if(t.percent===null){
        fill.style.width='0%';pctEl.textContent='—';pctEl.className='pct-big empty';
        tgt.style.display='none';cs.textContent='kalibre değil';cs.className='cal-state no';
      }else{
        fill.style.width=t.percent+'%';
        pctEl.textContent='%'+Math.round(t.percent);pctEl.className='pct-big';
        cs.textContent='✓ strok '+t.full+' adım';cs.className='cal-state ok';
        // hedef isareti
        if(t.target_percent!==null&&Math.abs(t.target_percent-t.percent)>0.5){
          tgt.style.display='block';tgt.style.left=t.target_percent+'%';
          live.style.display='inline-flex'; // hareket ediyor
        }else{
          tgt.style.display='none';live.style.display='none';
        }
      }
    });
    // IMU guncelle
    if(s.imu&&s.imu.ok){
      document.getElementById('imuOff').style.display='none';
      document.getElementById('imuOn').style.display='grid';
      document.getElementById('imuTag').textContent='aktif';
      const h=s.imu.heading;
      document.getElementById('hdgVal').textContent=Math.round(h);
      // Ibre donuyor (heading yonunu gosterir)
      document.getElementById('needle').style.transform=`translate(-50%,-100%) rotate(${h}deg)`;
      // Roll/Pitch bar (-90..+90 -> bar merkezinden)
      const r=s.imu.roll, p=s.imu.pitch;
      document.getElementById('rollVal').textContent=Math.round(r)+'°';
      document.getElementById('pitchVal').textContent=Math.round(p)+'°';
      setBar('rollBar',r); setBar('pitchBar',p);
      // Kalibrasyon (byte -> 4 deger)
      const c=s.imu.calib;
      setCal('calSys',(c>>6)&3); setCal('calGyro',(c>>4)&3);
      setCal('calAcc',(c>>2)&3); setCal('calMag',c&3);
    }else{
      document.getElementById('imuOff').style.display='block';
      document.getElementById('imuOn').style.display='none';
      document.getElementById('imuTag').textContent='bekleniyor';
    }
  }catch(e){}
}
function setBar(id,v){
  // v: -90..+90 derece -> bar (%). merkez=0
  const el=document.getElementById(id);
  const clamped=Math.max(-90,Math.min(90,v));
  const half=clamped/90*50; // -50..+50 %
  if(half>=0){el.style.left='50%';el.style.width=half+'%';}
  else{el.style.left=(50+half)+'%';el.style.width=(-half)+'%';}
}
function setCal(id,v){
  const el=document.getElementById(id);
  el.textContent=v;el.className='cv c'+v;
}
async function pollLog(){
  try{
    const d=await api('/api/log?since='+logSince);
    if(d.lines.length){
      const m=document.getElementById('mon');
      d.lines.forEach(l=>{const v=document.createElement('div');v.className=l.startsWith('TX:')?'tx':l.startsWith('RX:')?'rx':'sys';v.textContent=l;m.appendChild(v);
        if(l.includes('ARMED')&&!l.includes('DISARM')){const b=document.getElementById('armBadge');b.textContent='ARMED';b.className='arm-badge on';}
        if(l.includes('DISARMED')){const b=document.getElementById('armBadge');b.textContent='DISARMED';b.className='arm-badge off';}
      });
      logSince=d.total;m.scrollTop=m.scrollHeight;
    }
  }catch(e){}
}
refreshPorts();
setInterval(poll,200);      // hizli poll = gercek anlik dolum
setInterval(pollLog,350);
setInterval(refreshPorts,4000);
</script>
</body>
</html>
"""

def open_browser():
    time.sleep(1.2); webbrowser.open("http://127.0.0.1:5000")

if __name__ == "__main__":
    print("="*50)
    print(" SEA STARS Web Kontrol v2")
    print(" http://127.0.0.1:5000")
    print(" Kapatmak: Ctrl+C")
    print("="*50)
    threading.Thread(target=open_browser, daemon=True).start()
    app.run(host="127.0.0.1", port=5000, debug=False, threaded=True)
