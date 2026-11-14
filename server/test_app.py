"""Local HTTP contract checks; no deployed server or firmware required."""

import http.client
import json
import os
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

import app


class ApiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.old_db = app.DB_PATH
        app.DB_PATH = os.path.join(cls.tmp.name, "test.db")
        app.init_db()
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), app.Api)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        app.DB_PATH = cls.old_db
        cls.tmp.cleanup()

    def request(self, method, path, data=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
        body = json.dumps(data).encode() if data is not None else None
        h = dict(headers or {})
        if body is not None:
            h["Content-Type"] = "application/json"
        conn.request(method, path, body=body, headers=h)
        response = conn.getresponse()
        raw = response.read()
        status = response.status
        cookie = response.getheader("Set-Cookie")
        conn.close()
        return status, json.loads(raw) if raw else {}, cookie

    def test_device_sync_hist_session_and_admin(self):
        status, enrolled, _ = self.request("POST", "/api/device/enroll", {
            "serial": "test-serial", "login": "test-board", "fw": "176"})
        self.assertEqual(status, 200)
        headers = {"Authorization": "Bearer " + enrolled["device_token"]}
        t = app.now()
        status, result, _ = self.request("POST", "/api/device/sync", {"ts": t, "soc": 70}, headers)
        self.assertEqual((status, result["ok"]), (200, True))
        status, _, _ = self.request("POST", "/api/device/sync", {"ts": "oops"}, headers)
        self.assertEqual(status, 400)
        status, result, _ = self.request("POST", "/api/device/hist", {
            "points": [[t - 20, 70, 1, 15, 0], [t - 10, 70, 1, 15, 0],
                       [t - 20, 70, 1, 15, 0]]}, headers)
        self.assertEqual((status, result["added"]), (200, 2))
        status, result, _ = self.request("POST", "/api/device/session", {
            "kind": "trip", "started_at": t - 60, "ended_at": t}, headers)
        self.assertEqual(status, 200)
        sid = result["session_id"]
        status, result, _ = self.request("POST", "/api/device/session", {
            "kind": "trip", "started_at": t - 60, "ended_at": t}, headers)
        self.assertEqual((status, result["session_id"], result["duplicate"]), (200, sid, True))
        status, _, _ = self.request("POST", "/api/device/session", {"kind": "trip"}, headers)
        self.assertEqual(status, 400)

        with app.db_lock, app.db() as c:
            c.execute("INSERT INTO users(username,pass_hash,role,created_at) VALUES(?,?,?,?)",
                      ("test-admin", app.hash_password("test-password"), "admin", t))
            uid = c.execute("SELECT id FROM users WHERE username='test-admin'").fetchone()[0]
            c.execute("INSERT INTO sessions(token,user_id,created_at,expires_at) VALUES(?,?,?,?)",
                      ("test-cookie", uid, t, t + 600))
            cmd = c.execute("INSERT INTO commands(device_id,name,status,created_at) VALUES(?,?,?,?)",
                            (enrolled["device_id"], "refresh", "queued", t)).lastrowid
        status, result, _ = self.request("POST", "/api/device/sync", {"ts": t + 5}, headers)
        self.assertEqual((status, result["cmd"]["id"]), (200, cmd))
        self.request("POST", "/api/device/sync", {"ts": t + 10, "ack": [cmd]}, headers)
        self.request("POST", "/api/device/sync", {"ts": t + 15, "nack": [cmd]}, headers)
        with app.db_lock, app.db() as c:
            self.assertEqual(c.execute("SELECT status FROM commands WHERE id=?", (cmd,)).fetchone()[0], "done")
        admin = {"Cookie": "leaf_session=test-cookie"}
        status, result, _ = self.request("POST", f"/api/admin/devices/{enrolled['device_id']}/unbind", {}, admin)
        self.assertEqual((status, result["ok"]), (200, True))
        status, result, _ = self.request("GET", "/api/fw/latest?board=bogus")
        self.assertEqual(status, 400)

    def test_flash_text_and_static_path_boundary(self):
        with open(os.path.join(app.WEB_DIR, "flash.html"), encoding="utf-8") as f:
            text = f.read()
        self.assertIn("Прошивка по USB", text)
        self.assertNotIn("РџСЂ", text)
        status, _, _ = self.request("GET", "/static/../app.py")
        self.assertEqual(status, 404)


if __name__ == "__main__":
    unittest.main()
