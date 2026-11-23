#!/usr/bin/env python3
"""Board emulator for working on web_ui/index.html without flashing.

Serves the page and emulates the firmware endpoints it uses, with the same rules:
  /data, /pwm?ch=&v=, /az/config, /az/set?, /az/cal?, /az/preset?, /board/data, /board/set?,
  /net_settings/data, /net_settings/set?, /reboot, and the firmware update
  (POST /fwupdate/start, then a RAM server that only takes POST /chunk and POST /finish).
Servo motion follows the firmware ramp (Q8 position, speed * 20 / 256 us per 20 ms, 255 = instant).

Run:  python web_ui/dev_server.py   ->  http://127.0.0.1:8090
      --packed serves Core/Src/web_ui_gz.c instead of index.html (checks the embedded artifact)
"""

import argparse
import gzip
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent.parent
SERVO_MAX = 3000
PRESETS = 8
NAME_BYTES = 31
ZERO_PULSE = 1346          # AZ_ZERO_PULSE in azimuth_service.h


class Servo:
    def __init__(self):
        self.target, self.q8, self.speed, self.center = 0, 0, 255, 1372
        self.known = 0          # pwm_service "known" position (position_service restores it at boot)

    def set_target(self, value):
        self.target = min(SERVO_MAX, value)
        if self.q8 == 0 and self.target and self.known and self.speed != 255:
            self.q8 = self.known << 8           # output on: ramp from where the servo stands
        elif self.speed == 255 or self.q8 == 0 or self.target == 0:
            self.q8 = self.target << 8
        if self.q8:
            self.known = self.q8 >> 8

    def set_speed(self, value):
        self.speed = value
        if value == 255:
            self.q8 = self.target << 8
            if self.q8:
                self.known = self.target

    def step(self, frames):
        if self.speed == 0:
            return
        target, step = self.target << 8, frames * self.speed * 20
        self.q8 = min(target, self.q8 + step) if self.q8 < target else max(target, self.q8 - step)
        if self.q8:
            self.known = self.q8 >> 8

    @property
    def pos(self):
        return self.q8 >> 8


class Board:
    def __init__(self):
        self.lock = threading.Lock()
        self.servos = [Servo(), Servo()]
        self.az = {"servo": 1, "pmin": 801, "pmax": 2012, "range": 2630, "inv": 0, "cal": None,
                   "presets": [None] * PRESETS}
        self.boot = {"servo1": 1372, "servo2": 1372, "servo1_speed": 255, "servo2_speed": 255}
        for index, servo in enumerate(self.servos, 1):
            servo.set_speed(self.boot[f"servo{index}_speed"])
            servo.set_target(self.boot[f"servo{index}"])
        self.net = {"dhcp": 0, "dhcp_fallback_attempts": 5, "ip": "192.168.67.121", "mask": "255.255.255.0",
                    "gateway": "192.168.67.1", "dns": "8.8.8.8", "mac": "02:12:34:56:78:9A",
                    "json_api": {"enabled": 1, "mode": 0, "port": 6700},
                    "web_auth": {"enabled": 0, "password": "admin"}}
        self.fw_mode = False          # True between /fwupdate/start and /finish
        self.fw_image = bytearray()
        self.started = time.monotonic()
        threading.Thread(target=self.tick, daemon=True).start()

    def tick(self):
        last = time.monotonic()
        while True:
            time.sleep(0.02)
            frames = int((time.monotonic() - last) / 0.02)
            if frames:
                last += frames * 0.02
                with self.lock:
                    for servo in self.servos:
                        servo.step(frames)

    def data(self):
        s1, s2 = self.servos
        servo = self.servos[self.az["servo"] - 1]
        from_pulse = lambda p: None if az_from_pulse(self.az, p) < 0 else az_from_pulse(self.az, p)
        return {"uptime_s": int(time.monotonic() - self.started), "pwm": {
            "servo1": s1.target, "servo2": s2.target, "servo1_speed": s1.speed, "servo2_speed": s2.speed,
            "servo1_pos": s1.pos, "servo2_pos": s2.pos, "servo1_center": s1.center, "servo2_center": s2.center},
            "az": {"pos": from_pulse(servo.pos), "target": from_pulse(servo.target)}}

    def goto(self, az10):
        """Mirror of Az_Goto: returns the error name or None."""
        result = az_pulse_for(self.az, az10)
        if isinstance(result, str):
            return result
        self.servos[self.az["servo"] - 1].set_target(result)
        return None

    def goto_zero(self):
        """Mirror of Az_GotoZero."""
        if not self.az["pmin"] <= ZERO_PULSE <= self.az["pmax"]:
            return "unreachable"
        self.servos[self.az["servo"] - 1].set_target(ZERO_PULSE)
        return None

    def config(self):
        az = self.az
        presets = [[slot, p[0], p[1]] for slot, p in enumerate(az["presets"]) if p]
        return {"fw": {"v": "1.0.0", "git": "v1.0.0", "build": "2026-11-19 10:00", "settings": 13},
                "servo": az["servo"], "pmin": az["pmin"], "pmax": az["pmax"], "range": az["range"],
                "inv": az["inv"], "zero": ZERO_PULSE, "cal": az["cal"], "presets": presets}


# ---- line-by-line mirror of Core/Src/azimuth_service.c (C integer division semantics)
def _c_div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def _div_round(n, d):
    return _c_div(n + _c_div(d, 2), d) if n >= 0 else -_c_div(-n + _c_div(d, 2), d)


def _wrap(a):
    a = a - _c_div(a, 3600) * 3600
    return a + 3600 if a < 0 else a


def az_from_pulse(az, pulse):
    """Az_FromPulse: azimuth x10 or -1."""
    span = az["pmax"] - az["pmin"]
    if not az["cal"] or pulse == 0 or span <= 0:
        return -1
    turned = _div_round((pulse - az["cal"][0]) * az["range"], span)
    return _wrap(az["cal"][1] + (-turned if az["inv"] else turned))


def az_pulse_for(az, az10):
    """Az_PulseFor: pulse, or an error name."""
    span = az["pmax"] - az["pmin"]
    if az10 >= 3600:
        return "bad_value"
    if not az["cal"] or span <= 0 or az["range"] == 0:
        return "not_calibrated"
    turn = _wrap(az10 - az["cal"][1])
    for _ in range(2):
        offset = _div_round(turn * span, az["range"])
        candidate = az["cal"][0] + (-offset if az["inv"] else offset)
        if az["pmin"] <= candidate <= az["pmax"]:
            return candidate
        turn -= 3600
    return "unreachable"


def uint(query, name, maximum):
    """Mirror of WebPages_ParseUint: digits only, 0..maximum, else ValueError."""
    raw = query.get(name, [None])[0]
    if raw is None or not raw.isdigit() or int(raw) > maximum:
        raise ValueError(name)
    return int(raw)


def preset_name(raw):
    """Mirror of WebPages_ParseText(is_name = 1)."""
    data = raw.encode("utf-8")
    if not data or len(data) > NAME_BYTES or any(b < 0x20 or b == 0x7F or b in b'"\\' for b in data):
        raise ValueError("name")
    return raw


def stm32_crc(data: bytes) -> int:
    """STM32 hardware CRC: CRC-32/MPEG-2 over little-endian words (same as the page and fw_update.c)."""
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        crc ^= int.from_bytes(data[i:i + 4].ljust(4, b"\xff"), "little")
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


IP_RE = re.compile(r"(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})")
MAC_RE = re.compile(r"[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}")


def make_handler(board: Board, page: bytes, packed: bool):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            if "/data" not in self.path:
                super().log_message(fmt, *args)

        def reply(self, status, body, ctype="application/json", extra=None):
            if isinstance(body, (dict, list)):
                body = (json.dumps(body, ensure_ascii=False) + "\n").encode()
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urlsplit(self.path)
            query = parse_qs(url.query, keep_blank_values=True)
            path = url.path
            if board.fw_mode:
                return self.reply(400, {"ok": 0})        # the RAM server knows nothing but /chunk and /finish
            try:
                with board.lock:
                    if path == "/net_settings/data":
                        return self.reply(200, board.net)
                    if path == "/reboot":
                        return self.reply(200, {"reboot": 1})
                    if path == "/":
                        return self.reply(200, page, "text/html; charset=utf-8",
                                          {"Content-Encoding": "gzip"} if packed else None)
                    if path == "/data":
                        return self.reply(200, board.data())
                    if path == "/az/config":
                        return self.reply(200, board.config())
                    if path == "/board/data":
                        return self.reply(200, {"pwm_defaults": dict(board.boot)})
                    if path == "/az/goto":
                        if "az" in query:
                            err = board.goto(uint(query, "az", 3599))
                        elif "zero" in query:
                            if uint(query, "zero", 1) != 1:
                                raise ValueError("zero")
                            err = board.goto_zero()
                        else:
                            preset = board.az["presets"][uint(query, "preset", PRESETS - 1)]
                            err = board.goto(preset[0]) if preset else "no_preset"
                        if err:
                            return self.reply(400, {"ok": 0, "err": err})
                        return self.reply(200, {"ok": 1, "pulse": board.servos[board.az["servo"] - 1].target})
                    if path == "/pwm":
                        self.pwm(query)
                    elif path == "/az/set":
                        self.az_set(query)
                    elif path == "/az/cal":
                        self.az_cal(query)
                    elif path == "/az/preset":
                        self.az_preset(query)
                    elif path == "/board/set":
                        self.board_set(query)
                    elif path == "/net_settings/set":
                        self.net_set(query)
                    else:
                        return self.reply(404, b"not found\n", "text/plain")
                return self.reply(200, {"ok": 1})
            except ValueError:
                return self.reply(400, {"ok": 0})

        def do_POST(self):
            url = urlsplit(self.path)
            query = parse_qs(url.query)
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            if url.path == "/fwupdate/start" and not board.fw_mode:
                board.fw_mode, board.fw_image = True, bytearray()
                return self.reply(200, {"ram_server": 1})
            if board.fw_mode and url.path == "/chunk":
                try:
                    offset, length = int(query["off"][0]), int(query["len"][0])
                    crc = int(query["crc"][0], 16)
                except (KeyError, ValueError):
                    return self.reply(400, {"ok": 0})
                if len(body) != length or stm32_crc(body) != crc or offset != len(board.fw_image):
                    return self.reply(400, {"ok": 0})
                board.fw_image += body
                return self.reply(200, {"ok": 1})
            if board.fw_mode and url.path == "/finish":
                print(f"firmware received: {len(board.fw_image)} bytes, 'rebooting'")
                board.fw_mode = False
                return self.reply(200, {"done": 1})
            return self.reply(400 if board.fw_mode else 404, {"ok": 0})

        def net_set(self, q):
            def text(name):
                return q.get(name, [""])[0]
            net = json.loads(json.dumps(board.net))
            net["dhcp"] = 0 if text("dhcp") == "0" else 1
            net["dhcp_fallback_attempts"] = uint(q, "dhcp_fallback_attempts", 255)
            for name in ("ip", "mask", "gateway", "dns"):
                match = IP_RE.fullmatch(text(name))
                if not match or any(int(part) > 255 for part in match.groups()):
                    raise ValueError(name)
                net[name] = text(name)
            if not MAC_RE.fullmatch(text("mac")) or int(text("mac")[:2], 16) & 1:
                raise ValueError("mac")
            net["mac"] = text("mac").upper().replace("-", ":")
            net["json_api"] = {"enabled": uint(q, "json_api_enabled", 1), "mode": uint(q, "json_api_mode", 1),
                               "port": uint(q, "json_api_port", 65535)}
            password = text("web_password")
            if not password or len(password) > 31 or any(not ("!" <= ch <= "~") or ch in "\"\\" for ch in password):
                raise ValueError("web_password")
            net["web_auth"] = {"enabled": uint(q, "web_auth_enabled", 1), "password": password}
            board.net = net

        def pwm(self, q):
            ch = q.get("ch", [""])[0]
            match = re.fullmatch(r"servo([12])(_speed)?", ch)
            if not match:
                raise ValueError("ch")
            servo = board.servos[int(match.group(1)) - 1]
            if match.group(2):
                servo.set_speed(uint(q, "v", 255))
                if "save" in q:
                    if uint(q, "save", 1) != 1:
                        raise ValueError("save")
                    board.boot[f"servo{match.group(1)}_speed"] = servo.speed   # WebPages_SaveServoSpeed
            else:
                servo.set_target(uint(q, "v", SERVO_MAX))

        def az_set(self, q):
            pmin, pmax = uint(q, "pmin", SERVO_MAX), uint(q, "pmax", SERVO_MAX)
            rng, inv, servo = uint(q, "range", 3600), uint(q, "inv", 1), uint(q, "servo", 2)
            if pmin == 0 or pmin >= pmax or rng == 0 or servo == 0:
                raise ValueError("mapping")
            if servo != board.az["servo"]:
                board.az["cal"] = None
            board.az.update(pmin=pmin, pmax=pmax, range=rng, inv=inv, servo=servo)

        def az_cal(self, q):
            if "clear" in q:
                board.az["cal"] = None
                return
            servo = board.servos[board.az["servo"] - 1]
            a10 = uint(q, "az", 3599)
            if servo.pos != servo.target or not board.az["pmin"] <= servo.pos <= board.az["pmax"]:
                raise ValueError("cal")
            board.az["cal"] = [servo.pos, a10]

        def az_preset(self, q):
            slot = uint(q, "slot", PRESETS - 1)
            board.az["presets"][slot] = None
            if "del" not in q:
                board.az["presets"][slot] = [uint(q, "az", 3599), preset_name(q.get("name", [""])[0])]

        def board_set(self, q):
            values = {"servo1": uint(q, "servo1", SERVO_MAX), "servo2": uint(q, "servo2", SERVO_MAX)}
            for name in ("servo1_speed", "servo2_speed"):
                if name in q:
                    values[name] = uint(q, name, 255)
            board.boot.update(values)
            for index, servo in enumerate(board.servos, 1):
                servo.set_speed(board.boot[f"servo{index}_speed"])
                servo.set_target(board.boot[f"servo{index}"])

    return Handler


def load_packed() -> bytes:
    text = (ROOT / "Core" / "Src" / "web_ui_gz.c").read_text(encoding="utf-8")
    body = text[text.index("{") + 1:text.rindex("}")]
    data = bytes(int(token, 16) for token in re.findall(r"0x([0-9A-Fa-f]{2})", body))
    declared = int(re.search(r"web_ui_gz_len = (\d+)U", text).group(1))
    assert len(data) == declared, (len(data), declared)
    gzip.decompress(data)  # must be a valid gzip stream
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--packed", action="store_true", help="serve the embedded gzip from web_ui_gz.c")
    args = parser.parse_args()
    page = load_packed() if args.packed else (ROOT / "web_ui" / "index.html").read_bytes()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(Board(), page, args.packed))
    server.daemon_threads = True
    print(f"web UI dev server on http://127.0.0.1:{args.port}  ({'packed' if args.packed else 'source'})")
    server.serve_forever()


if __name__ == "__main__":
    main()
