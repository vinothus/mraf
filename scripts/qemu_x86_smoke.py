#!/usr/bin/env python3
"""Boot the MRAF firmware on qemu_x86 with slirp networking and smoke-test it.

QEMU's emulated e1000 NIC is bridged through user networking (slirp) and guest
port 80 is forwarded to host 18083, so the dashboard and REST API are reachable
from the development machine even though no real board is attached.
"""
import base64
import http.client
import json
import os
import pty
import re
import select
import signal
import subprocess
import sys
import threading
import time

BUILD = "/workspace/mraf/firmware/zephyr/build/qemu_x86"
BASE = "http://127.0.0.1:18083"
AUTH = ("admin", "farm1234")
BOOT_TIMEOUT = 90.0
LOG = "/tmp/opencode/qemu-x86-boot.log"

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


class Serial:
    def __init__(self, master):
        self.master = master
        self.lines = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def _run(self):
        buf = b""
        while not self.stop.is_set():
            r, _, _ = select.select([self.master], [], [], 0.2)
            if not r:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = ANSI.sub("", raw.decode("utf-8", "replace")).replace("\r", "")
                with self.lock:
                    self.lines.append(line)
                print("  [serial] " + line, flush=True)

    def dump(self):
        with self.lock:
            return list(self.lines)

    def wait_for(self, needle, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                if any(needle in line for line in self.lines):
                    return True
            time.sleep(0.2)
        return False


def _send(conn, path, method="GET", body=None):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if path != "/health":
        token = base64.b64encode(("%s:%s" % AUTH).encode()).decode()
        headers["Authorization"] = "Basic " + token
    conn.request(method, path, body=data, headers=headers)
    resp = conn.getresponse()
    return resp.status, resp.read().decode("utf-8", "replace")


def request(path, method="GET", body=None):
    # Keep one connection alive across calls: QEMU slirp handles HTTP/1.1
    # keep-alive more reliably than a close-after-every-request client, and a
    # power-state change can reset an in-flight socket. Retry once on a fresh
    # connection before declaring failure.
    global _conn
    last_exc = None
    for attempt in range(3):
        try:
            if _conn is None:
                _conn = http.client.HTTPConnection(\
                    "127.0.0.1", 18083, timeout=10)
            return _send(_conn, path, method, body)
        except (ConnectionError, http.client.HTTPException, OSError) as exc:
            last_exc = exc
            if _conn is not None:
                _conn.close()
                _conn = None
            time.sleep(0.5)
    raise last_exc


_conn = None



def check(name, fn, expects):
    try:
        status, body = fn()
        passed = status == 200 and expects(body)
        print("%-34s %s  (HTTP %d, %d bytes)" %
              (name, "PASS" if passed else "FAIL", status, len(body)), flush=True)
        return passed
    except Exception as exc:  # noqa: BLE001 - report any transport failure
        print("%-34s FAIL  (%s)" % (name, exc), flush=True)
        return False


def main():
    master, slave = pty.openpty()
    env = dict(os.environ)
    env["ZEPHYR_TOOLCHAIN_VARIANT"] = "zephyr"
    env["ZEPHYR_SDK_INSTALL_DIR"] = "/opt/zephyr-sdk-1.0.1"
    proc = subprocess.Popen(
        ["west", "build", "-d", BUILD, "-t", "run"],
        cwd="/opt/zephyrproject", env=env,
        stdin=slave, stdout=slave, stderr=slave, close_fds=False,
        start_new_session=True)
    os.close(slave)
    os.set_blocking(master, False)

    serial = Serial(master)
    serial.start()
    ok = True
    try:
        if not serial.wait_for("MRAF ", BOOT_TIMEOUT):
            print("FAIL: firmware did not reach the MRAF banner", file=sys.stderr)
            return 1
        time.sleep(3)
        with open(LOG, "w") as fh:
            fh.write("\n".join(serial.dump()) + "\n")

        ok &= check("GET /health", lambda: request("/health"),
                    lambda b: '"ok":true' in b.replace(" ", ""))
        ok &= check("GET / dashboard", lambda: request("/"),
                    lambda b: "<!doctype html" in b.lower())
        ok &= check("GET /api/status (auth)", lambda: request("/api/status"),
                    lambda b: '"ip":"10.0.2.15"' in b.replace(" ", ""))
        ok &= check("GET /api/config", lambda: request("/api/config"),
                    lambda b: '"device"' in b)
        ok &= check("POST /api/config", lambda: request("/api/config", "POST", {"timezone": "UTC0"}),
                    lambda b: True)
        ok &= check("POST /api/sim/power", lambda: request("/api/sim/power", "POST", {"present": False}),
                    lambda b: '"ok":true' in b.replace(" ", ""))
        ok &= check("POST /api/sim/power restore", lambda: request("/api/sim/power", "POST", {"present": True}),
                    lambda b: '"ok":true' in b.replace(" ", ""))
        ok &= check("GET /api/status after POST", lambda: request("/api/status"),
                    lambda b: '"present":true' in b.replace(" ", ""))
        time.sleep(1)
    finally:
        with open(LOG, "w") as fh:
            fh.write("\n".join(serial.dump()) + "\n")
        serial.stop.set()
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)
        os.close(master)
        try:
            os.unlink(os.path.join(BUILD, "qemu.pid"))
        except OSError:
            pass

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
