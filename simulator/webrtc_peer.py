"""WebRTC data-channel peer for the native simulator.

This stands in for the ESP32: it registers with the Spring Boot control plane,
exchanges SDP/ICE through the server's signalling registry and then serves the
dashboard JSON directly to the browser over a peer-to-peer RTCDataChannel. The
server relays the handshake but never sees the data.

aiortc (https://github.com/aiortc/aiortc) provides the WebRTC stack.
"""

from __future__ import annotations

import asyncio
import json
import logging
import threading
import time
import urllib.error
import urllib.request
from typing import Any, Optional

log = logging.getLogger("mraf.webrtc")

try:
    from aiortc import (
        RTCConfiguration,
        RTCIceCandidate,
        RTCIceServer,
        RTCPeerConnection,
        RTCSessionDescription,
    )
    HAVE_AIORTC = True
except Exception as exc:  # pragma: no cover - optional dependency
    HAVE_AIORTC = False
    _AIORTC_ERROR = str(exc)


class WebRtcPeer:
    """Runs an aiortc peer in a background event loop and bridges it to SimApp."""

    def __init__(self, app: Any):
        self.app = app
        self.cfg = app.cfg.get("webrtc", {}) if isinstance(app.cfg, dict) else {}
        self.device_id = (app.cfg.get("device", {}) or {}).get("device_id") or "SIM"
        self.server = str(self.cfg.get("server") or "").rstrip("/")
        self.token = (app.cfg.get("coap", {}) or {}).get("token") or ""
        self.interval_ms = int(self.cfg.get("poll_ms") or 400)
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._pc: Optional[Any] = None
        self._channel: Optional[Any] = None
        self._since = 0
        self._synced = False
        self._lock = threading.Lock()
        self.state = "off"
        self.last_error = ""
        self.rtt_ms: Optional[float] = None
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.connected_at: Optional[float] = None
        self.stun_url = ""
        self.last_ping_at = 0.0

    # -- lifecycle ----------------------------------------------------------
    @property
    def enabled(self) -> bool:
        return bool(self.cfg.get("enabled")) and bool(self.server)

    def start(self) -> None:
        if not self.enabled:
            self.state = "disabled"
            return
        if not HAVE_AIORTC:
            self.state = "unavailable"
            self.last_error = "aiortc not installed"
            log.warning("WebRTC requested but aiortc is unavailable: %s", self.last_error)
            return
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._thread_main, name="mraf-webrtc", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        loop = self._loop
        if loop and loop.is_running():
            asyncio.run_coroutine_threadsafe(self._shutdown(), loop)
        if self._thread:
            self._thread.join(timeout=3.0)

    def _thread_main(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._run())
        except Exception as exc:  # pragma: no cover
            self.last_error = str(exc)
            log.warning("WebRTC loop stopped: %s", exc)
        finally:
            self._loop.close()

    # -- HTTP helpers -------------------------------------------------------
    async def _http(self, method: str, path: str, body: Optional[dict] = None) -> dict:
        url = self.server + path
        headers = {"X-Mraf-Token": self.token}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"

        def _do() -> dict:
            req = urllib.request.Request(url, data=data, method=method, headers=headers)
            with urllib.request.urlopen(req, timeout=8) as resp:
                raw = resp.read().decode("utf-8")
            return json.loads(raw) if raw else {}

        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, _do)

    # -- main loop ----------------------------------------------------------
    async def _run(self) -> None:
        while not self._stop.is_set():
            try:
                await self._ensure_peer()
                await self._poll_signals()
            except Exception as exc:
                self.last_error = str(exc)
                self.state = "error"
                log.debug("WebRTC poll error: %s", exc)
            await asyncio.sleep(max(0.15, self.interval_ms / 1000.0))

    async def _ensure_peer(self) -> None:
        if self._pc is not None:
            return
        try:
            cfg = await self._http(
                "GET", f"/api/webrtc/config?device_id={self.device_id}")
        except Exception as exc:
            self.state = "no-server"
            self.last_error = str(exc)
            return
        ice_servers = []
        for entry in cfg.get("iceServers", []):
            urls = entry.get("urls")
            if urls:
                ice_servers.append(RTCIceServer(urls=urls))
                self.stun_url = urls
        configuration = RTCConfiguration(iceServers=ice_servers)
        pc = RTCPeerConnection(configuration=configuration)
        self._pc = pc
        self.state = "waiting"

        @pc.on("datachannel")
        def _on_datachannel(channel):  # noqa: ANN001
            self._channel = channel
            channel.on("message", self._on_message)

        @pc.on("icecandidate")
        async def _on_icecandidate(candidate):  # noqa: ANN001
            if candidate is not None:
                await self._post_signal("candidate", {
                    "candidate": candidate.candidate,
                    "sdpMid": candidate.sdpMid,
                    "sdpMLineIndex": candidate.sdpMLineIndex,
                })

        @pc.on("connectionstatechange")
        async def _on_state():  # noqa: ANN202
            state = pc.connectionState
            self.state = state
            if state == "connected":
                self.connected_at = time.time()
            await self._report()

    async def _poll_signals(self) -> None:
        data = await self._http(
            "GET",
            f"/api/webrtc/signal?device_id={self.device_id}&role=device&since={self._since}",
        )
        if not self._synced:
            # On (re)start adopt the mailbox head as the cursor so we never
            # replay a handshake that predates this process (a stale offer
            # would otherwise yield an answer with a mismatched ICE ufrag/pwd).
            self._since = max(self._since, int(data.get("seq", 0) or 0))
            self._synced = True
            return
        for sig in data.get("signals", []):
            self._since = max(self._since, int(sig.get("seq", 0)))
            await self._handle_signal(sig)

    async def _handle_signal(self, sig: dict) -> None:
        kind = sig.get("type")
        if kind == "offer":
            # A fresh offer always starts a new session: drop any previous
            # peer connection so a browser reconnect cannot renegotiate a
            # half-open link (which leaves ICE stuck and no answer posted).
            if self._pc is not None:
                await self._teardown_peer()
            await self._ensure_peer()
        pc = self._pc
        if pc is None:
            return
        if kind == "offer":
            await pc.setRemoteDescription(
                RTCSessionDescription(sdp=sig.get("sdp", ""), type="offer"))
            answer = await pc.createAnswer()
            await pc.setLocalDescription(answer)
            # aiortc does not emit trickle candidates; the gathered candidates
            # are folded into the local description, so wait for gathering to
            # finish before shipping the answer SDP.
            await self._wait_gathering(pc)
            await self._post_signal("answer", {"sdp": pc.localDescription.sdp})
        elif kind == "candidate" and sig.get("candidate"):
            try:
                await pc.addIceCandidate(
                    RTCIceCandidate(sig["candidate"], sig.get("sdpMid"),
                                    sig.get("sdpMLineIndex")))
            except Exception as exc:
                log.debug("addIceCandidate failed: %s", exc)
        elif kind == "bye":
            await self._teardown_peer()

    async def _wait_gathering(self, pc, timeout: float = 6.0) -> None:
        if pc.iceGatheringState == "complete":
            return
        done = asyncio.Event()

        @pc.on("icegatheringstatechange")
        def _on_gathering():
            if pc.iceGatheringState == "complete":
                done.set()

        try:
            await asyncio.wait_for(done.wait(), timeout)
        except asyncio.TimeoutError:
            log.debug("ICE gathering timed out; sending partial answer")

    async def _post_signal(self, sig_type: str, payload: dict) -> None:
        body = {"device_id": self.device_id, "from": "device", "type": sig_type}
        body.update(payload)
        try:
            await self._http("POST", "/api/webrtc/signal", body)
        except Exception as exc:
            log.debug("signal post failed: %s", exc)

    async def _report(self) -> None:
        body = {
            "device_id": self.device_id,
            "role": "device",
            "state": self.state,
            "transport": "datachannel",
            "rx_bytes": self.rx_bytes,
            "tx_bytes": self.tx_bytes,
            "stun": self.stun_url,
        }
        if self.rtt_ms is not None:
            body["rtt_ms"] = round(self.rtt_ms, 2)
        try:
            await self._http("POST", "/api/webrtc/report", body)
        except Exception:
            pass

    # -- data channel -------------------------------------------------------
    def _on_message(self, raw: Any) -> None:
        if isinstance(raw, (bytes, bytearray)):
            try:
                raw = raw.decode("utf-8")
            except Exception:
                return
        self.rx_bytes += len(raw) if isinstance(raw, str) else 0
        try:
            req = json.loads(raw)
        except Exception:
            return
        reply = self._dispatch(req)
        text = json.dumps(reply)
        self.tx_bytes += len(text)
        channel = self._channel
        if channel is not None and channel.readyState == "open":
            try:
                channel.send(text)
            except Exception as exc:
                log.debug("channel send failed: %s", exc)

    def _dispatch(self, req: dict) -> dict:
        rid = req.get("id")
        cmd = str(req.get("cmd") or "")
        try:
            if cmd == "ping":
                self.last_ping_at = time.time()
                return {"id": rid, "ok": True, "data": {"pong": req.get("t"), "transport": "webrtc"}}
            if cmd == "status":
                return {"id": rid, "ok": True, "data": self.app.snapshot()}
            if cmd == "config":
                return {"id": rid, "ok": True, "data": self.app.config_api()}
            if cmd == "events":
                return {"id": rid, "ok": True, "data": {"events": self.app.events()}}
            if cmd == "wireguard":
                return {"id": rid, "ok": True, "data": self.app.wireguard_status()}
            if cmd == "control":
                body = req.get("body") or {}
                status = self.app.control(
                    str(body.get("target", "")),
                    str(body.get("mode", "auto")),
                    int(body.get("id") or 0),
                    int(body.get("boost_min") or 0),
                    int(body.get("pulse_ms") or 0),
                )
                return {"id": rid, "ok": True, "data": {"ok": True, **status}}
            if cmd == "chat":
                return {"id": rid, "ok": True, "data": self.app.chat(str(req.get("text") or ""))}
            if cmd == "time":
                return {"id": rid, "ok": True, "data": {"ok": True, **self.app.set_time(float(req.get("epoch") or 0))}}
            if cmd == "pump-reset":
                return {"id": rid, "ok": True, "data": {"ok": True, **self.app.reset_pump_fault()}}
            if cmd == "bye":
                return {"id": rid, "ok": True, "data": {"bye": True}}
            return {"id": rid, "ok": False, "error": f"unknown command '{cmd}'"}
        except Exception as exc:
            return {"id": rid, "ok": False, "error": str(exc)}

    # -- teardown -----------------------------------------------------------
    async def _teardown_peer(self) -> None:
        pc = self._pc
        self._pc = None
        self._channel = None
        self.state = "closed"
        if pc is not None:
            try:
                await pc.close()
            except Exception:
                pass

    async def _shutdown(self) -> None:
        await self._teardown_peer()
        await self._report()

    # -- status -------------------------------------------------------------
    def status(self) -> dict:
        return {
            "enabled": bool(self.cfg.get("enabled")),
            "available": HAVE_AIORTC,
            "server": self.server,
            "device_id": self.device_id,
            "state": self.state,
            "connected": self.state == "connected",
            "transport": "datachannel",
            "stun": self.stun_url,
            "rtt_ms": self.rtt_ms,
            "rx_bytes": self.rx_bytes,
            "tx_bytes": self.tx_bytes,
            "connected_at": int(self.connected_at) if self.connected_at else 0,
            "last_error": self.last_error,
        }
