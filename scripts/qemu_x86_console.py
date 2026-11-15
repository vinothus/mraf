#!/usr/bin/env python3
"""Run the qemu_x86 target and mirror its serial console to stdout.

Boots the firmware through the Zephyr ``run`` target in a pty so QEMU sees a
real terminal, then copies serial output to this process' stdout until the
child exits or a signal arrives. Handy for poking the HTTP API from another
shell while watching the guest log.

Usage: qemu_x86_console.py [--build DIR]
"""
import argparse
import os
import pty
import select
import signal
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build",
                        default="/workspace/mraf/firmware/zephyr/build/qemu_x86")
    args = parser.parse_args()

    env = dict(os.environ)
    env["ZEPHYR_TOOLCHAIN_VARIANT"] = "zephyr"
    env["ZEPHYR_SDK_INSTALL_DIR"] = "/opt/zephyr-sdk-1.0.1"

    master, slave = pty.openpty()
    proc = subprocess.Popen(
        ["west", "build", "-d", args.build, "-t", "run"],
        cwd="/opt/zephyrproject", env=env,
        stdin=slave, stdout=slave, stderr=slave, close_fds=False)

    def shutdown(*_):
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    os.close(slave)

    try:
        while True:
            r, _, _ = select.select([master], [], [], 0.5)
            if not r:
                if proc.poll() is not None:
                    break
                continue
            try:
                data = os.read(master, 4096)
            except OSError:
                break
            if not data:
                break
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    finally:
        shutdown()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        os.close(master)
        try:
            os.unlink(os.path.join(args.build, "qemu.pid"))
        except OSError:
            pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
