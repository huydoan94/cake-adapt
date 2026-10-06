#!/usr/bin/env python3
"""Two-way charts of a router cake-adapt log against a LibreQoS report.

Download is drawn above zero and upload below it, on one shared time axis
(seconds from the report's startedAt; both clocks are epoch time).

usage: chart.py LOG REPORT OUT.svg [--overview]

Without --overview the chart covers the LibreQoS test and adds its
per-second throughput bins and RTT samples. With it, the chart covers the
whole log and marks the test window. Standard library only.
"""

import gzip
import json
import sys

DL = "#1f6fb4"
DL_LIGHT = "#8cbfe8"
UL = "#d0661c"
UL_LIGHT = "#f0b27a"
GREY = "#555"
WIDTH = 1200
LEFT = 70
RIGHT = 20
PANEL_GAP = 46


def read_log(path, t0):
    opener = gzip.open if path.endswith(".gz") else open
    load, summary, queue, shaper = [], [], [], []
    with opener(path, "rt") as log:
        for line in log:
            fields = line.rstrip("\n").split("; ")
            kind = fields[0]
            if kind == "LOAD":
                t = float(fields[2]) - t0
                load.append((t, int(fields[4]) / 1e3, int(fields[5]) / 1e3,
                             int(fields[6]) / 1e3, int(fields[7]) / 1e3))
            elif kind == "SUMMARY":
                t = float(fields[2]) - t0
                summary.append((t, int(fields[7]) / 1e3, int(fields[8]) / 1e3))
            elif kind == "TCP_QUEUE":
                t = float(fields[2]) - t0
                dl = int(fields[5]) / 1e3 if fields[4] == "1" else None
                ul = int(fields[7]) / 1e3 if fields[6] == "1" else None
                queue.append((t, dl, ul))
            elif kind == "SHAPER":
                shaper.append(float(fields[2]) - t0)
    return load, summary, queue, shaper


class Panel:
    def __init__(self, top, height, x0, x1, ymax_up, ymax_down, unit):
        self.top, self.height = top, height
        self.x0, self.x1 = x0, x1
        self.up, self.down = ymax_up, ymax_down
        self.unit = unit
        self.out = []

    def x(self, t):
        return LEFT + (t - self.x0) / (self.x1 - self.x0) * (WIDTH - LEFT - RIGHT)

    def y(self, v):
        span = self.up + self.down
        return self.top + (self.up - v) / span * self.height

    def clip(self, v):
        return max(-self.down, min(self.up, v))

    def line(self, points, color, width=1.4, dash=None, opacity=1.0):
        segment = []
        for t, v in points + [(None, None)]:
            if v is None or t is None or not self.x0 <= t <= self.x1:
                if len(segment) > 1:
                    d = " ".join(f"{a:.1f},{b:.1f}" for a, b in segment)
                    extra = f' stroke-dasharray="{dash}"' if dash else ""
                    self.out.append(
                        f'<polyline points="{d}" fill="none" stroke="{color}" '
                        f'stroke-width="{width}" stroke-opacity="{opacity}"'
                        f'{extra}/>')
                segment = []
                continue
            segment.append((self.x(t), self.y(self.clip(v))))

    def steps(self, bins, color, width=2.2):
        # LibreQoS bins are one-second averages: draw each as a flat step.
        pts = []
        for start, end, v in bins:
            pts += [(start, v), (end, v)]
        self.line(pts, color, width, dash="5,3")

    def dots(self, points, color, r=2.2):
        for t, v in points:
            if self.x0 <= t <= self.x1:
                self.out.append(
                    f'<circle cx="{self.x(t):.1f}" cy="{self.y(self.clip(v)):.1f}" '
                    f'r="{r}" fill="{color}" fill-opacity="0.7"/>')

    def frame(self, title, step, label_up, label_down, xstep):
        o = []
        bottom = self.top + self.height
        right = WIDTH - RIGHT
        o.append(f'<text x="{LEFT}" y="{self.top - 8}" font-weight="bold">{title}</text>')
        v = -self.down
        while v <= self.up + 1e-9:
            y = self.y(v)
            o.append(f'<line x1="{LEFT}" y1="{y:.1f}" x2="{right}" y2="{y:.1f}" '
                     f'stroke="#ddd"/>')
            o.append(f'<text x="{LEFT - 6}" y="{y + 4:.1f}" text-anchor="end">'
                     f'{abs(v):g}</text>')
            v += step
        y0 = self.y(0)
        o.append(f'<line x1="{LEFT}" y1="{y0:.1f}" x2="{right}" y2="{y0:.1f}" '
                 f'stroke="#888" stroke-width="1.2"/>')
        t = (int(self.x0 // xstep) + 1) * xstep
        while t <= self.x1:
            x = self.x(t)
            o.append(f'<line x1="{x:.1f}" y1="{self.top}" x2="{x:.1f}" y2="{bottom}" '
                     f'stroke="#eee"/>')
            o.append(f'<text x="{x:.1f}" y="{bottom + 14}" text-anchor="middle">{t:g}</text>')
            t += xstep
        o.append(f'<rect x="{LEFT}" y="{self.top}" width="{right - LEFT}" '
                 f'height="{self.height}" fill="none" stroke="#999"/>')
        mid_up = self.y(self.up / 2)
        mid_down = self.y(-self.down / 2)
        o.append(f'<text transform="translate(16,{mid_up:.1f}) rotate(-90)" '
                 f'text-anchor="middle" fill="{DL}">{label_up}</text>')
        o.append(f'<text transform="translate(16,{mid_down:.1f}) rotate(-90)" '
                 f'text-anchor="middle" fill="{UL}">{label_down}</text>')
        o.append(f'<text x="{LEFT - 6}" y="{self.top - 8}" text-anchor="end" '
                 f'fill="{GREY}">{self.unit}</text>')
        return o + self.out


def legend(x, y, items):
    o = []
    for label, color, dash, kind in items:
        if kind == "dot":
            o.append(f'<circle cx="{x + 12}" cy="{y - 4}" r="3" fill="{color}"/>')
        else:
            extra = f' stroke-dasharray="{dash}"' if dash else ""
            o.append(f'<line x1="{x}" y1="{y - 4}" x2="{x + 24}" y2="{y - 4}" '
                     f'stroke="{color}" stroke-width="2"{extra}/>')
        o.append(f'<text x="{x + 30}" y="{y}">{label}</text>')
        x += 36 + 6.4 * len(label)
    return o


def phases_band(report, panels, top, bottom):
    o = []
    shades = {"download": DL_LIGHT, "upload": UL_LIGHT, "bidirectional": "#c9b6e4"}
    first = panels[0]
    for phase in report["phases"]:
        a, b = phase["startMs"] / 1e3, phase["endMs"] / 1e3
        color = shades.get(phase["name"], "#e6e6e6")
        opacity = 0.28 if phase["name"] in shades else 0.35
        xa, xb = first.x(a), first.x(b)
        o.append(f'<rect x="{xa:.1f}" y="{top}" width="{xb - xa:.1f}" '
                 f'height="{bottom - top}" fill="{color}" fill-opacity="{opacity}"/>')
        o.append(f'<text x="{(xa + xb) / 2:.1f}" y="{top + 13}" text-anchor="middle" '
                 f'fill="{GREY}" font-size="11">{phase["name"]}</text>')
    return o


def svg(width, height, body, title):
    return "\n".join([
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="sans-serif" font-size="12">',
        f'<rect width="{width}" height="{height}" fill="white"/>',
        f'<text x="{LEFT}" y="22" font-size="15" font-weight="bold">{title}</text>',
        *body, "</svg>", ""])


def chart(log_path, report_path, out_path, overview):
    report = json.load(open(report_path))
    t0 = report["startedAt"] / 1e3
    load, summary, queue, shaper = read_log(log_path, t0)
    s = report["summary"]
    if overview:
        x0 = min(p[0] for p in load) - 2
        x1 = max(p[0] for p in load) + 2
        xstep = 30
    else:
        x0, x1, xstep = -2, report["durationMs"] / 1e3 + 2, 5

    rate = Panel(64, 330, x0, x1, 160, 100, "Mbit/s")
    delay = Panel(64 + 330 + PANEL_GAP, 220, x0, x1, 40, 40, "ms")
    panels = [rate, delay]
    if not overview:
        rtt = Panel(delay.top + 220 + PANEL_GAP, 140, x0, x1, 60, 0, "ms")
        panels.append(rtt)
    bottom = panels[-1].top + panels[-1].height

    rate.line([(p[0], p[3]) for p in load], DL, 2.0, dash="2,2")
    rate.line([(p[0], -p[4]) for p in load], UL, 2.0, dash="2,2")
    rate.line([(p[0], p[1]) for p in load], DL, 1.2)
    rate.line([(p[0], -p[2]) for p in load], UL, 1.2)

    delay.line([(q[0], q[1]) for q in queue], DL_LIGHT, 1.6)
    delay.line([(q[0], None if q[2] is None else -q[2]) for q in queue], UL_LIGHT, 1.6)
    delay.line([(p[0], p[1]) for p in summary], DL, 1.3)
    delay.line([(p[0], -p[2]) for p in summary], UL, 1.3)

    body = []
    if overview:
        a, b = rate.x(0), rate.x(report["durationMs"] / 1e3)
        body.append(f'<rect x="{a:.1f}" y="{rate.top}" width="{b - a:.1f}" '
                    f'height="{bottom - rate.top}" fill="#ffe680" fill-opacity="0.35"/>')
        body.append(f'<text x="{(a + b) / 2:.1f}" y="{rate.top + 13}" '
                    f'text-anchor="middle" fill="{GREY}" font-size="11">LibreQoS test</text>')
    else:
        body += phases_band(report, panels, rate.top, bottom)
        bins = report["bins1s"]
        rate.steps([(b["startMs"] / 1e3, b["endMs"] / 1e3, b["downloadMbps"]) for b in bins],
                   "#0b3d66")
        rate.steps([(b["startMs"] / 1e3, b["endMs"] / 1e3, -b["uploadMbps"]) for b in bins],
                   "#7a3200")
        base = s["baselineRtt"]
        colors = {"download": DL, "upload": UL, "bidirectional": "#7b4fb8"}
        for phase in ("baseline", "downloadWarmup", "download", "uploadWarmup", "upload",
                      "bidirectional", "recovery"):
            rtt.dots([(x["elapsedMs"] / 1e3, x["rttMs"] - base)
                      for x in report["latencySamples"]
                      if x["phase"] == phase and not x["loss"]], colors.get(phase, GREY))

    for t in shaper:
        if x0 <= t <= x1:
            x = rate.x(t)
            body.append(f'<line x1="{x:.1f}" y1="{rate.top + rate.height - 4}" x2="{x:.1f}" '
                        f'y2="{rate.top + rate.height}" stroke="{GREY}"/>')

    body += rate.frame("Throughput: CAKE bandwidth and traffic", 20,
                       "download", "upload", xstep)
    body += delay.frame("Delay: fping one-way delta (controller input) and TCP queue estimate",
                        10, "download", "upload", xstep)
    rows = [bottom + 56]
    if not overview:
        body += rtt.frame(f"LibreQoS RTT above its baseline ({base:.1f} ms), per sample",
                          20, "RTT", "", xstep)
    body.append(f'<text x="{(LEFT + WIDTH - RIGHT) / 2}" y="{bottom + 32}" '
                f'text-anchor="middle" fill="{GREY}">seconds from LibreQoS test start '
                f'({report["metadata"]["generatedAt"][:10]}, startedAt {t0:.3f})</text>')

    items = [("CAKE bandwidth (router)", GREY, "2,2", "line"),
             ("traffic through CAKE (router)", GREY, None, "line")]
    if not overview:
        items.append(("LibreQoS 1 s bin", "#222", "5,3", "line"))
    items += [("fping delta, averaged", GREY, None, "line"),
              ("TCP queue estimate (light)", "#aaa", None, "line")]
    body += legend(LEFT, rows[0], items)
    footer = [f'LibreQoS summary: download {s["downloadThroughputMbps"]:.1f} Mbit/s, '
              f'upload {s["uploadThroughputMbps"]:.1f} Mbit/s; bloat {s["downloadBloat"]:.1f} / '
              f'{s["uploadBloat"]:.1f} / {s["bidirectionalBloat"]:.1f} ms '
              f'(download / upload / bidirectional); grade {s["totalGrade"]}.',
              'Ticks under the throughput panel are SHAPER changes. Values beyond an axis '
              'are clipped to it. fping measures round trips, so its delta is the same in '
              'both halves.']
    for i, text in enumerate(footer):
        body.append(f'<text x="{LEFT}" y="{rows[0] + 22 + 16 * i}" fill="{GREY}">{text}</text>')
    title = ("cake-adapt on the router, whole log" if overview
             else "cake-adapt on the router during a LibreQoS test")
    open(out_path, "w").write(svg(WIDTH, rows[0] + 52, body, title))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--overview"]
    chart(args[0], args[1], args[2], "--overview" in sys.argv)
