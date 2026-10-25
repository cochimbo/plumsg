#!/usr/bin/env python3
"""Compares two runs of run.sh (the base branch's and the change's) ROM by ROM.

    compare.py BASE_DIR HEAD_DIR REPORT.html [SUMMARY.md]

A ROM is "same" when every frame drew the same picture in both runs (hashes.txt),
else it says on which frame they first went apart. The HTML report puts the
screenshots of both runs side by side, those that changed first; the Markdown table
is for the workflow's summary page. The exit code is always 0: whether a change is
good or bad is for a person to see.
"""
import base64
import html
import json
import sys
from pathlib import Path


def runs(root: Path):
    return {str(p.parent.relative_to(root)) for p in root.rglob("hashes.txt")} | \
           {str(p.parent.relative_to(root)) for p in root.rglob("exit-code.txt")}


def frames(d: Path):
    """frame number -> picture hash, from a run's hashes.txt."""
    out = {}
    f = d / "hashes.txt"
    if f.exists():
        for line in f.read_text().splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0].isdigit():
                out[int(parts[0])] = parts[1]
    return out


def exit_code(d: Path):
    f = d / "exit-code.txt"
    return f.read_text().strip() if f.exists() else "-"


def img(p: Path):
    if not p.exists():
        return "<div class=missing>(none)</div>"
    data = base64.b64encode(p.read_bytes()).decode()
    return f'<img src="data:image/png;base64,{data}">'


def main():
    base, head, report = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    summary = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    rows = []
    for name in sorted(runs(base) | runs(head)):
        b, h = base / name, head / name
        fb, fh = frames(b), frames(h)
        if not fb or not fh:
            status, first = ("only in the change" if fh else "only in the base"), None
        else:
            first = next((n for n in sorted(set(fb) | set(fh)) if fb.get(n) != fh.get(n)), None)
            status = "same" if first is None else "changed"
        rows.append((name, status, first, exit_code(b), exit_code(h), b, h))
    rows.sort(key=lambda r: (r[1] == "same", r[0]))

    changed = sum(1 for r in rows if r[1] != "same")
    lines = [f"### Test ROMs: {changed} of {len(rows)} changed", "",
             "| ROM | result | first different frame | exit (base → change) |", "|---|---|---|---|"]
    for name, status, first, eb, eh, _, _ in rows:
        mark = "✅ same" if status == "same" else f"⚠️ {status}"
        lines.append(f"| {name} | {mark} | {'' if first is None else first} | {eb} → {eh} |")
    lines += ["", "Screenshots side by side: the **test-rom-report** artifact (report.html)."]
    if summary:
        with summary.open("a") as f:
            f.write("\n".join(lines) + "\n")
    print("\n".join(lines))

    parts = ["<!doctype html><meta charset=utf-8><title>Test ROMs</title><style>",
             "body{font:14px sans-serif;background:#111;color:#ddd;margin:16px}",
             "h2{margin-top:32px}.same{color:#7c7}.changed{color:#fc6}",
             "table{border-collapse:collapse}td{padding:4px;vertical-align:top}",
             "img{image-rendering:pixelated;width:320px}.missing{width:320px;color:#888}",
             "</style>", f"<h1>Test ROMs: {changed} of {len(rows)} changed</h1>"]
    for name, status, first, eb, eh, b, h in rows:
        cls = "same" if status == "same" else "changed"
        note = "" if first is None else f" (first at frame {first})"
        parts.append(f"<h2>{html.escape(name)} <span class={cls}>{status}{note}</span></h2>")
        if status == "same":
            parts.append(f"<table><tr><td>{img(h / 'last.png')}</td></tr></table>")
            continue
        shots = sorted({p.name for p in b.glob('*.png')} | {p.name for p in h.glob('*.png')})
        parts.append("<table><tr><th>base</th><th>change</th></tr>")
        for s in shots:
            parts.append(f"<tr><td>{img(b / s)}<br>{s}</td><td>{img(h / s)}<br>{s}</td></tr>")
        parts.append("</table>")
    report.write_text("\n".join(parts))


if __name__ == "__main__":
    main()
