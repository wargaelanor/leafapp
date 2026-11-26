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

    def test_climate_schedule_duration_and_ownership(self):
        t = app.now()
        with app.db_lock, app.db() as c:
            c.execute("INSERT INTO users(username,pass_hash,role,created_at) VALUES(?,?,?,?)",
                      ("schedule-owner", app.hash_password("test-password"), "owner", t))
            uid = c.execute("SELECT id FROM users WHERE username='schedule-owner'").fetchone()[0]
            c.execute("INSERT INTO sessions(token,user_id,created_at,expires_at) VALUES(?,?,?,?)",
                      ("schedule-cookie", uid, t, t + 600))
            dev = c.execute("INSERT INTO devices(serial,login,device_token,owner_id,status,created_at) VALUES(?,?,?,?,?,?)",
                             ("schedule-board", "schedule-board", "schedule-token", uid, "active", t)).lastrowid
            other_dev = c.execute("INSERT INTO devices(serial,login,device_token,owner_id,status,created_at) VALUES(?,?,?,?,?,?)",
                                  ("schedule-board-2", "schedule-board-2", "schedule-token-2", uid, "active", t)).lastrowid
        auth = {"Cookie": "leaf_session=schedule-cookie"}
        for minutes in (0, 4, 61):
            status, _, _ = self.request("POST", "/api/sched", {
                "device": dev, "at": t + 3600, "minutes": minutes}, auth)
            self.assertEqual(status, 400)
        status, result, _ = self.request("POST", "/api/sched", {
            "device": dev, "at": t + 3600, "minutes": 15}, auth)
        self.assertEqual(status, 200)
        status, sched, _ = self.request("GET", "/api/sched", headers=auth)
        self.assertEqual(status, 200)
        self.assertTrue(any(x["id"] == result["id"] for x in sched["schedules"]))
        status, implicit, _ = self.request("POST", "/api/sched", {
            "at": t + 7200, "minutes": 20}, auth)
        self.assertEqual(status, 200)
        self.assertNotEqual(implicit["id"], result["id"])
        status, _, _ = self.request("POST", "/api/sched/cancel", {
            "device": other_dev, "id": result["id"]}, auth)
        self.assertEqual(status, 404)
        status, cancelled, _ = self.request("POST", "/api/sched/cancel", {
            "device": dev, "id": result["id"]}, auth)
        self.assertEqual(status, 200)
        self.assertTrue(cancelled["ok"])
        status, _, _ = self.request("POST", "/api/sched/cancel", {"id": result["id"]}, auth)
        self.assertEqual(status, 200)
        status, sched, _ = self.request("GET", "/api/sched", headers=auth)
        self.assertEqual(next(x for x in sched["schedules"] if x["id"] == result["id"])["status"], "cancelled")
        status, other_sched, _ = self.request("GET", f"/api/sched?device={other_dev}", headers=auth)
        self.assertEqual(status, 200)
        self.assertFalse(any(x["id"] == result["id"] for x in other_sched["schedules"]))
        with app.db_lock, app.db() as c:
            scheduled = c.execute("SELECT at_ts,args FROM schedules WHERE id=?", (result["id"],)).fetchone()
            due = c.execute("SELECT id FROM schedules WHERE status='waiting' AND at_ts<=?", (t + 3600,)).fetchall()
            events = c.execute("SELECT COUNT(*) FROM events WHERE device_id=? AND kind='sched_cancelled'", (dev,)).fetchone()[0]
        self.assertNotIn(result["id"], [x["id"] for x in due])
        self.assertEqual(events, 1)
        self.assertEqual(scheduled["at_ts"], t + 3600)
        self.assertEqual(json.loads(scheduled["args"])["minutes"], 15)

        with app.db_lock, app.db() as c:
            for kind, stamp, km, soc0, soc1 in (
                ("charge", t - 9000, 0, 40, 90),
                ("trip", t - 7000, 12, 90, 72),
                ("trip", t - 5000, 8, 72, 60),
                ("charge", t - 3000, 0, 60, 95),
                ("trip", t - 1000, 4, 95, 89),
            ):
                c.execute("INSERT INTO dev_sessions(device_id,kind,started_at,ended_at,"
                          "distance_km,energy_kwh,soc_start,soc_end) VALUES(?,?,?,?,?,?,?,?)",
                          (dev, kind, stamp - 100, stamp, km, km / 5 if km else 0, soc0, soc1))
            c.execute("INSERT INTO samples(device_id,ts,payload) VALUES(?,?,?)",
                      (dev, t - 5000, json.dumps({"amb": 12.5})))
        status, stats, _ = self.request("GET", "/api/stats?device=" + str(dev), headers=auth)
        self.assertEqual(status, 200)
        self.assertTrue(any(day["amb"] == 12.5 for day in stats["per_day"]))
        self.assertEqual(len(stats["discharge_cycles"]), 1)
        self.assertEqual(stats["discharge_cycles"][0]["km"], 20)
        self.assertEqual(stats["discharge_cycles"][0]["soc_start"], 90)
        self.assertEqual(stats["discharge_cycles"][0]["soc_end"], 60)

        with app.db_lock, app.db() as c:
            c.execute("INSERT INTO commands(device_id,name,args,status,created_at) VALUES(?,?,?,?,?)",
                      (dev, "clim_on", json.dumps({"minutes": 15,
                       "_schedule_expires_at": t - 60}), "queued", t - 120))
        status, sync, _ = self.request("POST", "/api/device/sync", {"ts": app.now()},
                                       {"Authorization": "Bearer schedule-token"})
        self.assertEqual(status, 200)
        self.assertIsNone(sync["cmd"])
        with app.db_lock, app.db() as c:
            expired = c.execute("SELECT status FROM commands WHERE device_id=? AND name='clim_on'",
                                (dev,)).fetchone()
        self.assertEqual(expired["status"], "expired")

    def test_interface_without_design_switch(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
        conn.request("GET", "/classic")
        response = conn.getresponse()
        self.assertEqual(response.status, 302)
        self.assertEqual(response.getheader("Location"), "/login")
        response.read()
        conn.close()

        t = app.now()
        with app.db_lock, app.db() as c:
            c.execute("INSERT INTO users(username,pass_hash,role,created_at) VALUES(?,?,?,?)",
                      ("design-user", app.hash_password("test-password"), "owner", t))
            uid = c.execute("SELECT id FROM users WHERE username='design-user'").fetchone()[0]
            c.execute("INSERT INTO sessions(token,user_id,created_at,expires_at) VALUES(?,?,?,?)",
                      ("design-cookie", uid, t, t + 600))
        for path, marker in (("/", b"redesign.css"), ("/radius", b"class=\"beta\""),
                             ("/radius.html", b"class=\"beta\""),
                             ("/classic", b"class=\"tabbar\""),
                             ("/classic.html", b"class=\"tabbar\"")):
            conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
            conn.request("GET", path, headers={"Cookie": "leaf_session=design-cookie"})
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            body = response.read()
            self.assertIn(marker, body)
            self.assertNotIn("Дизайн интерфейса".encode("utf-8"), body)
            conn.close()

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
