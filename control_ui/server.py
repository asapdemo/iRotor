#!/usr/bin/env python3
"""Azimuth control UI for the servo board.

The browser talks to this server over HTTP (login, REST API, server-sent events).
This server talks to the board over its JSON API (UDP, port 6700 by default),
so the board itself needs no changes and the browser needs no UDP.

Run:   python server.py            (then open http://<this-pc>:8080)
Login: admin / admin on first start -- change it on the Settings page.

Only the Python standard library is used.
"""

import argparse
import hashlib
import hmac
import json
import os
import secrets
import socket
import threading
import time
import uuid
from http import HTTPStatus
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
DATA_FILE = BASE_DIR / "data.json"

SESSION_TTL_S = 12 * 3600
POLL_INTERVAL_S = 0.2
EVENT_INTERVAL_S = 0.25
PULSE_ABS_MAX = 3000          # firmware limit (PWM_SERVICE_SERVO_MAX)
PBKDF2_ROUNDS = 200_000

DEFAULT_DATA = {
    "board": {"host": "192.168.1.121", "port": 6700, "timeout_s": 0.3},
    "servo": 1,
    # pulse_min..pulse_max us of pulse width covers range_deg of real rotation
    # (measured on the mechanism: 801..2012 us = 263 deg).
    # invert = False: a larger pulse turns clockwise (azimuth grows).
    "mapping": {"pulse_min": 801, "pulse_max": 2012, "range_deg": 263.0, "invert": False},
    # Set by "Bind to azimuth": this pulse width points at this azimuth.
    "calibration": None,
    "presets": [],
    "auth": None,
}


# --------------------------------------------------------------------------- storage

def hash_password(password: str, salt: str) -> str:
    return hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), PBKDF2_ROUNDS).hex()


class Store:
    """data.json: settings, calibration, presets and the login. Written atomically."""

    def __init__(self, path: Path):
        self.path = path
        self.lock = threading.RLock()
        self.data = json.loads(json.dumps(DEFAULT_DATA))
        if path.exists():
            loaded = json.loads(path.read_text(encoding="utf-8"))
            for key, value in loaded.items():
                if isinstance(value, dict) and isinstance(self.data.get(key), dict):
                    self.data[key].update(value)
                else:
                    self.data[key] = value
        if not self.data.get("auth"):
            self.set_password("admin", "admin", is_default=True)
        self.save()

    def save(self):
        with self.lock:
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(self.data, indent=2, ensure_ascii=False), encoding="utf-8")
            os.replace(tmp, self.path)

    def set_password(self, username: str, password: str, is_default: bool = False):
        salt = secrets.token_hex(16)
        with self.lock:
            self.data["auth"] = {
                "username": username,
                "salt": salt,
                "hash": hash_password(password, salt),
                "default": is_default,
            }

    def check_login(self, username: str, password: str) -> bool:
        auth = self.data["auth"]
        good_user = hmac.compare_digest(username, auth["username"])
        good_pass = hmac.compare_digest(hash_password(password, auth["salt"]), auth["hash"])
        return good_user and good_pass


# --------------------------------------------------------------------------- geometry

class Geometry:
    """Linear map between servo pulse width (us) and azimuth (deg, 0 = north, clockwise)."""

    def __init__(self, mapping: dict, calibration):
        self.pmin = int(mapping["pulse_min"])
        self.pmax = int(mapping["pulse_max"])
        self.range = float(mapping["range_deg"])
        self.dir = -1 if mapping.get("invert") else 1
        self.deg_per_us = self.range / (self.pmax - self.pmin)
        self.cal = calibration

    @property
    def calibrated(self) -> bool:
        return self.cal is not None

    def azimuth(self, pulse):
        if not self.cal or not pulse:
            return None
        return (self.cal["azimuth"] + self.dir * (pulse - self.cal["pulse"]) * self.deg_per_us) % 360.0

    def sector(self):
        """Reachable arc drawn clockwise from `start` over `span` degrees."""
        if not self.cal:
            return None
        start = self.azimuth(self.pmin) if self.dir > 0 else self.azimuth(self.pmax)
        return {"start": start, "span": self.range}

    def pulse_for(self, azimuth: float) -> int:
        """Pulse width for an azimuth, or ValueError if it lies in the unreachable zone."""
        if not self.cal:
            raise ValueError("not calibrated: bind the current position to an azimuth first")
        offset = (self.dir * (azimuth - self.azimuth(self.pmin))) % 360.0
        if offset > 360.0 - 1e-6:
            offset = 0.0
        if offset > self.range + 1e-6:
            raise ValueError(f"azimuth {azimuth:.1f} is in the unreachable zone")
        return min(self.pmax, max(self.pmin, round(self.pmin + offset / self.deg_per_us)))

    def clamp(self, pulse: float) -> int:
        return min(self.pmax, max(self.pmin, round(pulse)))


def validate_mapping(mapping: dict) -> dict:
    pmin = int(mapping["pulse_min"])
    pmax = int(mapping["pulse_max"])
    range_deg = float(mapping["range_deg"])
    if not (1 <= pmin < pmax <= PULSE_ABS_MAX):
        raise ValueError(f"need 1 <= pulse_min < pulse_max <= {PULSE_ABS_MAX}")
    if not (0.0 < range_deg <= 360.0):
        raise ValueError("range_deg must be in (0, 360]")
    return {"pulse_min": pmin, "pulse_max": pmax, "range_deg": range_deg, "invert": bool(mapping.get("invert"))}


# --------------------------------------------------------------------------- board link

class Board:
    """Request/response over the board's UDP JSON API. Every datagram gets the /data JSON back."""

    def __init__(self, store: Store):
        self.store = store
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if hasattr(socket, "SIO_UDP_CONNRESET"):
            # Windows: an ICMP "port unreachable" would otherwise break every later recvfrom.
            self.sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        self.live = {"online": False, "error": "not polled yet", "pwm": {}, "updated": 0.0}

    def request(self, payload: dict) -> dict:
        board = self.store.data["board"]
        message = json.dumps(payload, separators=(",", ":")).encode()
        if len(message) >= 192:
            raise ValueError("request too long for the board (192 bytes)")
        with self.lock:
            self._drain()
            self.sock.settimeout(float(board["timeout_s"]))
            self.sock.sendto(message, (board["host"], int(board["port"])))
            data, _ = self.sock.recvfrom(2048)
        reply = json.loads(data.decode("utf-8", "replace"))
        self.live = {"online": True, "error": None, "pwm": reply.get("pwm", {}),
                     "uptime_s": reply.get("uptime_s"), "updated": time.time()}
        return reply

    def _drain(self):
        self.sock.setblocking(False)
        try:
            while True:
                self.sock.recvfrom(2048)
        except (BlockingIOError, OSError):
            pass
        finally:
            self.sock.setblocking(True)

    def poll_forever(self):
        while True:
            try:
                self.request({})
            except (OSError, ValueError) as error:
                self.live = {"online": False, "error": str(error) or type(error).__name__,
                             "pwm": self.live.get("pwm", {}), "updated": self.live.get("updated", 0.0)}
            time.sleep(POLL_INTERVAL_S)


# --------------------------------------------------------------------------- app logic

class App:
    def __init__(self, store: Store, board: Board):
        self.store = store
        self.board = board
        self.sessions = {}
        self.sessions_lock = threading.Lock()

    def geometry(self) -> Geometry:
        return Geometry(self.store.data["mapping"], self.store.data["calibration"])

    def key(self, suffix: str = "") -> str:
        return f"servo{int(self.store.data['servo'])}{suffix}"

    def state(self) -> dict:
        live = self.board.live
        geo = self.geometry()
        pwm = live.get("pwm", {})
        pulse = pwm.get(self.key("_pos"))
        target = pwm.get(self.key())
        return {
            "online": live["online"],
            "error": live["error"],
            "age_s": round(time.time() - live["updated"], 2) if live.get("updated") else None,
            "servo": int(self.store.data["servo"]),
            "pulse": pulse,
            "target_pulse": target,
            "speed": pwm.get(self.key("_speed")),
            "center_pulse": pwm.get(self.key("_center")),
            "moving": pulse is not None and target is not None and pulse != target,
            "calibrated": geo.calibrated,
            "azimuth": geo.azimuth(pulse),
            "target_azimuth": geo.azimuth(target),
            "sector": geo.sector(),
            "mapping": self.store.data["mapping"],
            "deg_per_us": geo.deg_per_us,
        }

    # -- motion

    def goto(self, azimuth: float) -> dict:
        pulse = self.geometry().pulse_for(float(azimuth) % 360.0)
        self.board.request({self.key(): pulse})
        return {"pulse": pulse}

    def jog(self, degrees: float) -> dict:
        geo = self.geometry()
        pwm = self.board.request({}).get("pwm", {})
        base = pwm.get(self.key()) or pwm.get(self.key("_center")) or (geo.pmin + geo.pmax) / 2
        pulse = geo.clamp(base + geo.dir * float(degrees) / geo.deg_per_us)
        self.board.request({self.key(): pulse})
        return {"pulse": pulse}

    def stop(self) -> dict:
        pwm = self.board.request({}).get("pwm", {})
        pulse = pwm.get(self.key("_pos"))
        if pulse:
            self.board.request({self.key(): pulse})
        return {"pulse": pulse}

    def set_speed(self, speed: int) -> dict:
        speed = int(speed)
        if not 0 <= speed <= 255:
            raise ValueError("speed must be 0..255")
        self.board.request({self.key("_speed"): speed})
        return {"speed": speed}

    def calibrate(self, azimuth: float) -> dict:
        geo = self.geometry()
        pwm = self.board.request({}).get("pwm", {})
        pulse = pwm.get(self.key("_pos"))
        if not pulse:
            raise ValueError("servo output is off, move it first")
        if pulse != pwm.get(self.key()):
            raise ValueError("servo is still moving, wait until it stops")
        if not geo.pmin <= pulse <= geo.pmax:
            raise ValueError(f"current pulse {pulse} us is outside {geo.pmin}..{geo.pmax} us")
        with self.store.lock:
            self.store.data["calibration"] = {"pulse": pulse, "azimuth": float(azimuth) % 360.0}
            self.store.save()
        return self.store.data["calibration"]

    # -- presets

    def add_preset(self, name: str, azimuth) -> dict:
        name = str(name).strip()[:40]
        if not name:
            raise ValueError("preset name is empty")
        if azimuth is None or azimuth == "":
            azimuth = self.state()["azimuth"]
            if azimuth is None:
                raise ValueError("current azimuth is unknown")
        azimuth = round(float(azimuth) % 360.0, 1)
        self.geometry().pulse_for(azimuth)  # reject unreachable presets now, not at use
        preset = {"id": uuid.uuid4().hex[:8], "name": name, "azimuth": azimuth}
        with self.store.lock:
            self.store.data["presets"].append(preset)
            self.store.save()
        return preset

    def delete_preset(self, preset_id: str):
        with self.store.lock:
            before = len(self.store.data["presets"])
            self.store.data["presets"] = [p for p in self.store.data["presets"] if p["id"] != preset_id]
            if len(self.store.data["presets"]) == before:
                raise ApiError(HTTPStatus.NOT_FOUND, "preset not found")
            self.store.save()

    def rename_preset(self, preset_id: str, name: str) -> dict:
        name = str(name).strip()[:40]
        if not name:
            raise ValueError("preset name is empty")
        with self.store.lock:
            for preset in self.store.data["presets"]:
                if preset["id"] == preset_id:
                    preset["name"] = name
                    self.store.save()
                    return preset
        raise ApiError(HTTPStatus.NOT_FOUND, "preset not found")

    # -- settings

    def config(self) -> dict:
        data = self.store.data
        return {"board": data["board"], "servo": data["servo"], "mapping": data["mapping"],
                "calibration": data["calibration"], "username": data["auth"]["username"]}

    def update_config(self, body: dict) -> dict:
        with self.store.lock:
            if "board" in body:
                board = body["board"]
                port = int(board["port"])
                timeout_s = float(board.get("timeout_s", 0.3))
                if not 1 <= port <= 65535 or not 0.05 <= timeout_s <= 5.0 or not str(board["host"]).strip():
                    raise ValueError("bad board host/port/timeout")
                self.store.data["board"] = {"host": str(board["host"]).strip(), "port": port, "timeout_s": timeout_s}
            if "servo" in body:
                if int(body["servo"]) not in (1, 2):
                    raise ValueError("servo must be 1 or 2")
                if int(body["servo"]) != int(self.store.data["servo"]):
                    self.store.data["calibration"] = None  # the old binding belongs to the other servo
                self.store.data["servo"] = int(body["servo"])
            if "mapping" in body:
                self.store.data["mapping"] = validate_mapping(body["mapping"])
            if body.get("clear_calibration"):
                self.store.data["calibration"] = None
            self.store.save()
        return self.config()

    # -- sessions

    def new_session(self, username: str) -> str:
        token = secrets.token_urlsafe(32)
        with self.sessions_lock:
            self.sessions[token] = {"user": username, "expires": time.time() + SESSION_TTL_S}
        return token

    def session(self, token):
        if not token:
            return None
        with self.sessions_lock:
            session = self.sessions.get(token)
            if session and session["expires"] > time.time():
                session["expires"] = time.time() + SESSION_TTL_S
                return session
            self.sessions.pop(token, None)
        return None

    def drop_session(self, token):
        with self.sessions_lock:
            self.sessions.pop(token, None)

    def drop_all_sessions(self):
        with self.sessions_lock:
            self.sessions.clear()


# --------------------------------------------------------------------------- HTTP

CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".ico": "image/x-icon"}


class ApiError(Exception):
    def __init__(self, status: HTTPStatus, message: str):
        super().__init__(message)
        self.status = status


def make_handler(app: App):
    class Handler(BaseHTTPRequestHandler):
        server_version = "AzimuthUI/1.0"

        def log_message(self, fmt, *args):
            if not self.path.startswith("/api/events") and not self.path.startswith("/api/state"):
                super().log_message(fmt, *args)

        # -- helpers

        def token(self):
            cookie = SimpleCookie(self.headers.get("Cookie", ""))
            return cookie["sid"].value if "sid" in cookie else None

        def send_json(self, status, body, headers=None):
            payload = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            for name, value in (headers or {}).items():
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(payload)

        def read_json(self) -> dict:
            if not self.headers.get("Content-Type", "").startswith("application/json"):
                # Blocks cross-site form posts; fetch() from our own page always sends JSON.
                raise ApiError(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "expected application/json")
            length = int(self.headers.get("Content-Length", "0"))
            if length > 64 * 1024:
                raise ApiError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "body too large")
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError:
                raise ApiError(HTTPStatus.BAD_REQUEST, "invalid JSON")
            if not isinstance(body, dict):
                raise ApiError(HTTPStatus.BAD_REQUEST, "expected a JSON object")
            return body

        def require_session(self):
            session = app.session(self.token())
            if not session:
                raise ApiError(HTTPStatus.UNAUTHORIZED, "login required")
            return session

        # -- dispatch

        def do_GET(self):
            self.dispatch("GET")

        def do_POST(self):
            self.dispatch("POST")

        def do_PUT(self):
            self.dispatch("PUT")

        def do_DELETE(self):
            self.dispatch("DELETE")

        def dispatch(self, method):
            path = self.path.split("?", 1)[0]
            try:
                if path.startswith("/api/"):
                    self.api(method, path)
                elif method == "GET":
                    self.static(path)
                else:
                    raise ApiError(HTTPStatus.METHOD_NOT_ALLOWED, "method not allowed")
            except ApiError as error:
                self.send_json(error.status, {"error": str(error)})
            except KeyError as error:
                self.send_json(HTTPStatus.BAD_REQUEST, {"error": f"missing field {error}"})
            except (ValueError, TypeError) as error:
                self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            except OSError as error:
                if isinstance(error, (BrokenPipeError, ConnectionResetError)):
                    return
                self.send_json(HTTPStatus.BAD_GATEWAY, {"error": f"board not responding ({error or type(error).__name__})"})

        def static(self, path):
            if path == "/":
                path = "/index.html"
            target = (STATIC_DIR / path.lstrip("/")).resolve()
            if STATIC_DIR not in target.parents or not target.is_file():
                raise ApiError(HTTPStatus.NOT_FOUND, "not found")
            payload = target.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", CONTENT_TYPES.get(target.suffix, "application/octet-stream"))
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-cache")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(payload)

        def api(self, method, path):
            route = (method, path)

            # -- public
            if route == ("POST", "/api/login"):
                body = self.read_json()
                if not app.store.check_login(str(body.get("username", "")), str(body.get("password", ""))):
                    time.sleep(0.5)
                    raise ApiError(HTTPStatus.UNAUTHORIZED, "wrong username or password")
                token = app.new_session(app.store.data["auth"]["username"])
                cookie = f"sid={token}; HttpOnly; SameSite=Strict; Path=/; Max-Age={SESSION_TTL_S}"
                return self.send_json(HTTPStatus.OK, {"user": app.store.data["auth"]["username"]},
                                      {"Set-Cookie": cookie})
            if route == ("POST", "/api/logout"):
                app.drop_session(self.token())
                return self.send_json(HTTPStatus.OK, {"ok": True},
                                      {"Set-Cookie": "sid=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0"})
            if route == ("GET", "/api/session"):
                session = app.session(self.token())
                if not session:
                    raise ApiError(HTTPStatus.UNAUTHORIZED, "login required")
                return self.send_json(HTTPStatus.OK, {"user": session["user"],
                                                      "default_password": app.store.data["auth"]["default"]})

            # -- everything below needs a session
            self.require_session()

            if route == ("GET", "/api/events"):
                return self.events()
            if route == ("GET", "/api/state"):
                return self.send_json(HTTPStatus.OK, app.state())
            if route == ("POST", "/api/goto"):
                return self.send_json(HTTPStatus.OK, app.goto(self.read_json()["azimuth"]))
            if route == ("POST", "/api/jog"):
                return self.send_json(HTTPStatus.OK, app.jog(self.read_json()["degrees"]))
            if route == ("POST", "/api/stop"):
                self.read_json()
                return self.send_json(HTTPStatus.OK, app.stop())
            if route == ("POST", "/api/speed"):
                return self.send_json(HTTPStatus.OK, app.set_speed(self.read_json()["speed"]))
            if route == ("POST", "/api/calibrate"):
                return self.send_json(HTTPStatus.OK, app.calibrate(self.read_json()["azimuth"]))
            if route == ("GET", "/api/presets"):
                return self.send_json(HTTPStatus.OK, app.store.data["presets"])
            if route == ("POST", "/api/presets"):
                body = self.read_json()
                return self.send_json(HTTPStatus.CREATED, app.add_preset(body.get("name", ""), body.get("azimuth")))
            if path.startswith("/api/presets/") and method in ("PUT", "DELETE"):
                preset_id = path.rsplit("/", 1)[1]
                if method == "DELETE":
                    app.delete_preset(preset_id)
                    return self.send_json(HTTPStatus.OK, {"ok": True})
                return self.send_json(HTTPStatus.OK, app.rename_preset(preset_id, self.read_json().get("name", "")))
            if route == ("GET", "/api/config"):
                return self.send_json(HTTPStatus.OK, app.config())
            if route == ("PUT", "/api/config"):
                return self.send_json(HTTPStatus.OK, app.update_config(self.read_json()))
            if route == ("POST", "/api/password"):
                body = self.read_json()
                if not app.store.check_login(app.store.data["auth"]["username"], str(body.get("current", ""))):
                    raise ApiError(HTTPStatus.FORBIDDEN, "current password is wrong")
                new_password = str(body.get("new", ""))
                username = str(body.get("username") or app.store.data["auth"]["username"]).strip()
                if len(new_password) < 6 or not username:
                    raise ApiError(HTTPStatus.BAD_REQUEST, "new password must have at least 6 characters")
                app.store.set_password(username, new_password)
                app.store.save()
                app.drop_all_sessions()
                return self.send_json(HTTPStatus.OK, {"ok": True})

            raise ApiError(HTTPStatus.NOT_FOUND, "no such API route")

        def events(self):
            """Server-sent events: the state every EVENT_INTERVAL_S until the browser goes away."""
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            token = self.token()
            try:
                while app.session(token):
                    self.wfile.write(f"data: {json.dumps(app.state())}\n\n".encode())
                    self.wfile.flush()
                    time.sleep(EVENT_INTERVAL_S)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Azimuth control UI for the servo board")
    parser.add_argument("--listen", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port (default 8080)")
    parser.add_argument("--board", help="board IP, saved to data.json (default from data.json)")
    args = parser.parse_args()

    store = Store(DATA_FILE)
    if args.board:
        store.data["board"]["host"] = args.board
        store.save()
    board = Board(store)
    threading.Thread(target=board.poll_forever, daemon=True).start()

    app = App(store, board)
    server = ThreadingHTTPServer((args.listen, args.port), make_handler(app))
    server.daemon_threads = True
    print(f"Azimuth UI on http://{args.listen}:{args.port}  board {store.data['board']['host']}:{store.data['board']['port']}")
    if store.data["auth"]["default"]:
        print("Login admin / admin -- change the password on the Settings page.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
