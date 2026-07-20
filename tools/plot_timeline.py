#!/usr/bin/env python3
"""Convert myclient CSV output into a dependency-free SVG packet timeline."""
from __future__ import annotations

import csv
from datetime import datetime
from pathlib import Path
import sys


def parse_time(value: str) -> datetime:
    return datetime.fromisoformat(value.strip().replace("Z", "+00:00"))


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: plot_timeline.py client.log timeline.svg", file=sys.stderr)
        return 2
    rows = []
    with Path(sys.argv[1]).open(newline="", encoding="utf-8") as source:
        for row in csv.reader(source):
            if len(row) < 3 or row[1].strip() not in {"DATA", "ACK"}:
                continue
            rows.append((parse_time(row[0]), row[1].strip(), int(row[2])))
    if not rows:
        print("No DATA or ACK rows found", file=sys.stderr)
        return 1
    start = rows[0][0]
    points = [((timestamp - start).total_seconds(), kind, sequence) for timestamp, kind, sequence in rows]
    max_time = max(value[0] for value in points) or 1.0
    max_seq = max(value[2] for value in points) or 1
    width, height, pad = 1000, 520, 60

    def x(value: float) -> float:
        return pad + value / max_time * (width - 2 * pad)

    def y(value: int) -> float:
        return height - pad - value / max_seq * (height - 2 * pad)

    data = " ".join(f"{x(t):.1f},{y(s):.1f}" for t, k, s in points if k == "DATA")
    ack = " ".join(f"{x(t):.1f},{y(s):.1f}" for t, k, s in points if k == "ACK")
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
  <rect width="100%" height="100%" fill="white"/>
  <line x1="{pad}" y1="{height-pad}" x2="{width-pad}" y2="{height-pad}" stroke="#222"/>
  <line x1="{pad}" y1="{pad}" x2="{pad}" y2="{height-pad}" stroke="#222"/>
  <text x="{width/2}" y="{height-15}" text-anchor="middle" font-family="sans-serif" font-size="16">Time (seconds)</text>
  <text x="18" y="{height/2}" text-anchor="middle" transform="rotate(-90 18 {height/2})" font-family="sans-serif" font-size="16">Sequence number</text>
  <polyline points="{data}" fill="none" stroke="#a55d45" stroke-width="2"/>
  <polyline points="{ack}" fill="none" stroke="#2e5f78" stroke-width="2"/>
  <text x="{width-pad-160}" y="{pad}" font-family="sans-serif" font-size="14" fill="#a55d45">DATA</text>
  <text x="{width-pad-90}" y="{pad}" font-family="sans-serif" font-size="14" fill="#2e5f78">ACK</text>
</svg>'''
    Path(sys.argv[2]).write_text(svg, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
