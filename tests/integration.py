#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import random
import shutil
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "bin" / "myserver"
CLIENT = ROOT / "bin" / "myclient"


def free_udp_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_case(name: str, payload: bytes, drop: int, mss: int = 512, winsz: int = 8) -> None:
    with tempfile.TemporaryDirectory(prefix=f"rudp-{name}-") as temporary:
        work = Path(temporary)
        source = work / "input.bin"
        source.write_bytes(payload)
        output = Path("received") / f"{name}.bin"
        port = free_udp_port()
        env = os.environ.copy()
        env.update({
            "RUDP_DROP_SEED": "7",
            "RUDP_RETRANSMIT_TIMEOUT_MS": "100",
            "RUDP_SERVER_TIMEOUT_MS": "5000",
        })
        server = subprocess.Popen(
            [str(SERVER), str(port), str(drop)],
            cwd=work,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            time.sleep(0.1)
            client = subprocess.run(
                [str(CLIENT), "127.0.0.1", str(port), str(mss), str(winsz), str(source), str(output)],
                cwd=work,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=20,
            )
            if client.returncode != 0:
                raise AssertionError(
                    f"{name}: client returned {client.returncode}\nstdout:\n{client.stdout}\nstderr:\n{client.stderr}"
                )
            destination = work / output
            deadline = time.time() + 2
            while not destination.exists() and time.time() < deadline:
                time.sleep(0.05)
            if not destination.exists():
                raise AssertionError(f"{name}: destination was not created")
            if digest(source) != digest(destination):
                raise AssertionError(f"{name}: output differs from input")
            if payload and "DATA" not in client.stdout:
                raise AssertionError(f"{name}: client did not emit DATA log lines")
            if "ACK" not in client.stdout:
                raise AssertionError(f"{name}: client did not emit ACK log lines")
        finally:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server.kill()


def main() -> None:
    random.seed(4)
    run_case("text", b"reliable transport over UDP\n" * 20, 0)
    run_case("empty", b"", 0)
    run_case("binary", bytes(random.getrandbits(8) for _ in range(50_000)), 0, 1024, 16)
    run_case("loss", bytes(random.getrandbits(8) for _ in range(12_000)), 15, 768, 12)
    print("integration tests passed")


if __name__ == "__main__":
    main()
