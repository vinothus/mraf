#!/usr/bin/env python3
"""Record the ESP32 firmware booting under Espressif QEMU as an MP4.

Runs the actual flash_image.bin under qemu-system-xtensa through a pty,
timestamps every serial line, then renders a terminal-style video with
ffmpeg so the "demo" can be shown without a real board.
"""
import fcntl
import os
import pty
import re
import select
import subprocess
import sys
import time

QEMU = "/opt/qemu-xtensa-softmmu/bin/qemu-system-xtensa"
FLASH = "/workspace/mraf/firmware/zephyr/build/esp32-sb/zephyr/zephyr/flash_image.bin"
EFUSE = "/workspace/mraf/firmware/zephyr/build/esp32-sb/zephyr/zephyr/qemu_efuse_eco3.bin"
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
OUT = "/workspace/mraf/docs/demo/mraf-qemu-esp32-boot.mp4"
RUN_SECONDS = 40
W, H = 1280, 960
FS = 20
ROW_STEP = 26
MARGIN_X, MARGIN_Y = 40, 48
# boot log is ~33 lines; keep every line inside the frame
MAX_ROWS = (H - MARGIN_Y - ROW_STEP) // ROW_STEP

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def drawtext_filter(lines, total):
    parts = []
    for i, (t, text) in enumerate(lines):
        esc = text.replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")
        esc = esc.replace("%", "\\%")
        tt = max(0.2, t)
        parts.append(
            "drawtext=fontfile=%s:fontsize=%d:fontcolor=0xe5e5e5:"
            "x=%d:y=%d:text='%s':enable='gte(t,%.3f)'"
            % (FONT, FS, MARGIN_X, MARGIN_Y + i * ROW_STEP, esc, tt)
        )
    # blinking block cursor on the row after the last line
    cy = MARGIN_Y + len(lines) * ROW_STEP
    parts.append(
        "drawtext=fontfile=%s:fontsize=%d:fontcolor=0x9be29b:"
        "x=%d:y=%d:text='█':enable='gte(t,%.3f)*eq(0,mod(floor(t*2),2))'"
        % (FONT, FS, MARGIN_X, cy, max(0.2, lines[-1][0] if lines else 0.2))
    )
    return ",".join(parts)


def main():
    if not os.path.exists(OUT):
        os.makedirs(os.path.dirname(OUT), exist_ok=True)

    master, slave = pty.openpty()
    cmd = [
        QEMU, "-machine", "esp32", "-nographic",
        "-drive", "file=%s,if=mtd,format=raw" % FLASH,
        "-drive", "file=%s,if=none,format=raw,id=efuse" % EFUSE,
        "-global", "driver=nvram.esp32.efuse,property=drive,value=efuse",
        "-global", "driver=timer.esp32.timg,property=wdt_disable,value=true",
    ]
    proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave,
                            close_fds=False)
    os.close(slave)
    os.set_blocking(master, False)

    start = None
    chunks = []  # (t_rel, text)
    buf = b""
    end = time.monotonic() + RUN_SECONDS
    while time.monotonic() < end:
        if proc.poll() is not None:
            break
        r, _, _ = select.select([master], [], [], 0.2)
        if not r:
            continue
        try:
            data = os.read(master, 4096)
        except BlockingIOError:
            continue
        except OSError:
            break
        if not data:
            break
        now = time.monotonic()
        if start is None:
            start = now
        chunks.append((now - start, data))
    os.close(master)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    if not chunks:
        print("no serial output captured", file=sys.stderr)
        return 1

    # Merge chunks into timestamped lines
    events = []
    for t, data in chunks:
        buf += data
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = ANSI.sub("", raw.decode("utf-8", "replace")).replace("\r", "")
            if line.strip() == "" and events and not events[-1][1]:
                continue
            events.append((t, line))
    if buf:
        line = ANSI.sub("", buf.decode("utf-8", "replace")).replace("\r", "")
        events.append((chunks[-1][0], line))

    total = min(RUN_SECONDS, events[-1][0] + 6.0)
    events = [(t, s) for t, s in events if t <= total]

    print("captured %d lines over %.1fs" % (len(events), total))

    vf = drawtext_filter(events, total)
    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "color=c=0x0c0c0c:s=%dx%d:r=10:d=%.2f" % (W, H, total),
        "-vf", vf,
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "22",
        "-movflags", "+faststart", OUT,
    ]
    subprocess.run(cmd, check=True)
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
