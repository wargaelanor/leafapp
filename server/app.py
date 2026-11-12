#!/usr/bin/env python3
"""
LeafControl API — единый сервер для платы, PWA и Android-приложения.

Заменяет MQTT/OVMS-сервер: плата сама выходит на сервер (HTTP), телефон
ходит только на сервер. Команды доставляются через очередь + опрос платой.

Зависимостей нет: только стандартная библиотека Python 3.11+ и SQLite.
"""

import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

DB_PATH = os.environ.get("LEAF_DB", "/data/leaf.db")
WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
ADMIN_USER = os.environ.get("LEAF_ADMIN_USER", "admin")
ADMIN_PASSWORD = os.environ.get("LEAF_ADMIN_PASSWORD", "")
POLL_S = int(os.environ.get("LEAF_POLL_S", "10"))
SESSION_DAYS = 30
ONLINE_WINDOW_S = 45

# Публичный адрес этого сервера (для ссылок на прошивки и APK).
SERVER_BASE = os.environ.get("LEAF_SERVER_BASE", "https://leaf.wargaelanor.keenetic.link")

# Ссылка на Android-приложение (release-ассет проекта).
APP_REV = os.environ.get("LEAF_REV", "sec-2c73f93")
APP_VERSION = "1.52"  # версия APK, лежащего на этом сервере
APP_NOTES = "Точка на графике плавно следует за пальцем"

APK_URL = os.environ.get(
    "LEAF_APK_URL",
    "https://github.com/wargaelanor/leafapp/releases/download/android-v1.52/LeafControl-1.52-debug.apk",
)

# Команды, которые шлёт PWA (те же пути, что у платы).
PWA_COMMANDS = {
    "/hvac_on": "clim_on",
    "/hvac_off": "clim_off",
    "/lock": "lock",
    "/unlock": "unlock",
    "/charge_on": "charge_start",
    "/charge_off": "charge_stop",
    "/refresh": "refresh",
    "/lights": "lights",
    "/hornlights": "horn",
    "/rhloff": "lights",
}

db_lock = threading.Lock()

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  username TEXT UNIQUE NOT NULL,
  pass_hash TEXT NOT NULL,
  role TEXT NOT NULL DEFAULT 'owner',
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
  token TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  login TEXT UNIQUE NOT NULL,
  name TEXT,
  serial TEXT,
  fw TEXT,
  owner_id INTEGER,
  device_token TEXT UNIQUE NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  created_at INTEGER NOT NULL,
  last_seen INTEGER DEFAULT 0,
  last_link TEXT,
  last_rssi INTEGER,
  bus_age_s INTEGER,
  state_json TEXT,
  state_ts INTEGER DEFAULT 0
);
CREATE TABLE IF NOT EXISTS samples (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER NOT NULL,
  ts INTEGER NOT NULL,
  payload TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS samples_dev_ts ON samples(device_id, ts);
CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER,
  kind TEXT NOT NULL,
  payload TEXT,
  ts INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS dev_sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER NOT NULL,
  kind TEXT NOT NULL,
  started_at INTEGER,
  ended_at INTEGER,
  duration_s INTEGER,
  distance_km REAL,
  energy_kwh REAL,
  soc_start REAL,
  soc_end REAL,
  meta TEXT
);
CREATE INDEX IF NOT EXISTS ds_dev ON dev_sessions(device_id, ended_at);
CREATE TABLE IF NOT EXISTS commands (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER NOT NULL,
  user_id INTEGER,
  name TEXT NOT NULL,
  args TEXT,
  status TEXT NOT NULL DEFAULT 'queued',
  created_at INTEGER NOT NULL,
  delivered_at INTEGER,
  acked_at INTEGER,
  result TEXT
);
CREATE INDEX IF NOT EXISTS cmd_dev ON commands(device_id, status);
CREATE TABLE IF NOT EXISTS schedules (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER NOT NULL,
  user_id INTEGER,
  kind TEXT NOT NULL,
  at_ts INTEGER NOT NULL,
  args TEXT,
  status TEXT NOT NULL DEFAULT 'waiting',
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS fcm_tokens (
  token TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL,
  platform TEXT,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS app_settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS maintenance (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id INTEGER NOT NULL,
  title TEXT NOT NULL,
  due_km INTEGER,
  due_date TEXT,
  note TEXT,
  done_at INTEGER,
  created_at INTEGER NOT NULL
);
"""


# ---------------------------------------------------------------- утилиты

def now() -> int:
    return int(time.time())


def connect() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=15)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=8000")
    return conn


def init_db() -> None:
    os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
    with db_lock, connect() as c:
        c.executescript(SCHEMA)
        cols = [r[1] for r in c.execute("PRAGMA table_info(devices)").fetchall()]
        if "config_json" not in cols:
            c.execute("ALTER TABLE devices ADD COLUMN config_json TEXT")
        if "config_ts" not in cols:
            c.execute("ALTER TABLE devices ADD COLUMN config_ts INTEGER DEFAULT 0")
        if "claim_code" not in cols:
            c.execute("ALTER TABLE devices ADD COLUMN claim_code TEXT")
        c.execute("""CREATE TABLE IF NOT EXISTS feedback (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER,
            device_id INTEGER,
            text TEXT NOT NULL,
            created_at INTEGER,
            status TEXT DEFAULT 'new'
        )""")
        # Код привязки должен быть у каждой платы всегда: по нему машину
        # можно перенести в другой аккаунт.
        for d in c.execute("SELECT id FROM devices WHERE claim_code IS NULL OR claim_code=''").fetchall():
            c.execute("UPDATE devices SET claim_code=? WHERE id=?", (make_claim_code(), d["id"]))
        row = c.execute("SELECT id FROM users WHERE username=?", (ADMIN_USER,)).fetchone()
        if not row and ADMIN_PASSWORD:
            c.execute(
                "INSERT INTO users(username, pass_hash, role, created_at) VALUES(?,?,?,?)",
                (ADMIN_USER, hash_password(ADMIN_PASSWORD), "admin", now()),
            )
            log_event(c, None, "admin_created", {"username": ADMIN_USER})
        elif row and ADMIN_PASSWORD:
            # Пароль администратора задаётся окружением и является истиной:
            # при старте контейнера он приводится к значению из LEAF_ADMIN_PASSWORD.
            c.execute(
                "UPDATE users SET pass_hash=?, role='admin' WHERE username=?",
                (hash_password(ADMIN_PASSWORD), ADMIN_USER),
            )



# --- защита от перебора пароля: 8 неудач за 5 минут на IP ------------------
LOGIN_FAILS: dict[str, list] = {}
LOGIN_LOCK = threading.Lock()
LOGIN_MAX_FAILS = 8
LOGIN_WINDOW_S = 300


def login_blocked(ip: str) -> bool:
    with LOGIN_LOCK:
        ts = [t for t in LOGIN_FAILS.get(ip, []) if now() - t < LOGIN_WINDOW_S]
        LOGIN_FAILS[ip] = ts
        return len(ts) >= LOGIN_MAX_FAILS


def login_failed(ip: str) -> None:
    with LOGIN_LOCK:
        LOGIN_FAILS.setdefault(ip, []).append(now())


def login_ok(ip: str) -> None:
    with LOGIN_LOCK:
        LOGIN_FAILS.pop(ip, None)


def hash_password(password: str, salt: bytes | None = None) -> str:
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 200_000)
    return f"pbkdf2$200000${salt.hex()}${digest.hex()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        _, iters, salt_hex, digest_hex = stored.split("$")
        digest = hashlib.pbkdf2_hmac(
            "sha256", password.encode(), bytes.fromhex(salt_hex), int(iters)
        )
        return hmac.compare_digest(digest.hex(), digest_hex)
    except Exception:
        return False


def make_claim_code() -> str:
    """Код привязки: 16 символов, группами по 4."""
    alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
    raw = "".join(secrets.choice(alphabet) for _ in range(16))
    return "-".join(raw[i:i + 4] for i in range(0, 16, 4))


def normalize_code(code: str) -> str:
    return "".join(ch for ch in (code or "").upper() if ch.isalnum())


def cleanup_unused_accounts(c) -> int:
    """Удаляет владельцев без машин, созданных больше 3 суток назад."""
    cutoff = now() - 3 * 86400
    rows = c.execute(
        "SELECT u.id FROM users u WHERE u.role='owner' AND u.created_at < ? "
        "AND NOT EXISTS (SELECT 1 FROM devices d WHERE d.owner_id=u.id)",
        (cutoff,),
    ).fetchall()
    for r in rows:
        c.execute("DELETE FROM sessions WHERE user_id=?", (r["id"],))
        c.execute("DELETE FROM fcm_tokens WHERE user_id=?", (r["id"],))
        c.execute("DELETE FROM users WHERE id=?", (r["id"],))
    if rows:
        log_event(c, None, "accounts_cleaned", {"count": len(rows)})
    return len(rows)


def log_event(c: sqlite3.Connection, device_id, kind: str, payload: dict) -> None:
    c.execute(
        "INSERT INTO events(device_id, kind, payload, ts) VALUES(?,?,?,?)",
        (device_id, kind, json.dumps(payload, ensure_ascii=False), now()),
    )


def row_to_device(r: sqlite3.Row) -> dict:
    return {
        "id": r["id"],
        "login": r["login"],
        "name": r["name"],
        "serial": r["serial"],
        "fw": r["fw"],
        "status": r["status"],
        "last_seen": r["last_seen"],
        "online": (now() - (r["last_seen"] or 0)) < ONLINE_WINDOW_S,
        "link": r["last_link"],
        "rssi": r["last_rssi"],
        "bus_age_s": r["bus_age_s"],
        "state": json.loads(r["state_json"]) if r["state_json"] else None,
        "state_ts": r["state_ts"],
        "poll_s": POLL_S,
        "claim_code": r["claim_code"],
    }


# ---------------------------------------------------------------- HTTP

class Api(BaseHTTPRequestHandler):
    server_version = "LeafControl/1.0"

    # ---------- infra

    def log_message(self, fmt, *args):
        print("%s %s" % (self.address_string(), fmt % args), flush=True)

    def _send(self, code: int, body: bytes, ctype: str, extra: dict | None = None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def json(self, code: int, obj, extra: dict | None = None):
        self._send(code, json.dumps(obj, ensure_ascii=False).encode(), "application/json; charset=utf-8", extra)

    def err(self, code: int, message: str):
        self.json(code, {"error": message})

    def body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            return {}
        if length > 4_000_000:
            raise ValueError("too large")
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    def cookies(self) -> dict:
        out = {}
        for part in (self.headers.get("Cookie") or "").split(";"):
            if "=" in part:
                k, v = part.split("=", 1)
                out[k.strip()] = v.strip()
        return out

    def is_secure_request(self) -> bool:
        """Запрос пришёл по HTTPS (напрямую или через прокси Keenetic).

        Keenetic терминирует TLS и переписывает Host на внутренний адрес, но
        оставляет заголовки X-Forwarded-*. Прямой доступ по LAN (без прокси)
        остаётся без Secure, иначе браузер не сохранит cookie на http.
        """
        forced = os.environ.get("LEAF_COOKIE_SECURE", "").strip().lower()
        if forced in ("1", "true", "yes"):
            return True
        if forced in ("0", "false", "no"):
            return False
        proto = (self.headers.get("X-Forwarded-Proto") or "").lower().strip()
        if proto:
            return proto == "https"
        if self.headers.get("X-Forwarded-For") or self.headers.get("X-Forwarded-Host"):
            return True
        host = (self.headers.get("Host") or "").lower().strip()
        return SERVER_BASE.startswith("https://") and bool(host) and host in SERVER_BASE.lower()

    def session_cookie(self, token: str) -> str:
        secure = "; Secure" if self.is_secure_request() else ""
        return f"leaf_session={token}; Path=/; HttpOnly; SameSite=Lax; Max-Age={SESSION_DAYS * 86400}{secure}"

    # ---------- auth helpers

    def user(self):
        token = self.cookies().get("leaf_session")
        if not token:
            return None
        with db_lock, connect() as c:
            r = c.execute(
                "SELECT u.id,u.username,u.role,s.expires_at FROM sessions s "
                "JOIN users u ON u.id=s.user_id WHERE s.token=?",
                (token,),
            ).fetchone()
            if not r:
                return None
            if r["expires_at"] < now():
                c.execute("DELETE FROM sessions WHERE token=?", (token,))
                return None
            return {"id": r["id"], "username": r["username"], "role": r["role"]}

    def device(self):
        auth = self.headers.get("Authorization") or ""
        if not auth.startswith("Bearer "):
            return None
        token = auth[7:].strip()
        with db_lock, connect() as c:
            return c.execute("SELECT * FROM devices WHERE device_token=?", (token,)).fetchone()

    def require_user(self):
        u = self.user()
        if not u:
            self.err(401, "auth required")
        return u

    def owned_device(self, u, device_ref: str):
        with db_lock, connect() as c:
            if device_ref.isdigit():
                r = c.execute("SELECT * FROM devices WHERE id=?", (int(device_ref),)).fetchone()
            else:
                r = c.execute("SELECT * FROM devices WHERE login=?", (device_ref,)).fetchone()
        if not r:
            return None
        if u["role"] != "admin" and r["owner_id"] != u["id"]:
            return None
        return r

    # ---------- routing

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_DELETE(self):
        self.route("DELETE")

    def do_PUT(self):
        self.route("PUT")

    def route(self, method: str):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"
        q = {k: v[-1] for k, v in parse_qs(parsed.query).items()}
        try:
            if path in ("/", "/index.html"):
                if self.user():
                    return self.serve_static("app.html")
                return self.serve_static("login.html")
            if path == "/login":
                return self.serve_static("login.html")
            if path == "/admin":
                u = self.user()
                if not u:
                    return self._send(302, b"", "text/plain", {"Location": "/login"})
                if u["role"] != "admin":
                    return self.err(403, "admin only")
                return self.serve_static("admin.html")
            if path == "/settings":
                return self._send(302, b"", "text/plain", {"Location": "/"})
            if path == "/app.apk":
                # APK живёт на этом сервере (кладётся вместе с релизом);
                # редирект на GitHub остаётся запасным вариантом.
                if os.path.isfile(os.path.join(WEB_DIR, "app.apk")):
                    return self.serve_static("app.apk")
                return self._send(302, b"", "text/plain", {"Location": APK_URL})
            if path == "/ui":
                return self.json(200, {"ok": True})
            if path.startswith("/static/"):
                return self.serve_static(path[len("/static/"):])
            if path == "/api/fw/latest" and method == "GET":
                # Последняя прошивка для платы. Имена: FR-2can.N / FR-TCall.N,
                # старые t2can-N / tcall-N тоже принимаем.
                board = (q.get("board") or "CMD").upper()
                patterns = (r"^fr-tcall\.(\d+)\.bin$", r"^tcall-(\d+)\.bin$") if board == "TCALL" \
                    else (r"^fr-2can\.(\d+)\.bin$", r"^t2can-(\d+)\.bin$")
                label = "FR-TCall" if board == "TCALL" else "FR-2can"
                fw_dir = os.path.join(WEB_DIR, "fw")
                best = None
                if os.path.isdir(fw_dir):
                    for name in os.listdir(fw_dir):
                        low = name.lower()
                        for idx, pat in enumerate(patterns):
                            m = re.fullmatch(pat, low)
                            if not m:
                                continue
                            # новые имена приоритетнее старых при равном номере
                            key = (int(m.group(1)), -idx)
                            if best is None or key > best[0]:
                                best = (key, name)
                            break
                cors = {"Access-Control-Allow-Origin": "*"}
                if best is None:
                    return self.json(200, {"ok": False, "board": board}, cors)
                name = best[1]
                full = os.path.join(fw_dir, name)
                ver = str(best[0][0])
                return self.json(200, {
                    "ok": True,
                    "board": board,
                    "version": ver,
                    "name": label + "." + ver,
                    "file": name,
                    "url": SERVER_BASE + "/fw/" + name,
                    "size": os.path.getsize(full),
                }, cors)

            if path == "/api/app/latest" and method == "GET":
                local_apk = os.path.join(WEB_DIR, "app.apk")
                url = SERVER_BASE + "/app.apk" if os.path.isfile(local_apk) else APK_URL
                return self.json(200, {
                    "version": APP_VERSION,
                    "url": url,
                    "notes": APP_NOTES,
                })

            if path == "/api/health":
                return self.json(200, {"ok": True, "ts": now(), "rev": APP_REV,
                                       "app": APP_VERSION,
                                       "host": (self.headers.get("Host") or "")[:64],
                                       "proto": (self.headers.get("X-Forwarded-Proto") or "")[:16],
                                       "fwd_host": (self.headers.get("X-Forwarded-Host") or "")[:64],
                                       "xff": (self.headers.get("X-Forwarded-For") or "")[:48]})
            if path.startswith("/api/"):
                return self.api(method, path, q)
            if method == "POST" and path in PWA_COMMANDS:
                return self.pwa_command(path, q)
            # Статика: /manifest.webmanifest, /sw.js, /icon-*.png, картинки PWA
            return self.serve_static(path.lstrip("/"))
        except Exception as exc:  # noqa: BLE001
            print("ERROR", path, repr(exc), flush=True)
            self.err(500, "internal error")

    def api(self, method: str, path: str, q: dict):
        # ---------- auth
        if path == "/api/auth/register" and method == "POST":
            return self.register()
        if path == "/api/feedback" and method == "POST":
            return self.post_feedback()
        if path == "/api/claim" and method == "POST":
            return self.claim_device()
        if path == "/api/auth/login" and method == "POST":
            return self.login()
        if path == "/api/auth/logout" and method == "POST":
            return self.logout()
        if path == "/api/auth/me" and method == "GET":
            u = self.require_user()
            if not u:
                return
            return self.json(200, {"user": u})
        if path == "/api/auth/password" and method == "POST":
            return self.change_password()

        # ---------- board endpoints (no user session)
        if path == "/api/device/enroll" and method == "POST":
            return self.device_enroll()
        if path == "/api/device/ping" and method == "POST":
            return self.device_ping()
        if path == "/api/device/hist" and method == "POST":
            return self.device_hist()
        if path == "/api/device/sync" and method == "POST":
            return self.device_sync()
        if path == "/api/device/state" and method == "POST":
            return self.device_state()
        if path == "/api/device/heartbeat" and method == "POST":
            return self.device_heartbeat()
        if path == "/api/device/session" and method == "POST":
            return self.device_session()
        if path == "/api/device/cmd" and method == "GET":
            return self.device_cmd_poll(q)
        if path == "/api/device/ack" and method == "POST":
            return self.device_ack()

        # ---------- user endpoints
        if path == "/api/stats" and method == "GET":
            return self.get_stats(q)
        if path == "/api/samples" and method == "GET":
            return self.get_samples(q)
        if path == "/api/settings" and method == "GET":
            return self.get_settings()
        if path == "/api/settings" and method == "POST":
            return self.post_settings()
        if path == "/api/maintenance" and method == "GET":
            return self.maintenance_list(q)
        if path == "/api/maintenance" and method == "POST":
            return self.maintenance_add()
        if path == "/api/maintenance/done" and method == "POST":
            return self.maintenance_done()
        if path == "/api/config" and method == "GET":
            return self.get_config(q)
        if path == "/api/config" and method == "POST":
            return self.post_config()
        if path == "/api/device/config" and method == "POST":
            return self.device_config()
        if path == "/api/status" and method == "GET":
            return self.pwa_status(q)
        if path in ("/api/trips", "/api/charges") and method == "GET":
            return self.pwa_history(path, q)
        if path in ("/api/trips/clear", "/api/charges/clear") and method == "POST":
            return self.json(200, {"ok": True})
        if path == "/api/tariff":
            with db_lock, connect() as c:
                if method == "POST":
                    b = self.body()
                    pairs = {"tariff_day": b.get("day"), "tariff_night": b.get("night"),
                             "night_from": b.get("from"), "night_to": b.get("to")}
                    for k, v in pairs.items():
                        if v is not None:
                            c.execute("INSERT OR REPLACE INTO app_settings(key,value) VALUES(?,?)",
                                      (k, str(v)))
                cfg = self.settings_all(c)
            return self.json(200, {
                "day": float(cfg["tariff_day"]), "night": float(cfg["tariff_night"]),
                "from": int(float(cfg["night_from"])), "to": int(float(cfg["night_to"])),
                "petrol_price": float(cfg["petrol_price"]),
                "petrol_l100": float(cfg["petrol_l100"]),
            })
        if path == "/api/device/select" and method == "POST":
            u = self.require_user()
            if not u:
                return
            ref = str(self.body().get("device", "")).strip()
            d = self.owned_device(u, ref) if ref else None
            if not d:
                return self.err(404, "device not found")
            cookie = f"leaf_dev={d['id']}; Path=/; SameSite=Lax; Max-Age={365 * 86400}"
            if self.is_secure_request():
                cookie += "; Secure"
            return self.json(200, {"ok": True, "device": d["id"], "login": d["login"],
                                   "name": d["name"]}, {"Set-Cookie": cookie})

        if path == "/api/devices" and method == "GET":
            return self.list_devices()
        if path == "/api/state" and method == "GET":
            return self.get_state(q)
        if path == "/api/hist" and method == "GET":
            return self.get_hist(q)
        if path == "/api/sessions" and method == "GET":
            return self.get_sessions(q)
        if path == "/api/session" and method == "GET":
            return self.get_session(q)
        if path == "/api/cmd" and method == "POST":
            return self.post_cmd()
        if path.startswith("/api/devices/") and method == "PUT":
            return self.rename_device(path)
        if path == "/api/sched" and method == "GET":
            return self.get_sched(q)
        if path == "/api/sched" and method == "POST":
            return self.post_sched()
        if path == "/api/sched/cancel" and method == "POST":
            return self.cancel_sched()
        if path == "/api/notify/token" and method == "POST":
            return self.register_fcm()

        # ---------- admin endpoints
        if path.startswith("/api/admin/"):
            u = self.require_user()
            if not u:
                return
            if u["role"] != "admin":
                return self.err(403, "admin only")
            return self.admin(method, path, q)

        return self.err(404, "unknown api")

    # ---------- static

    def serve_static(self, name: str):
        name = name.replace("\\", "/").lstrip("/")
        full = os.path.normpath(os.path.join(WEB_DIR, name))
        if not full.startswith(os.path.normpath(WEB_DIR)) or not os.path.isfile(full):
            return self.err(404, "not found")
        ctype = {
            ".html": "text/html; charset=utf-8",
            ".js": "application/javascript; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".png": "image/png",
            ".svg": "image/svg+xml",
            ".ico": "image/x-icon",
            ".json": "application/json",
            ".webmanifest": "application/manifest+json",
        }.get(os.path.splitext(full)[1].lower(), "application/octet-stream")
        with open(full, "rb") as fh:
            data = fh.read()
        extra = {"Access-Control-Allow-Origin": "*"} if name.startswith("fw/") else None
        self._send(200, data, ctype, extra)

    # ---------- auth

    def register(self):
        """Свободная регистрация владельца: свой логин и пароль, машины добавляются кодом."""
        data = self.body()
        username = (data.get("username") or "").strip().lower()
        password = data.get("password") or ""
        if not re.fullmatch(r"[a-z0-9_.-]{3,32}", username):
            return self.err(400, "логин: 3–32 символа, латиница, цифры, . _ -")
        if len(password) < 8:
            return self.err(400, "пароль короче 8 символов")
        with db_lock, connect() as c:
            if c.execute("SELECT 1 FROM users WHERE username=?", (username,)).fetchone():
                return self.err(409, "такой логин уже занят")
            cur = c.execute(
                "INSERT INTO users(username, pass_hash, role, created_at) VALUES(?,?,?,?)",
                (username, hash_password(password), "owner", now()),
            )
            uid = cur.lastrowid
            token = secrets.token_urlsafe(32)
            c.execute("INSERT INTO sessions(token,user_id,created_at,expires_at) VALUES(?,?,?,?)",
                      (token, uid, now(), now() + SESSION_DAYS * 86400))
            log_event(c, None, "account_registered", {"username": username})
        cookie = self.session_cookie(token)
        self.json(200, {"ok": True, "user": {"id": uid, "username": username, "role": "owner"}},
                  {"Set-Cookie": cookie})

    def post_feedback(self):
        """Сообщение от владельца: что не работает, что улучшить."""
        u = self.require_user()
        if not u:
            return
        text = (self.body().get("text") or "").strip()
        if len(text) < 5:
            return self.err(400, "слишком коротко")
        if len(text) > 2000:
            text = text[:2000]
        with db_lock, connect() as c:
            c.execute("INSERT INTO feedback(user_id,text,created_at,status) VALUES(?,?,?,'new')",
                      (u["id"], text, now()))
            log_event(c, None, "feedback_new", {"username": u["username"]})
        return self.json(200, {"ok": True})

    def claim_device(self):
        """Привязка платы к аккаунту по коду с её экрана."""
        u = self.require_user()
        if not u:
            return
        code = normalize_code(self.body().get("code"))
        if len(code) < 16:
            return self.err(400, "код слишком короткий")
        with db_lock, connect() as c:
            # Код с экрана платы — это ключ к машине. Он действует всегда, поэтому
            # плату можно перенести в другой аккаунт, даже если она уже привязана.
            rows = c.execute("SELECT * FROM devices WHERE claim_code IS NOT NULL").fetchall()
            match = None
            for r in rows:
                if normalize_code(r["claim_code"]) == code:
                    match = r
                    break
            if not match:
                return self.err(404, "код не найден — проверьте его на экране платы")
            prev_owner = match["owner_id"]
            if prev_owner == u["id"]:
                return self.json(200, {"ok": True, "device_id": match["id"],
                                       "login": match["login"], "already": True})
            c.execute(
                "UPDATE devices SET owner_id=?, status='active', claim_code=? WHERE id=?",
                (u["id"], match["claim_code"], match["id"]),
            )
            log_event(c, match["id"], "device_claimed",
                      {"by": u["username"], "from_user": prev_owner})
        self.json(200, {"ok": True, "device_id": match["id"], "login": match["login"]})

    def login(self):
        ip = (self.client_address[0] if self.client_address else "?")
        if login_blocked(ip):
            return self.err(429, "слишком много попыток входа, подождите 5 минут")
        data = self.body()
        username = (data.get("username") or "").strip()
        password = data.get("password") or ""
        with db_lock, connect() as c:
            r = c.execute("SELECT * FROM users WHERE username=?", (username,)).fetchone()
            if not r or not verify_password(password, r["pass_hash"]):
                log_event(c, None, "login_failed", {"username": username})
                login_failed(ip)
                return self.err(401, "неверный логин или пароль")
            token = secrets.token_urlsafe(32)
            c.execute(
                "INSERT INTO sessions(token,user_id,created_at,expires_at) VALUES(?,?,?,?)",
                (token, r["id"], now(), now() + SESSION_DAYS * 86400),
            )
            log_event(c, None, "login_ok", {"username": username})
        login_ok(ip)
        cookie = self.session_cookie(token)
        self.json(200, {"ok": True, "user": {"id": r["id"], "username": r["username"], "role": r["role"]}},
                  {"Set-Cookie": cookie})

    def logout(self):
        token = self.cookies().get("leaf_session")
        if token:
            with db_lock, connect() as c:
                c.execute("DELETE FROM sessions WHERE token=?", (token,))
        self.json(200, {"ok": True}, {"Set-Cookie": "leaf_session=; Path=/; Max-Age=0"})

    def change_password(self):
        u = self.require_user()
        if not u:
            return
        data = self.body()
        old = data.get("old") or ""
        new = data.get("new") or ""
        if len(new) < 8:
            return self.err(400, "пароль короче 8 символов")
        with db_lock, connect() as c:
            r = c.execute("SELECT pass_hash FROM users WHERE id=?", (u["id"],)).fetchone()
            if not verify_password(old, r["pass_hash"]):
                return self.err(403, "старый пароль неверен")
            c.execute("UPDATE users SET pass_hash=? WHERE id=?", (hash_password(new), u["id"]))
            log_event(c, None, "password_changed", {"username": u["username"]})
        self.json(200, {"ok": True})

    # ---------- board

    def device_enroll(self):
        """Регистрация платы. Возвращает токен устройства и код привязки."""
        data = self.body()
        serial = (data.get("serial") or "").strip()
        login = (data.get("login") or "").strip().lower()
        fw = data.get("fw") or ""
        name = data.get("name") or "Leaf"
        with db_lock, connect() as c:
            row = None
            if serial:
                row = c.execute("SELECT * FROM devices WHERE serial=?", (serial,)).fetchone()
            if not row and login:
                row = c.execute("SELECT * FROM devices WHERE login=?", (login,)).fetchone()
            if row:
                c.execute("UPDATE devices SET fw=?, last_seen=? WHERE id=?", (fw, now(), row["id"]))
                return self.json(200, {
                    "device_token": row["device_token"],
                    "status": row["status"],
                    "device_id": row["id"],
                    "claim_code": row["claim_code"] or "",
                })
            base = login or ("dev-" + re.sub(r"[^a-z0-9]", "", (serial or secrets.token_hex(4)).lower()))
            unique = base
            n = 0
            while c.execute("SELECT 1 FROM devices WHERE login=?", (unique,)).fetchone():
                n += 1
                unique = "%s%d" % (base, n)
            token = secrets.token_hex(24)
            code = make_claim_code()
            cur = c.execute(
                "INSERT INTO devices(login,name,serial,fw,owner_id,device_token,status,created_at,last_seen,claim_code)"
                " VALUES(?,?,?,?,NULL,?,?,?,?,?)",
                (unique, name, serial or None, fw, token, "unclaimed", now(), now(), code),
            )
            log_event(c, cur.lastrowid, "device_enrolled", {"serial": serial, "code": code})
        self.json(200, {"device_token": token, "status": "unclaimed",
                        "device_id": cur.lastrowid, "claim_code": code})

    def device_heartbeat(self):
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        data = self.body()
        with db_lock, connect() as c:
            c.execute(
                "UPDATE devices SET last_seen=?, last_link=?, last_rssi=?, bus_age_s=? WHERE id=?",
                (now(), data.get("link"), data.get("rssi"), data.get("bus_age_s"), dev["id"]),
            )
        self.json(200, {"ok": True, "poll_s": POLL_S})

    def device_ping(self):
        """Лёгкий признак жизни: версия прошивки и канал связи, без телеметрии."""
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        d = self.body()
        fw = (d.get("fw") or "").strip()[:64]
        with db_lock, connect() as c:
            c.execute(
                "UPDATE devices SET last_seen=?, last_link=COALESCE(?, last_link),"
                " last_rssi=COALESCE(?, last_rssi), fw=COALESCE(?, fw)"
                " WHERE id=?",
                (now(), d.get("link"), d.get("rssi"), fw or None, dev["id"]),
            )
        return self.json(200, {"ok": True, "ts": int(now())})

    def device_hist(self):
        """Пачка точек, накопленных платой, пока не было связи."""
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        d = self.body()
        pts = d.get("points") or []
        if not isinstance(pts, list):
            return self.err(400, "points must be a list")
        added = 0
        with db_lock, connect() as c:
            row = c.execute("SELECT MAX(ts) AS m FROM samples WHERE device_id=?",
                            (dev["id"],)).fetchone()
            last_ts = row["m"] or 0
            for p in pts[:500]:
                try:
                    ts = int(p[0])
                    soc = float(p[1])
                    kw = float(p[2])
                    batt = float(p[3])
                    spd = float(p[4])
                except (TypeError, ValueError, IndexError):
                    continue
                if ts <= last_ts:
                    continue
                payload = json.dumps({"soc": soc, "pack_kw": kw, "batt": batt,
                                      "spd": spd, "hist": True}, ensure_ascii=False)
                c.execute("INSERT INTO samples(device_id, ts, payload) VALUES(?,?,?)",
                          (dev["id"], ts, payload))
                last_ts = ts
                added += 1
        return self.json(200, {"ok": True, "added": added})

    def device_sync(self):
        """Один запрос на цикл: состояние + подтверждения + очередь команд."""
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        data = self.body()
        ts = int(data.pop("ts", now()))
        acks = data.pop("ack", []) or []
        nacks = data.pop("nack", []) or []
        link = data.pop("link", None)
        rssi = data.pop("rssi", None)
        bus_age = data.pop("bus_age_s", None)
        fw = data.pop("fw", None)

        with db_lock, connect() as c:
            c.execute(
                "UPDATE devices SET state_json=?, state_ts=?, last_seen=?, last_link=?, last_rssi=?,"
                " bus_age_s=?, fw=COALESCE(?, fw) WHERE id=?",
                (json.dumps(data, ensure_ascii=False), ts, now(), link, rssi,
                 bus_age, fw, dev["id"]),
            )
            last = c.execute(
                "SELECT ts FROM samples WHERE device_id=? ORDER BY ts DESC LIMIT 1", (dev["id"],)
            ).fetchone()
            if not last or ts - last["ts"] >= 5:
                c.execute("INSERT INTO samples(device_id, ts, payload) VALUES(?,?,?)",
                          (dev["id"], ts, json.dumps(data, ensure_ascii=False)))
            for aid in acks[:8]:
                try:
                    c.execute("UPDATE commands SET status='done', acked_at=? WHERE id=? AND device_id=?",
                              (now(), int(aid), dev["id"]))
                except (TypeError, ValueError):
                    pass
            for aid in nacks[:8]:
                try:
                    c.execute("UPDATE commands SET status='failed', acked_at=? WHERE id=? AND device_id=?",
                              (now(), int(aid), dev["id"]))
                except (TypeError, ValueError):
                    pass
            row = c.execute(
                "SELECT * FROM commands WHERE device_id=? AND status='queued' ORDER BY id LIMIT 1",
                (dev["id"],),
            ).fetchone()
            cmd = None
            if row:
                c.execute("UPDATE commands SET status='delivered', delivered_at=? WHERE id=?",
                          (now(), row["id"]))
                cmd = {"id": row["id"], "name": row["name"],
                       "args": json.loads(row["args"]) if row["args"] else {}}
            want_cfg = (now() - (dev["config_ts"] or 0)) > 300
            online = True
        self.json(200, {"ok": True, "cmd": cmd, "poll_s": 5, "config": want_cfg,
                        "online": online, "claim_code": dev["claim_code"] or ""})

    def device_state(self):
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        data = self.body()
        ts = int(data.pop("ts", now()))
        with db_lock, connect() as c:
            c.execute(
                "UPDATE devices SET state_json=?, state_ts=?, last_seen=?, bus_age_s=? WHERE id=?",
                (json.dumps(data, ensure_ascii=False), ts, now(), data.get("bus_age_s"), dev["id"]),
            )
            c.execute(
                "INSERT INTO samples(device_id, ts, payload) VALUES(?,?,?)",
                (dev["id"], ts, json.dumps(data, ensure_ascii=False)),
            )
        self.json(200, {"ok": True})

    def device_session(self):
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        d = self.body()
        kind = d.get("kind")
        if kind not in ("trip", "charge", "climate"):
            return self.err(400, "kind must be trip|charge|climate")
        with db_lock, connect() as c:
            # Повторная отправка после офлайна: та же запись не должна
            # задваиваться в истории.
            dup = c.execute(
                "SELECT id FROM dev_sessions WHERE device_id=? AND kind=? AND started_at=? LIMIT 1",
                (dev["id"], kind, d.get("started_at")),
            ).fetchone()
            if dup:
                return self.json(200, {"ok": True, "session_id": dup["id"], "duplicate": True})
            cur = c.execute(
                "INSERT INTO dev_sessions(device_id,kind,started_at,ended_at,duration_s,distance_km,"
                "energy_kwh,soc_start,soc_end,meta) VALUES(?,?,?,?,?,?,?,?,?,?)",
                (dev["id"], kind, d.get("started_at"), d.get("ended_at"), d.get("duration_s"),
                 d.get("distance_km"), d.get("energy_kwh"), d.get("soc_start"), d.get("soc_end"),
                 json.dumps(d, ensure_ascii=False)),
            )
            sid = cur.lastrowid
            log_event(c, dev["id"], f"session_{kind}", {"id": sid, "kwh": d.get("energy_kwh")})
        self.json(200, {"ok": True, "session_id": sid})

    def device_cmd_poll(self, q: dict):
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        wait = min(int(q.get("wait", "0") or 0), 25)
        deadline = time.time() + wait
        while True:
            with db_lock, connect() as c:
                c.execute("UPDATE devices SET last_seen=? WHERE id=?", (now(), dev["id"]))
                r = c.execute(
                    "SELECT * FROM commands WHERE device_id=? AND status='queued' ORDER BY id LIMIT 1",
                    (dev["id"],),
                ).fetchone()
                if r:
                    c.execute("UPDATE commands SET status='delivered', delivered_at=? WHERE id=?",
                              (now(), r["id"]))
                    return self.json(200, {"id": r["id"], "name": r["name"],
                                           "args": json.loads(r["args"]) if r["args"] else {}})
            if time.time() >= deadline:
                return self.json(200, {"id": None})
            time.sleep(1)

    def device_ack(self):
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        d = self.body()
        with db_lock, connect() as c:
            c.execute("UPDATE commands SET status=?, acked_at=?, result=? WHERE id=? AND device_id=?",
                      ("done" if d.get("ok", True) else "failed", now(),
                       json.dumps(d.get("result"), ensure_ascii=False), d.get("id"), dev["id"]))
        self.json(200, {"ok": True})

    # ---------- user data

    def pwa_pick_device(self, u):
        """Устройство для PWA: выбранное в cookie, иначе на связи, иначе свежее."""
        ref = self.cookies().get("leaf_dev")
        if ref:
            d = self.owned_device(u, ref)
            if d:
                return d
        with db_lock, connect() as c:
            if u["role"] == "admin":
                rows = c.execute("SELECT * FROM devices ORDER BY last_seen DESC").fetchall()
            else:
                rows = c.execute("SELECT * FROM devices WHERE owner_id=? ORDER BY last_seen DESC",
                                 (u["id"],)).fetchall()
        if not rows:
            return None
        for r in rows:
            if r["status"] == "active" and (now() - (r["last_seen"] or 0)) < ONLINE_WINDOW_S:
                return r
        for r in rows:
            if r["status"] == "active":
                return r
        return rows[0]

    def pwa_status(self, q):
        u = self.require_user()
        if not u:
            return
        d = self.pwa_pick_device(u)
        if not d:
            return self.err(404, "no devices")
        st = json.loads(d["state_json"]) if d["state_json"] else {}
        soc = st.get("soc", -1)
        charging = bool(st.get("charging"))
        online_now = (now() - (d["last_seen"] or 0)) < ONLINE_WINDOW_S
        payload = {
            "online": online_now,
            "age": int(now() - (d["state_ts"] or d["last_seen"] or now())),
            "fw": d["fw"] or "",
            "fw_desc": "",
            "soc": soc,
            "isoc": soc,
            "irange": -1,
            "cabin": st.get("cabin", -99),
            "amb": st.get("amb", -99),
            "btemp": st.get("batt", -99),
            "cdelta": -1,
            "charging": charging,
            "charger": "charging" if charging else "",
            "hvac": bool(st.get("hvac")),
            "hvac_timer": 0,
            "setpoint": 22,
            "fan": 0,
            "heat": False,
            "cool": False,
            "vent": 0,
            "lock": 1 if st.get("locked") else 0,
            "locking": True,
            "lights": 0,
            "v": st.get("pack_v", -1),
            "i": 0,
            "power": st.get("pack_kw", 0),
            "pack": True,
            "vvalid": True,
            "soh": -1,
            "gids": -1,
            "odo": st.get("odo", 0),
            "vin": "",
            "on": 0,
            "seq": False,
            "fage": d["bus_age_s"] if d["bus_age_s"] is not None else -1,
            "epoch": d["state_ts"] or 0,
            "tariff_day": None,
            "tariff_night": None,
            "night_from": None,
            "night_to": None,
            "gps": None,
            "device": d["login"],
            "device_id": d["id"],
            "device_name": d["name"],
        }
        if st.get("v12") is not None or st.get("doors") is not None:
            payload["uds"] = {
                "gear": "",
                "v12": st.get("v12"),
                "i12": None,
                "plug": bool(charging),
                "mode": "",
                "spd": 0,
            }
        self.json(200, payload)

    def pwa_history(self, path, q):
        u = self.require_user()
        if not u:
            return
        d = self.pwa_pick_device(u)
        if not d:
            return self.json(200, [])
        kind = "trip" if path.endswith("trips") else "charge"
        with db_lock, connect() as c:
            rows = c.execute(
                "SELECT * FROM dev_sessions WHERE device_id=? AND kind=? "
                "ORDER BY COALESCE(ended_at,started_at) DESC LIMIT 200",
                (d["id"], kind),
            ).fetchall()
        out = []
        for r in rows:
            wh = (r["energy_kwh"] or 0) * 1000.0
            out.append({
                "s": r["started_at"] or 0,
                "e": r["ended_at"] or 0,
                "d": r["duration_s"] or 0,
                "k": r["distance_km"] or 0,
                "w": wh,
                "wd": wh,
                "wn": 0,
                "s0": r["soc_start"] if r["soc_start"] is not None else -1,
                "s1": r["soc_end"] if r["soc_end"] is not None else -1,
                "r": "server",
            })
        self.json(200, out)

    def pwa_command(self, path, q):
        u = self.require_user()
        if not u:
            return
        d = self.pwa_pick_device(u)
        if not d:
            return self.err(404, "no devices")
        if d["status"] != "active" and u["role"] != "admin":
            return self.err(403, "device not approved")
        name = PWA_COMMANDS[path]
        args = {}
        if path == "/hvac_on":
            try:
                args["minutes"] = int(q.get("minutes", 20))
            except ValueError:
                args["minutes"] = 20
        with db_lock, connect() as c:
            cur = c.execute(
                "INSERT INTO commands(device_id,user_id,name,args,status,created_at) VALUES(?,?,?,?,?,?)",
                (d["id"], u["id"], name, json.dumps(args, ensure_ascii=False), "queued", now()),
            )
            cid = cur.lastrowid
            log_event(c, d["id"], "cmd_queued", {"cmd": name, "id": cid, "by": u["username"]})
            online = (now() - (d["last_seen"] or 0)) < ONLINE_WINDOW_S
        return self.json(200, {"ok": True, "id": cid, "pushed": online, "poll_s": POLL_S})

    # ---------- настройки, статистика, ТО

    SETTING_DEFAULTS = {
        "tariff_day": "5.50",
        "tariff_night": "2.80",
        "night_from": "0",
        "night_to": "7",
        "petrol_price": "62.00",
        "petrol_l100": "7.0",
        "currency": "₽",
    }

    def settings_all(self, c):
        rows = c.execute("SELECT key, value FROM app_settings").fetchall()
        out = dict(self.SETTING_DEFAULTS)
        for r in rows:
            out[r["key"]] = r["value"]
        return out

    def get_settings(self):
        u = self.require_user()
        if not u:
            return
        with db_lock, connect() as c:
            self.json(200, self.settings_all(c))

    def post_settings(self):
        u = self.require_user()
        if not u:
            return
        data = self.body()
        allowed = set(self.SETTING_DEFAULTS)
        with db_lock, connect() as c:
            for k, v in data.items():
                if k in allowed:
                    c.execute("INSERT OR REPLACE INTO app_settings(key,value) VALUES(?,?)",
                              (k, str(v)))
            out = self.settings_all(c)
        self.json(200, out)

    def get_stats(self, q):
        u = self.require_user()
        if not u:
            return
        d = self.owned_device(u, q.get("device", "0"))
        if not d:
            d = self.pwa_pick_device(u)
        if not d:
            return self.err(404, "device not found")
        days = int(q.get("days", "0"))
        since = now() - days * 86400 if days > 0 else 0
        with db_lock, connect() as c:
            trips = c.execute(
                "SELECT COUNT(*) n, COALESCE(SUM(distance_km),0) km, COALESCE(SUM(energy_kwh),0) kwh "
                "FROM dev_sessions WHERE device_id=? AND kind='trip' AND COALESCE(ended_at,0)>=?",
                (d["id"], since),
            ).fetchone()
            charges = c.execute(
                "SELECT COUNT(*) n, COALESCE(SUM(energy_kwh),0) kwh "
                "FROM dev_sessions WHERE device_id=? AND kind='charge' AND COALESCE(ended_at,0)>=?",
                (d["id"], since),
            ).fetchone()
            climate = c.execute(
                "SELECT COUNT(*) n, COALESCE(SUM(energy_kwh),0) kwh, "
                "COALESCE(SUM(duration_s),0) secs "
                "FROM dev_sessions WHERE device_id=? AND kind='climate' AND COALESCE(ended_at,0)>=?",
                (d["id"], since),
            ).fetchone()
            per_day = c.execute(
                "SELECT date(COALESCE(ended_at,started_at),'unixepoch','localtime') d, "
                "COALESCE(SUM(distance_km),0) km, COALESCE(SUM(energy_kwh),0) kwh "
                "FROM dev_sessions WHERE device_id=? AND kind='trip' AND COALESCE(ended_at,0)>=? "
                "GROUP BY d ORDER BY d",
                (d["id"], since),
            ).fetchall()
            soh_rows = c.execute(
                "SELECT ts, payload FROM samples WHERE device_id=? AND ts>=? "
                "AND payload LIKE '%\"soh\"%' ORDER BY ts",
                (d["id"], since),
            ).fetchall()
            cfg = self.settings_all(c)
            maint = c.execute(
                "SELECT * FROM maintenance WHERE device_id=? AND done_at IS NULL ORDER BY due_km",
                (d["id"],),
            ).fetchall()

        km = float(trips["km"] or 0)
        kwh = float(trips["kwh"] or 0)
        ch_kwh = float(charges["kwh"] or 0)
        day_price = float(cfg.get("tariff_day", 0) or 0)
        night_price = float(cfg.get("tariff_night", 0) or 0)
        petrol_price = float(cfg.get("petrol_price", 0) or 0)
        petrol_l100 = float(cfg.get("petrol_l100", 0) or 0)

        electric_cost = kwh * day_price
        petrol_cost = (km / 100.0) * petrol_l100 * petrol_price
        consumption = (km / kwh) if kwh > 0 else 0

        soh = []
        for r in soh_rows:
            try:
                p = json.loads(r["payload"])
            except Exception:
                continue
            v = p.get("soh")
            if isinstance(v, (int, float)) and v > 0:
                soh.append({"ts": r["ts"], "soh": round(float(v), 2)})

        self.json(200, {
            "device": d["id"],
            "days": days,
            "trips": {"count": trips["n"], "km": round(km, 1), "kwh": round(kwh, 2)},
            "charges": {"count": charges["n"], "kwh": round(ch_kwh, 2)},
            "climate": {"count": climate["n"], "kwh": round(float(climate["kwh"] or 0), 2),
                        "hours": round(float(climate["secs"] or 0) / 3600.0, 2)},
            "per_day": [{"date": r["d"], "km": round(float(r["km"] or 0), 2),
                         "kwh": round(float(r["kwh"] or 0), 2)} for r in per_day],
            "consumption_km_per_kwh": round(consumption, 2),
            "cost": {
                "electric": round(electric_cost, 2),
                "petrol": round(petrol_cost, 2),
                "saved": round(petrol_cost - electric_cost, 2),
                "currency": cfg.get("currency", "₽"),
            },
            "soh": soh,
            "maintenance": [dict(r) for r in maint],
            "settings": cfg,
        })

    def get_samples(self, q):
        u = self.require_user()
        if not u:
            return
        d = self.owned_device(u, q.get("device", "0"))
        if not d:
            d = self.pwa_pick_device(u)
        if not d:
            return self.err(404, "device not found")
        try:
            ts_from = int(q.get("from", 0))
            ts_to = int(q.get("to", 0))
        except ValueError:
            return self.err(400, "bad range")
        if ts_to <= ts_from:
            return self.json(200, {"points": []})
        with db_lock, connect() as c:
            rows = c.execute(
                "SELECT ts, payload FROM samples WHERE device_id=? AND ts BETWEEN ? AND ? ORDER BY ts",
                (d["id"], ts_from, ts_to),
            ).fetchall()
        pts = []
        for r in rows:
            try:
                p = json.loads(r["payload"])
            except Exception:
                continue
            pts.append({
                "ts": r["ts"],
                "kw": p.get("pack_kw"),
                "soc": p.get("soc"),
                "batt": p.get("batt"),
                "spd": p.get("spd"),
            })
        self.json(200, {"points": pts})

    def maintenance_list(self, q):
        u = self.require_user()
        if not u:
            return
        d = self.owned_device(u, q.get("device", "0"))
        if not d:
            d = self.pwa_pick_device(u)
        if not d:
            return self.err(404, "device not found")
        with db_lock, connect() as c:
            rows = c.execute("SELECT * FROM maintenance WHERE device_id=? ORDER BY done_at, due_km",
                             (d["id"],)).fetchall()
        self.json(200, {"items": [dict(r) for r in rows]})

    def maintenance_add(self):
        u = self.require_user()
        if not u:
            return
        body = self.body()
        d = self.owned_device(u, str(body.get("device", "0")))
        if not d:
            d = self.pwa_pick_device(u)
        if not d:
            return self.err(404, "device not found")
        title = (body.get("title") or "").strip()
        if not title:
            return self.err(400, "title required")
        with db_lock, connect() as c:
            cur = c.execute(
                "INSERT INTO maintenance(device_id,title,due_km,due_date,note,done_at,created_at)"
                " VALUES(?,?,?,?,?,NULL,?)",
                (d["id"], title, body.get("due_km"), body.get("due_date"), body.get("note"), now()),
            )
            self.json(200, {"ok": True, "id": cur.lastrowid})

    def maintenance_done(self):
        u = self.require_user()
        if not u:
            return
        body = self.body()
        mid = int(body.get("id", 0))
        with db_lock, connect() as c:
            row = c.execute("SELECT device_id FROM maintenance WHERE id=?", (mid,)).fetchone()
            if not row:
                return self.err(404, "not found")
            dev = c.execute("SELECT owner_id FROM devices WHERE id=?", (row["device_id"],)).fetchone()
            if u["role"] != "admin" and (not dev or dev["owner_id"] != u["id"]):
                return self.err(403, "forbidden")
            c.execute("UPDATE maintenance SET done_at=? WHERE id=?", (now(), mid))
        self.json(200, {"ok": True})

    # ---------- настройки платы

    def get_config(self, q):
        u = self.require_user()
        if not u:
            return
        d = self.owned_device(u, q.get("device", "0")) or self.pwa_pick_device(u)
        if not d:
            return self.err(404, "device not found")
        cfg = json.loads(d["config_json"]) if d["config_json"] else {}
        self.json(200, {"config": cfg, "ts": d["config_ts"] or 0, "login": d["login"],
                        "fw": d["fw"], "serial": d["serial"]})

    def device_config(self):
        dev = self.device()
        if not dev:
            return self.err(401, "bad device token")
        data = self.body()
        with db_lock, connect() as c:
            c.execute("UPDATE devices SET config_json=?, config_ts=? WHERE id=?",
                      (json.dumps(data, ensure_ascii=False), now(), dev["id"]))
        self.json(200, {"ok": True})

    def post_config(self):
        u = self.require_user()
        if not u:
            return
        body = self.body()
        d = self.owned_device(u, str(body.get("device", "0"))) or self.pwa_pick_device(u)
        if not d:
            return self.err(404, "device not found")
        key = body.get("key")
        if not key:
            return self.err(400, "key required")
        value = body.get("value")
        with db_lock, connect() as c:
            cur = c.execute(
                "INSERT INTO commands(device_id,user_id,name,args,status,created_at) VALUES(?,?,?,?,?,?)",
                (d["id"], u["id"], "set_config",
                 json.dumps({"key": key, "value": value}, ensure_ascii=False), "queued", now()),
            )
            cid = cur.lastrowid
            log_event(c, d["id"], "config_set", {"key": key, "value": value, "by": u["username"]})
            online = (now() - (d["last_seen"] or 0)) < ONLINE_WINDOW_S
        self.json(200, {"ok": True, "id": cid, "pushed": online, "poll_s": POLL_S})

    def admin_reset_password(self, path: str):
        """Админ задаёт новый пароль владельцу (например, если плата его не показала)."""
        try:
            uid = int(path.split("/")[4])
        except (ValueError, IndexError):
            return self.err(400, "bad user id")
        new = (self.body().get("password") or "").strip()
        if len(new) < 8:
            return self.err(400, "пароль короче 8 символов")
        with db_lock, connect() as c:
            row = c.execute("SELECT username FROM users WHERE id=?", (uid,)).fetchone()
            if not row:
                return self.err(404, "user not found")
            c.execute("UPDATE users SET pass_hash=? WHERE id=?", (hash_password(new), uid))
            log_event(c, None, "admin_password_reset", {"username": row["username"]})
        self.json(200, {"ok": True, "username": row["username"]})

    def list_devices(self):
        u = self.require_user()
        if not u:
            return
        with db_lock, connect() as c:
            if u["role"] == "admin":
                rows = c.execute("SELECT * FROM devices ORDER BY id").fetchall()
            else:
                rows = c.execute("SELECT * FROM devices WHERE owner_id=? ORDER BY id", (u["id"],)).fetchall()
        self.json(200, {"devices": [row_to_device(r) for r in rows], "role": u["role"]})

    def get_state(self, q: dict):
        u = self.require_user()
        if not u:
            return
        d = self.owned_device(u, q.get("device", "0"))
        if not d:
            d = self.pwa_pick_device(u)
        if not d:
            return self.err(404, "device not found")
        self.json(200, row_to_device(d))

    def rename_device(self, path: str):
        u = self.require_user()
        if not u:
            return
        try:
            dev_id = int(path.rsplit("/", 1)[-1])
        except ValueError:
            return self.err(400, "bad id")
        name = (self.body().get("name") or "").strip()
        if not name:
            return self.err(400, "name required")
        if len(name) > 40:
            return self.err(400, "name too long")
        with db_lock, connect() as c:
            row = c.execute("SELECT owner_id FROM devices WHERE id=?", (dev_id,)).fetchone()
            if not row:
                return self.err(404, "device not found")
            if u["role"] != "admin" and row["owner_id"] != u["id"]:
                return self.err(403, "forbidden")
            c.execute("UPDATE devices SET name=? WHERE id=?", (name, dev_id))
            log_event(c, dev_id, "device_renamed", {"name": name, "by": u["username"]})
        self.json(200, {"ok": True, "name": name})

    def post_cmd(self):
        u = self.require_user()
        if not u:
            return
        d = self.body()
        dev = self.owned_device(u, str(d.get("device", "0")))
        if not dev:
            return self.err(404, "device not found")
        if dev["status"] != "active" and u["role"] != "admin":
            return self.err(403, "устройство не одобрено")
        name = d.get("cmd")
        if not name:
            return self.err(400, "cmd required")
        args = d.get("args") or {}
        with db_lock, connect() as c:
            cur = c.execute(
                "INSERT INTO commands(device_id,user_id,name,args,status,created_at) VALUES(?,?,?,?,?,?)",
                (dev["id"], u["id"], name, json.dumps(args, ensure_ascii=False), "queued", now()),
            )
            cid = cur.lastrowid
            log_event(c, dev["id"], "cmd_queued", {"cmd": name, "id": cid, "by": u["username"]})
            online = (now() - (dev["last_seen"] or 0)) < ONLINE_WINDOW_S
        self.json(200, {"ok": True, "id": cid, "pushed": online, "poll_s": POLL_S})

    def get_hist(self, q: dict):
        u = self.require_user()
        if not u:
            return
        dev = self.owned_device(u, q.get("device", "0"))
        if not dev:
            return self.err(404, "device not found")
        hours = min(int(q.get("hours", "24")), 24 * 30)
        since = now() - hours * 3600
        limit = min(int(q.get("limit", "2000")), 5000)
        with db_lock, connect() as c:
            rows = c.execute(
                "SELECT ts, payload FROM samples WHERE device_id=? AND ts>=? ORDER BY ts DESC LIMIT ?",
                (dev["id"], since, limit),
            ).fetchall()
        out = []
        for r in rows:
            p = json.loads(r["payload"])
            p["ts"] = r["ts"]
            out.append(p)
        out.reverse()
        self.json(200, {"points": out})

    def get_sessions(self, q: dict):
        u = self.require_user()
        if not u:
            return
        dev = self.owned_device(u, q.get("device", "0"))
        if not dev:
            return self.err(404, "device not found")
        days = min(int(q.get("days", "30")), 365)
        since = now() - days * 86400
        with db_lock, connect() as c:
            rows = c.execute(
                "SELECT * FROM dev_sessions WHERE device_id=? AND COALESCE(ended_at,started_at)>=?"
                " ORDER BY COALESCE(ended_at,started_at) DESC LIMIT 500",
                (dev["id"], since),
            ).fetchall()
        self.json(200, {"sessions": [dict(r) for r in rows]})

    def get_session(self, q: dict):
        u = self.require_user()
        if not u:
            return
        with db_lock, connect() as c:
            r = c.execute("SELECT * FROM dev_sessions WHERE id=?", (int(q.get("id", "0")),)).fetchone()
            if not r:
                return self.err(404, "session not found")
            d = c.execute("SELECT owner_id,status FROM devices WHERE id=?", (r["device_id"],)).fetchone()
        if u["role"] != "admin" and (not d or d["owner_id"] != u["id"]):
            return self.err(403, "forbidden")
        self.json(200, dict(r))

    # ---------- schedules

    def get_sched(self, q: dict):
        u = self.require_user()
        if not u:
            return
        dev = self.owned_device(u, q.get("device", "0"))
        if not dev:
            return self.err(404, "device not found")
        with db_lock, connect() as c:
            rows = c.execute(
                "SELECT * FROM schedules WHERE device_id=? ORDER BY at_ts DESC LIMIT 50", (dev["id"],)
            ).fetchall()
        self.json(200, {"schedules": [dict(r) for r in rows]})

    def post_sched(self):
        u = self.require_user()
        if not u:
            return
        d = self.body()
        dev = self.owned_device(u, str(d.get("device", "0")))
        if not dev:
            return self.err(404, "device not found")
        at_ts = int(d.get("at", 0))
        if at_ts < now():
            return self.err(400, "время в прошлом")
        minutes = int(d.get("minutes", 20))
        with db_lock, connect() as c:
            cur = c.execute(
                "INSERT INTO schedules(device_id,user_id,kind,at_ts,args,status,created_at)"
                " VALUES(?,?,?,?,?,?,?)",
                (dev["id"], u["id"], "clim_on", at_ts,
                 json.dumps({"minutes": minutes}, ensure_ascii=False), "waiting", now()),
            )
            sid = cur.lastrowid
        self.json(200, {"ok": True, "id": sid})

    def cancel_sched(self):
        u = self.require_user()
        if not u:
            return
        d = self.body()
        with db_lock, connect() as c:
            r = c.execute("SELECT * FROM schedules WHERE id=?", (int(d.get("id", 0)),)).fetchone()
            if not r:
                return self.err(404, "not found")
            dev = c.execute("SELECT owner_id FROM devices WHERE id=?", (r["device_id"],)).fetchone()
            if u["role"] != "admin" and (not dev or dev["owner_id"] != u["id"]):
                return self.err(403, "forbidden")
            c.execute("UPDATE schedules SET status='cancelled' WHERE id=?", (r["id"],))
        self.json(200, {"ok": True})

    # ---------- notifications

    def register_fcm(self):
        u = self.require_user()
        if not u:
            return
        d = self.body()
        token = (d.get("token") or "").strip()
        if not token:
            return self.err(400, "token required")
        with db_lock, connect() as c:
            c.execute(
                "INSERT OR REPLACE INTO fcm_tokens(token,user_id,platform,created_at) VALUES(?,?,?,?)",
                (token, u["id"], d.get("platform") or "android", now()),
            )
        self.json(200, {"ok": True})

    # ---------- admin

    def admin(self, method: str, path: str, q: dict):
        if path == "/api/admin/devices" and method == "GET":
            status = q.get("status", "pending")
            with db_lock, connect() as c:
                if status == "all":
                    rows = c.execute("SELECT * FROM devices ORDER BY id DESC").fetchall()
                else:
                    rows = c.execute("SELECT * FROM devices WHERE status=? ORDER BY id DESC",
                                     (status,)).fetchall()
                users = {r["id"]: r["username"] for r in c.execute("SELECT id,username FROM users").fetchall()}
            out = []
            for r in rows:
                d = row_to_device(r)
                d["owner"] = users.get(r["owner_id"])
                d["last_seen"] = r["last_seen"]
                out.append(d)
            return self.json(200, {"devices": out})

        if path.startswith("/api/admin/devices/") and method == "POST":
            parts = path.split("/")
            dev_id = int(parts[4])
            action = parts[5] if len(parts) > 5 else ""
            if action == "unbind":
                with db_lock, connect() as c:
                    row = c.execute("SELECT login FROM devices WHERE id=?", (did,)).fetchone()
                    if not row:
                        return self.err(404, "device not found")
                    c.execute(
                        "UPDATE devices SET owner_id=NULL, status='unclaimed', claim_code=? WHERE id=?",
                        (make_claim_code(), did),
                    )
                    log_event(c, did, "admin_unbound_device", {"login": row["login"]})
                return self.json(200, {"ok": True})

            if action == "assign":
                username = (self.body().get("username") or "").strip().lower()
                with db_lock, connect() as c:
                    urow = c.execute("SELECT id FROM users WHERE username=?", (username,)).fetchone()
                    if not urow:
                        return self.err(404, "user not found")
                    if not c.execute("SELECT 1 FROM devices WHERE id=?", (dev_id,)).fetchone():
                        return self.err(404, "device not found")
                    c.execute("UPDATE devices SET owner_id=?, status='active', claim_code=NULL WHERE id=?",
                              (urow["id"], dev_id))
                    log_event(c, dev_id, "admin_assigned_device", {"username": username})
                return self.json(200, {"ok": True, "owner": username})
            if action not in ("approve", "reject"):
                return self.err(400, "unknown action")
            new_status = "active" if action == "approve" else "rejected"
            with db_lock, connect() as c:
                c.execute("UPDATE devices SET status=? WHERE id=?", (new_status, dev_id))
                log_event(c, dev_id, f"device_{action}", {})
                r = c.execute("SELECT login FROM devices WHERE id=?", (dev_id,)).fetchone()
            return self.json(200, {"ok": True, "status": new_status, "login": r["login"] if r else None})

        if path.startswith("/api/admin/users/") and method == "POST" and path.endswith("/password"):
            return self.admin_reset_password(path)

        if path == "/api/admin/users" and method == "POST":
            data = self.body()
            username = (data.get("username") or "").strip().lower()
            password = data.get("password") or ""
            if not re.fullmatch(r"[a-z0-9_.-]{3,32}", username):
                return self.err(400, "логин: 3-32 символа, латиница, цифры, . _ -")
            if len(password) < 8:
                return self.err(400, "пароль короче 8 символов")
            with db_lock, connect() as c:
                if c.execute("SELECT 1 FROM users WHERE username=?", (username,)).fetchone():
                    return self.err(409, "такой логин уже занят")
                cur = c.execute(
                    "INSERT INTO users(username, pass_hash, role, created_at) VALUES(?,?,?,?)",
                    (username, hash_password(password), "owner", now()),
                )
                log_event(c, None, "admin_created_user", {"username": username})
            return self.json(200, {"ok": True, "id": cur.lastrowid,
                                   "username": username, "password": password})

        if path.startswith("/api/admin/users/") and method == "DELETE":
            try:
                uid = int(path.rsplit("/", 1)[-1])
            except ValueError:
                return self.err(400, "bad id")
            with db_lock, connect() as c:
                row = c.execute("SELECT username, role FROM users WHERE id=?", (uid,)).fetchone()
                if not row:
                    return self.err(404, "user not found")
                if row["role"] == "admin":
                    return self.err(400, "администратора удалить нельзя")
                # машины отвязываем и выдаём новые коды
                devs = c.execute("SELECT id FROM devices WHERE owner_id=?", (uid,)).fetchall()
                for d in devs:
                    c.execute(
                        "UPDATE devices SET owner_id=NULL, status='unclaimed', claim_code=? WHERE id=?",
                        (make_claim_code(), d["id"]),
                    )
                c.execute("DELETE FROM sessions WHERE user_id=?", (uid,))
                c.execute("DELETE FROM fcm_tokens WHERE user_id=?", (uid,))
                c.execute("DELETE FROM users WHERE id=?", (uid,))
                log_event(c, None, "admin_deleted_user",
                          {"username": row["username"], "devices_unbound": len(devs)})
            return self.json(200, {"ok": True, "devices_unbound": len(devs)})

        if path.startswith("/api/admin/devices/") and method == "DELETE":
            try:
                did = int(path.rsplit("/", 1)[-1])
            except ValueError:
                return self.err(400, "bad id")
            with db_lock, connect() as c:
                row = c.execute("SELECT login FROM devices WHERE id=?", (did,)).fetchone()
                if not row:
                    return self.err(404, "device not found")
                for table in ("samples", "dev_sessions", "commands", "schedules", "maintenance"):
                    c.execute("DELETE FROM %s WHERE device_id=?" % table, (did,))
                c.execute("DELETE FROM devices WHERE id=?", (did,))
                log_event(c, None, "admin_deleted_device", {"login": row["login"]})
            return self.json(200, {"ok": True})

        if path.startswith("/api/admin/devices/") and method == "POST":
            parts = path.split("/")
            try:
                did = int(parts[4])
            except (ValueError, IndexError):
                return self.err(400, "bad id")
            action = parts[5] if len(parts) > 5 else ""
            if action == "assign":
                username = (self.body().get("username") or "").strip().lower()
                with db_lock, connect() as c:
                    urow = c.execute("SELECT id, role FROM users WHERE username=?", (username,)).fetchone()
                    if not urow:
                        return self.err(404, "user not found")
                    if not c.execute("SELECT 1 FROM devices WHERE id=?", (did,)).fetchone():
                        return self.err(404, "device not found")
                    c.execute("UPDATE devices SET owner_id=?, status='active', claim_code=NULL WHERE id=?",
                              (urow["id"], did))
                    log_event(c, did, "admin_assigned_device", {"username": username})
                return self.json(200, {"ok": True, "owner": username})

        if path == "/api/admin/firmware" and method == "GET":
            fw_dir = os.path.join(WEB_DIR, "fw")
            items = []
            if os.path.isdir(fw_dir):
                for name in sorted(os.listdir(fw_dir)):
                    if not name.lower().endswith(".bin"):
                        continue
                    full = os.path.join(fw_dir, name)
                    items.append({
                        "name": name,
                        "size": os.path.getsize(full),
                        "url": SERVER_BASE + "/fw/" + name,
                    })
            return self.json(200, {"items": items, "base": SERVER_BASE})

        if path == "/api/admin/firmware/update" and method == "POST":
            body = self.body()
            dev = self.owned_device(u_ctx := self.require_user(), str(body.get("device", "0")))
            if not dev:
                return self.err(404, "device not found")
            name = os.path.basename((body.get("file") or "").strip())
            full = os.path.join(WEB_DIR, "fw", name)
            if not name.lower().endswith(".bin") or not os.path.isfile(full):
                return self.err(404, "firmware not found")
            url = SERVER_BASE + "/fw/" + name
            with db_lock, connect() as c:
                cur = c.execute(
                    "INSERT INTO commands(device_id,user_id,name,args,status,created_at) VALUES(?,?,?,?,?,?)",
                    (dev["id"], u_ctx["id"], "fw_update",
                     json.dumps({"url": url}, ensure_ascii=False), "queued", now()),
                )
                cid = cur.lastrowid
                log_event(c, dev["id"], "fw_update_queued", {"file": name, "by": u_ctx["username"]})
                online = (now() - (dev["last_seen"] or 0)) < ONLINE_WINDOW_S
            return self.json(200, {"ok": True, "id": cid, "url": url, "pushed": online})

        if path == "/api/admin/users" and method == "GET":
            with db_lock, connect() as c:
                rows = c.execute(
                    "SELECT u.id,u.username,u.role,u.created_at, "
                    "(SELECT COUNT(*) FROM devices d WHERE d.owner_id=u.id) AS devices, "
                    "(SELECT MAX(d.last_seen) FROM devices d WHERE d.owner_id=u.id) AS last_seen, "
                    "EXISTS(SELECT 1 FROM devices d2 WHERE d2.login=u.username) AS is_device "
                    "FROM users u ORDER BY u.id").fetchall()
                out = []
                for r in rows:
                    item = dict(r)
                    devs = c.execute(
                        "SELECT id, login, name, fw, last_seen, status FROM devices "
                        "WHERE owner_id=? ORDER BY id", (r["id"],)
                    ).fetchall()
                    item["device_list"] = [
                        {"id": d["id"], "login": d["login"], "name": d["name"],
                         "fw": d["fw"], "last_seen": d["last_seen"],
                         "online": (now() - (d["last_seen"] or 0)) < ONLINE_WINDOW_S}
                        for d in devs
                    ]
                    item["feedback_open"] = c.execute(
                        "SELECT COUNT(*) FROM feedback WHERE user_id=? AND status='new'",
                        (r["id"],)).fetchone()[0]
                    out.append(item)
            return self.json(200, {"users": out})

        if path == "/api/admin/feedback" and method == "GET":
            with db_lock, connect() as c:
                rows = c.execute(
                    "SELECT f.*, u.username FROM feedback f "
                    "LEFT JOIN users u ON u.id=f.user_id ORDER BY f.id DESC LIMIT 200"
                ).fetchall()
            return self.json(200, {"items": [dict(r) for r in rows]})

        if path.startswith("/api/admin/feedback/") and method == "POST":
            try:
                fid = int(path.rsplit("/", 1)[-1])
            except ValueError:
                return self.err(400, "bad id")
            st = (self.body().get("status") or "done")
            with db_lock, connect() as c:
                c.execute("UPDATE feedback SET status=? WHERE id=?", (st, fid))
            return self.json(200, {"ok": True})

        if path == "/api/admin/events" and method == "GET":
            with db_lock, connect() as c:
                rows = c.execute("SELECT * FROM events ORDER BY id DESC LIMIT 200").fetchall()
            return self.json(200, {"events": [dict(r) for r in rows]})

        if path == "/api/admin/commands" and method == "GET":
            with db_lock, connect() as c:
                rows = c.execute("SELECT * FROM commands ORDER BY id DESC LIMIT 100").fetchall()
            return self.json(200, {"commands": [dict(r) for r in rows]})

        return self.err(404, "unknown admin api")


# ---------------------------------------------------------------- фоновые задачи

def scheduler_loop():
    while True:
        try:
            with db_lock, connect() as c:
                due = c.execute(
                    "SELECT * FROM schedules WHERE status='waiting' AND at_ts<=?", (now(),)
                ).fetchall()
                for s in due:
                    args = json.loads(s["args"]) if s["args"] else {}
                    c.execute(
                        "INSERT INTO commands(device_id,user_id,name,args,status,created_at)"
                        " VALUES(?,?,?,?,?,?)",
                        (s["device_id"], s["user_id"], "clim_on",
                         json.dumps(args, ensure_ascii=False), "queued", now()),
                    )
                    c.execute("UPDATE schedules SET status='sent' WHERE id=?", (s["id"],))
                    log_event(c, s["device_id"], "sched_triggered", {"id": s["id"], "args": args})
        except Exception as exc:  # noqa: BLE001
            print("scheduler error", repr(exc), flush=True)
        time.sleep(15)


def accounts_cleanup_loop():
    while True:
        try:
            with db_lock, connect() as c:
                n = cleanup_unused_accounts(c)
                if n:
                    print("cleanup: removed %d unused accounts" % n, flush=True)
        except Exception as exc:  # noqa: BLE001
            print("accounts cleanup error", repr(exc), flush=True)
        time.sleep(1800)


def samples_cleanup():
    keep_days = int(os.environ.get("LEAF_KEEP_DAYS", "365"))
    while True:
        try:
            with db_lock, connect() as c:
                c.execute("DELETE FROM samples WHERE ts < ?", (now() - keep_days * 86400,))
        except Exception as exc:  # noqa: BLE001
            print("cleanup error", repr(exc), flush=True)
        time.sleep(3600)


def main():
    init_db()
    threading.Thread(target=scheduler_loop, daemon=True).start()
    threading.Thread(target=samples_cleanup, daemon=True).start()
    threading.Thread(target=accounts_cleanup_loop, daemon=True).start()
    port = int(os.environ.get("LEAF_PORT", "8080"))
    srv = ThreadingHTTPServer(("0.0.0.0", port), Api)
    print(f"LeafControl API on :{port}, db={DB_PATH}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
