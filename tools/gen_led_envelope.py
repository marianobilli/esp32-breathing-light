#!/usr/bin/env python3
"""
gen_led_envelope.py
Builds include/led_envelope.h from breath_in.wav and breath_out.wav.

Each table represents a "breath progress" 0..255 sampled every 50 ms. The
shape is a threshold-triggered, linear-in-time ramp whose local slope is
modulated by the audio's per-window dBFS relative to its own active-region
average (windows louder than average steepen the slope, quieter ones
flatten it). See plan in /Users/mbilling/.claude/plans/ for the full
derivation.

Usage:
    python3 tools/gen_led_envelope.py [--on-db -40] [--off-db -52]

Two thresholds because audio onsets are sharper than offsets:
  --on-db  : first crossing — marks "audio starts" (high, above noise floor)
  --off-db : last crossing  — marks "audio fades out" (low, captures the tail)
Both clips share the same pair (they were recorded with the same setup).
"""

import argparse
import glob
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from datetime import datetime

SAMPLE_RATE    = 16000
WINDOW_MS      = 50
WINDOW_SAMPLES = SAMPLE_RATE * WINDOW_MS // 1000  # 800
INT16_MAX      = 32767
SILENT_DB      = -120.0

DEFAULT_ON_DB  = -40.0
DEFAULT_OFF_DB = -52.0

# Firmware profile bends: all baked into led_envelope.h as separate tables so
# firmware can switch between them at runtime via the OLED menu.
DEFAULT_PROFILE_BENDS = [0.0, 0.9, 1.5, 2.5]

# Viz comparison bends default to the firmware profiles (so the viz shows
# exactly what's flashed). Override with --compare-bends for exploration.
DEFAULT_COMPARE_BENDS = DEFAULT_PROFILE_BENDS


def _bend_color(i: int, n: int) -> str:
    """Evenly-spaced hue from cyan (i=0) to red (i=n-1). Works for any n."""
    h = 180 - (i / max(n - 1, 1)) * 180
    return f"hsl({int(h)}, 75%, 45%)"

SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT   = os.path.dirname(SCRIPT_DIR)
AUDIO_DIR   = os.path.join(REPO_ROOT, "data")
INCLUDE_DIR = os.path.join(REPO_ROOT, "include")


def _is_canonical_wav(path: str) -> bool:
    """True if the file is already 16-bit mono WAV at SAMPLE_RATE."""
    try:
        with wave.open(path, "rb") as wf:
            return (wf.getsampwidth() == 2
                    and wf.getnchannels() == 1
                    and wf.getframerate() == SAMPLE_RATE)
    except (wave.Error, EOFError):
        return False


def _ffmpeg_convert(src: str, dst: str) -> None:
    """Transcode src to 16-bit mono WAV at SAMPLE_RATE at dst."""
    if shutil.which("ffmpeg") is None:
        sys.exit(
            f"error: '{src}' is not 16-bit mono {SAMPLE_RATE} Hz WAV and "
            f"ffmpeg is not installed.\n"
            f"Install it (macOS: 'brew install ffmpeg') or pre-convert the file."
        )
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-i", src, "-ac", "1", "-ar", str(SAMPLE_RATE),
         "-sample_fmt", "s16", dst],
        check=True,
    )


def find_breath_source(stem: str) -> str:
    """Return path to breath_{stem}.*; prefer .wav, else first other match."""
    wav_path = os.path.join(AUDIO_DIR, f"breath_{stem}.wav")
    if os.path.exists(wav_path):
        return wav_path
    candidates = sorted(glob.glob(os.path.join(AUDIO_DIR, f"breath_{stem}.*")))
    if not candidates:
        sys.exit(f"error: no data/breath_{stem}.* file found")
    return candidates[0]


def ensure_canonical_wav(src: str) -> str:
    """Ensure data/breath_{in,out}.wav is canonical. Returns the canonical path.

    If src is already canonical, returns it unchanged. Otherwise converts via
    ffmpeg and writes the result to data/breath_{in,out}.wav (overwriting any
    non-canonical .wav in place), so `pio run -t uploadfs` flashes what the
    firmware's I2S setup expects. Non-.wav sources are left untouched.
    """
    base, ext = os.path.splitext(src)
    canonical = base + ".wav" if ext.lower() != ".wav" else src

    if _is_canonical_wav(src) and canonical == src:
        return src

    # Convert to a temp file first so a failure doesn't corrupt data/.
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
    try:
        _ffmpeg_convert(src, tmp_path)
        shutil.move(tmp_path, canonical)
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
    print(f"converted {os.path.basename(src)} -> {os.path.basename(canonical)} "
          f"(16-bit mono {SAMPLE_RATE} Hz)")
    return canonical


def rms_envelope(path: str) -> list[float]:
    """Per-window RMS in int16 units from a 16-bit mono WAV at SAMPLE_RATE."""
    with wave.open(path, "rb") as wf:
        assert wf.getsampwidth() == 2, "Expected 16-bit samples"
        assert wf.getnchannels() == 1, "Expected mono audio"
        assert wf.getframerate() == SAMPLE_RATE, f"Expected {SAMPLE_RATE} Hz"
        raw = wf.readframes(wf.getnframes())

    samples = struct.unpack(f"<{len(raw) // 2}h", raw)

    out = []
    for i in range(0, len(samples), WINDOW_SAMPLES):
        window = samples[i : i + WINDOW_SAMPLES]
        if not window:
            break
        out.append(math.sqrt(sum(s * s for s in window) / len(window)))
    return out


def to_dbfs(rms: float) -> float:
    if rms < 1.0:
        return SILENT_DB
    return 20.0 * math.log10(rms / INT16_MAX)


def compute_envelope(rms: list[float], on_db: float, off_db: float, bend: float, ascending: bool):
    """Returns (table, info). See module docstring for the algorithm."""
    n = len(rms)
    rms_db = [to_dbfs(v) for v in rms]

    # first_in: first window above the (high) on-threshold — skips noise floor.
    # last_in : last  window above the (low)  off-threshold — captures fading tail.
    first_candidates = [i for i, db in enumerate(rms_db) if db >= on_db]
    last_candidates  = [i for i, db in enumerate(rms_db) if db >= off_db]
    if not first_candidates:
        raise RuntimeError(
            f"No window crosses on-threshold {on_db:.1f} dBFS "
            f"(peak={max(rms_db):.1f} dBFS)."
        )
    if not last_candidates:
        raise RuntimeError(
            f"No window crosses off-threshold {off_db:.1f} dBFS."
        )
    first_in = first_candidates[0]
    last_in  = last_candidates[-1]
    if last_in - first_in < 2:
        raise RuntimeError(
            f"Active region too short ({last_in - first_in + 1} window(s)) "
            f"— lower on-threshold or raise off-threshold."
        )

    active = rms_db[first_in : last_in + 1]
    peak_db = max(active)
    avg_db  = sum(active) / len(active)

    # Per-window slope weight: linear, anchored at 1.0 at avg_db,
    # clamped to [0.5, 1.5] at the active-region extremes. Above-average
    # loudness steepens the local slope, below-average flattens it.
    eps = 1e-6
    min_db = min(active)
    span_up   = max(peak_db - avg_db, eps)
    span_down = max(avg_db - min_db, eps)

    weights = [0.0] * n
    for i in range(first_in, last_in + 1):
        db = rms_db[i]
        if db >= avg_db:
            u = (db - avg_db) / span_up
            weights[i] = 1.0 + bend * u           # 1.0 at avg, (1+bend) at peak
        else:
            u = (avg_db - db) / span_down
            # floor keeps progress strictly monotonic when bend >= 1.0 (at which
            # point the quietest windows would otherwise hit 0 or go negative).
            weights[i] = max(0.01, 1.0 - bend * u)

    cum_total = sum(weights)
    progress = [0.0] * n
    running = 0.0
    for i in range(first_in, last_in + 1):
        running += weights[i]
        progress[i] = running / cum_total  # progress[last_in] == 1.0 exactly

    table = [0] * n
    if ascending:
        for i in range(n):
            if i < first_in:
                table[i] = 0
            elif i > last_in:
                table[i] = 255
            else:
                table[i] = round(progress[i] * 255)
    else:
        for i in range(n):
            if i < first_in:
                table[i] = 255
            elif i > last_in:
                table[i] = 0
            else:
                table[i] = round((1.0 - progress[i]) * 255)

    info = {
        "n_windows":  n,
        "first_in":   first_in,
        "last_in":    last_in,
        "first_in_s": first_in * WINDOW_MS / 1000.0,
        "last_in_s":  last_in  * WINDOW_MS / 1000.0,
        "peak_db":    peak_db,
        "avg_db":     avg_db,
        "rms_db":     rms_db,
        "ascending":  ascending,
    }
    return table, info


VIZ_HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>LED envelope visualization</title>
<style>
  body { font: 14px/1.4 system-ui, -apple-system, sans-serif; max-width: 1100px; margin: 24px auto; padding: 0 16px; color: #222; }
  h1 { font-size: 18px; margin: 0 0 4px; }
  .meta { color: #666; font-size: 12px; margin-bottom: 24px; }
  section { margin-bottom: 32px; }
  section header { font-weight: 600; margin-bottom: 4px; }
  section .stats { color: #666; font-size: 12px; margin-bottom: 6px; }
  .legend { font-size: 12px; color: #555; margin-top: 4px; margin-bottom: 6px; }
  .legend span { margin-right: 14px; white-space: nowrap; }
  .swatch { display: inline-block; width: 14px; height: 2px; vertical-align: middle; margin-right: 4px; }
  .swatch.dashed { height: 0; border-top: 2px dashed currentColor; }
  svg { width: 100%; height: auto; display: block; background: #fff; border: 1px solid #eee; }
  .tooltip { position: absolute; background: rgba(0,0,0,0.85); color: #fff; padding: 4px 8px; border-radius: 3px; font-size: 11px; pointer-events: none; white-space: nowrap; z-index: 10; }
  .axis text { font: 10px sans-serif; fill: #555; }
  .grid line { stroke: #eee; }
</style>
</head>
<body>
<h1>LED envelope visualization</h1>
<div class="meta">
  Generated <span id="gen-time"></span>
  &middot; on_db = <span id="on-db"></span> dBFS
  &middot; off_db = <span id="off-db"></span> dBFS
  &middot; firmware profiles = <span id="profiles"></span>
  &middot; window = <span id="win-ms"></span> ms
</div>
<div id="charts"></div>
<div id="tooltip" class="tooltip" style="display:none"></div>

<script>
window.envelopeData = __PAYLOAD__;
</script>

<script>
__RENDERER_JS__
</script>
</body>
</html>
"""

VIZ_RENDERER_JS = r"""
(function () {
  const data = window.envelopeData;
  document.getElementById('gen-time').textContent = data.generated_at;
  document.getElementById('on-db').textContent = data.on_db.toFixed(1);
  document.getElementById('off-db').textContent = data.off_db.toFixed(1);
  document.getElementById('profiles').textContent =
    '[' + data.profile_bends.map(b => b.toFixed(2)).join(', ') + ']';
  document.getElementById('win-ms').textContent = data.window_ms;

  const isProfile = b => data.profile_bends.some(p => Math.abs(p - b) < 1e-6);

  const charts = document.getElementById('charts');
  const tooltip = document.getElementById('tooltip');

  data.clips.forEach(clip => {
    const sec = document.createElement('section');
    const dur = (clip.rms_db.length * data.window_ms / 1000).toFixed(2);
    sec.innerHTML =
      '<header>' + clip.filename + ' — ' + (clip.ascending ? 'inhale (LED 0 \u2192 255)' : 'exhale (LED 255 \u2192 0)') + '</header>' +
      '<div class="stats">' +
        dur + 's &middot; ' + clip.rms_db.length + ' windows &middot; ' +
        'avg = ' + clip.avg_db.toFixed(1) + ' dBFS &middot; ' +
        'peak = ' + clip.peak_db.toFixed(1) + ' dBFS &middot; ' +
        'active = [' + clip.first_in_s.toFixed(2) + 's .. ' + clip.last_in_s.toFixed(2) + 's]' +
      '</div>' +
      '<div class="legend">' +
        '<span style="color:#555"><span class="swatch" style="background:#888"></span>volume (dBFS, left axis)</span>' +
        clip.series.map(s =>
          '<span style="color:' + s.color + '">' +
          '<span class="swatch" style="background:' + s.color + '"></span>' +
          'LED bend=' + s.bend.toFixed(2) +
          (isProfile(s.bend) ? ' \u2605' : '') +
          '</span>'
        ).join('') +
        '<span style="color:#9a3412"><span class="swatch dashed"></span>on_db</span>' +
        '<span style="color:#7f1d1d"><span class="swatch dashed"></span>off_db</span>' +
        '<span style="color:#065f46"><span class="swatch dashed"></span>active region</span>' +
      '</div>' +
      '<div class="chart-container" style="position:relative"></div>';
    charts.appendChild(sec);
    renderChart(sec.querySelector('.chart-container'), clip, data);
  });

  function pickStep(range, targetTicks) {
    if (range <= 0) return 1;
    const raw = range / targetTicks;
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const norm = raw / mag;
    let nice;
    if (norm < 1.5) nice = 1;
    else if (norm < 3) nice = 2;
    else if (norm < 7) nice = 5;
    else nice = 10;
    return nice * mag;
  }

  function renderChart(container, clip, data) {
    const W = 1000, H = 340;
    const m = { top: 16, right: 56, bottom: 32, left: 56 };
    const pw = W - m.left - m.right;
    const ph = H - m.top - m.bottom;

    const winS = data.window_ms / 1000;
    const n = clip.rms_db.length;
    const tMax = n * winS;

    let dbMin = Math.min.apply(null, clip.rms_db);
    if (!isFinite(dbMin)) dbMin = -80;
    dbMin = Math.floor(dbMin / 10) * 10;
    // also include thresholds in the visible range
    dbMin = Math.min(dbMin, Math.floor(Math.min(data.on_db, data.off_db) / 10) * 10);
    const dbMax = 0;

    const xOf = t => m.left + (t / tMax) * pw;
    const yDb = db => m.top + (1 - (Math.max(dbMin, Math.min(db, dbMax)) - dbMin) / (dbMax - dbMin)) * ph;
    const yBr = br => m.top + (1 - br / 255) * ph;

    const xStep = pickStep(tMax, 10);
    const xTicks = [];
    for (let t = 0; t <= tMax + 1e-9; t += xStep) xTicks.push(t);

    const dbTicks = [];
    for (let v = dbMin; v <= dbMax + 1e-9; v += 10) dbTicks.push(v);

    const brTicks = [0, 50, 100, 150, 200, 250];

    const dbPath = clip.rms_db
      .map((v, i) => (i ? 'L' : 'M') + xOf(i * winS).toFixed(1) + ',' + yDb(v).toFixed(1))
      .join(' ');

    const brPaths = clip.series.map(s => ({
      bend:  s.bend,
      color: s.color,
      width: isProfile(s.bend) ? 2.25 : 1.25,
      d: s.brightness
        .map((v, i) => (i ? 'L' : 'M') + xOf(i * winS).toFixed(1) + ',' + yBr(v).toFixed(1))
        .join(' '),
    }));

    const onY  = yDb(data.on_db);
    const offY = yDb(data.off_db);

    let svg = '<svg viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="xMidYMid meet">';
    svg += '<rect x="' + m.left + '" y="' + m.top + '" width="' + pw + '" height="' + ph + '" fill="#fafafa" stroke="#ccc"/>';

    svg += '<g class="grid">';
    xTicks.forEach(t => {
      const x = xOf(t);
      svg += '<line x1="' + x + '" y1="' + m.top + '" x2="' + x + '" y2="' + (m.top + ph) + '" />';
    });
    dbTicks.forEach(v => {
      const y = yDb(v);
      svg += '<line x1="' + m.left + '" y1="' + y + '" x2="' + (m.left + pw) + '" y2="' + y + '" />';
    });
    svg += '</g>';

    svg += '<line x1="' + m.left + '" y1="' + onY  + '" x2="' + (m.left + pw) + '" y2="' + onY  + '" stroke="#f97316" stroke-width="1" stroke-dasharray="6 3" />';
    svg += '<line x1="' + m.left + '" y1="' + offY + '" x2="' + (m.left + pw) + '" y2="' + offY + '" stroke="#dc2626" stroke-width="1" stroke-dasharray="6 3" />';

    [clip.first_in_s, clip.last_in_s].forEach(t => {
      const x = xOf(t);
      svg += '<line x1="' + x + '" y1="' + m.top + '" x2="' + x + '" y2="' + (m.top + ph) + '" stroke="#10b981" stroke-width="1" stroke-dasharray="3 3" />';
    });

    svg += '<path d="' + dbPath + '" fill="none" stroke="#888" stroke-width="1.5"/>';
    brPaths.forEach(p => {
      svg += '<path d="' + p.d + '" fill="none" stroke="' + p.color + '" stroke-width="' + p.width + '"/>';
    });

    svg += '<g class="axis">';
    xTicks.forEach(t => {
      const x = xOf(t);
      const label = (t < 1 ? t.toFixed(2) : t.toFixed(1)) + 's';
      svg += '<text x="' + x + '" y="' + (m.top + ph + 14) + '" text-anchor="middle">' + label + '</text>';
    });
    dbTicks.forEach(v => {
      const y = yDb(v);
      svg += '<text x="' + (m.left - 6) + '" y="' + (y + 3) + '" text-anchor="end">' + v + '</text>';
    });
    brTicks.forEach(v => {
      const y = yBr(v);
      svg += '<text x="' + (m.left + pw + 6) + '" y="' + (y + 3) + '" text-anchor="start">' + v + '</text>';
    });
    svg += '<text x="' + (m.left + pw - 4) + '" y="' + (onY  - 3) + '" text-anchor="end" fill="#f97316">on=' + data.on_db.toFixed(0) + '</text>';
    svg += '<text x="' + (m.left + pw - 4) + '" y="' + (offY - 3) + '" text-anchor="end" fill="#dc2626">off=' + data.off_db.toFixed(0) + '</text>';
    svg += '</g>';

    svg += '<line class="crosshair" x1="0" y1="' + m.top + '" x2="0" y2="' + (m.top + ph) + '" stroke="#000" stroke-width="0.5" style="display:none" />';
    svg += '</svg>';
    container.innerHTML = svg;

    const svgEl = container.querySelector('svg');
    const crosshair = svgEl.querySelector('.crosshair');
    svgEl.addEventListener('mousemove', e => {
      const rect = svgEl.getBoundingClientRect();
      const scaleX = W / rect.width;
      const xSvg = (e.clientX - rect.left) * scaleX;
      if (xSvg < m.left || xSvg > m.left + pw) {
        crosshair.style.display = 'none';
        tooltip.style.display = 'none';
        return;
      }
      const t = (xSvg - m.left) / pw * tMax;
      const idx = Math.max(0, Math.min(n - 1, Math.round(t / winS)));
      const sx = xOf(idx * winS);
      crosshair.setAttribute('x1', sx);
      crosshair.setAttribute('x2', sx);
      crosshair.style.display = '';
      tooltip.style.display = '';
      tooltip.style.left = (e.pageX + 12) + 'px';
      tooltip.style.top  = (e.pageY + 12) + 'px';
      let tipHtml =
        't = ' + (idx * winS).toFixed(2) + 's<br>' +
        'dBFS = ' + clip.rms_db[idx].toFixed(1);
      clip.series.forEach(s => {
        const star = isProfile(s.bend) ? ' &#9733;' : '';
        tipHtml += '<br><span style="color:' + s.color + '">&#9632;</span>' +
                   ' bend=' + s.bend.toFixed(2) + star + ': LED=' + s.brightness[idx];
      });
      tooltip.innerHTML = tipHtml;
    });
    svgEl.addEventListener('mouseleave', () => {
      crosshair.style.display = 'none';
      tooltip.style.display = 'none';
    });
  }
})();
"""


def write_viz_html(out_path: str, on_db: float, off_db: float, profile_bends: list[float], clips: list[dict]):
    payload = {
        "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "on_db": on_db,
        "off_db": off_db,
        "profile_bends": list(profile_bends),
        "window_ms": WINDOW_MS,
        "clips": clips,
    }
    payload_json = json.dumps(payload)
    html = VIZ_HTML_TEMPLATE.replace("__PAYLOAD__", payload_json).replace("__RENDERER_JS__", VIZ_RENDERER_JS)
    with open(out_path, "w") as f:
        f.write(html)


def format_array(name: str, data: list[int]) -> str:
    items_per_line = 16
    lines = []
    for i in range(0, len(data), items_per_line):
        chunk = data[i : i + items_per_line]
        lines.append("    " + ", ".join(f"{v:3d}" for v in chunk))
    body = ",\n".join(lines)
    return f"static const uint8_t {name}[{len(data)}] = {{\n{body}\n}};"


def main():
    parser = argparse.ArgumentParser(
        description="Generate include/led_envelope.h from the breath WAVs.",
    )
    parser.add_argument(
        "--on-db",
        type=float,
        default=DEFAULT_ON_DB,
        help=f"dBFS threshold for first crossing — marks audio start "
             f"(default {DEFAULT_ON_DB}).",
    )
    parser.add_argument(
        "--off-db",
        type=float,
        default=DEFAULT_OFF_DB,
        help=f"dBFS threshold for last crossing — marks audio fade-out "
             f"(default {DEFAULT_OFF_DB}).",
    )
    parser.add_argument(
        "--profile-bends",
        type=lambda s: [float(x) for x in s.split(",") if x.strip()],
        default=DEFAULT_PROFILE_BENDS,
        help=f"Comma-separated bend values baked into led_envelope.h as "
             f"runtime-switchable profiles (default "
             f"{','.join(str(v) for v in DEFAULT_PROFILE_BENDS)}). "
             f"The firmware menu cycles through these.",
    )
    parser.add_argument(
        "--compare-bends",
        type=lambda s: [float(x) for x in s.split(",") if x.strip()],
        default=None,  # falls back to --profile-bends if unset
        help="Comma-separated bend values plotted in the viz for "
             "side-by-side comparison. Defaults to --profile-bends.",
    )
    args = parser.parse_args()
    if any(b < 0.0 for b in args.profile_bends):
        parser.error("--profile-bends values must all be >= 0")
    if not args.profile_bends:
        parser.error("--profile-bends must contain at least one value")
    if args.compare_bends is None:
        args.compare_bends = list(args.profile_bends)
    if any(b < 0.0 for b in args.compare_bends):
        parser.error("--compare-bends values must all be >= 0")

    src_in  = ensure_canonical_wav(find_breath_source("in"))
    src_out = ensure_canonical_wav(find_breath_source("out"))

    rms_in  = rms_envelope(src_in)
    rms_out = rms_envelope(src_out)

    # Compute one (table_in, table_out) pair per profile bend.
    profile_tables_in  = []
    profile_tables_out = []
    info_in_ref  = None
    info_out_ref = None
    for bend in args.profile_bends:
        tbl_in,  info_in  = compute_envelope(rms_in,  args.on_db, args.off_db, bend, ascending=True)
        tbl_out, info_out = compute_envelope(rms_out, args.on_db, args.off_db, bend, ascending=False)
        profile_tables_in.append(tbl_in)
        profile_tables_out.append(tbl_out)
        # Active region / avg / peak are bend-independent; capture once for logging.
        if info_in_ref is None:
            info_in_ref, info_out_ref = info_in, info_out

    n_in  = info_in_ref["n_windows"]
    n_out = info_out_ref["n_windows"]

    print(f"thresholds: on={args.on_db:.1f} dBFS, off={args.off_db:.1f} dBFS")
    print(f"profiles  : {args.profile_bends}")
    print(f"breath_in : {n_in:3d} windows, "
          f"active=[{info_in_ref['first_in_s']:.2f}s..{info_in_ref['last_in_s']:.2f}s], "
          f"avg={info_in_ref['avg_db']:6.1f} dBFS, peak={info_in_ref['peak_db']:6.1f} dBFS")
    print(f"breath_out: {n_out:3d} windows, "
          f"active=[{info_out_ref['first_in_s']:.2f}s..{info_out_ref['last_in_s']:.2f}s], "
          f"avg={info_out_ref['avg_db']:6.1f} dBFS, peak={info_out_ref['peak_db']:6.1f} dBFS")

    # --- Build the header ---
    nprof = len(args.profile_bends)
    profile_list_str = ", ".join(f"{b:.2f}" for b in args.profile_bends)
    labels_cpp = ", ".join(f"\"{b:.2f}\"" for b in args.profile_bends)
    in_ptrs_cpp  = ", ".join(f"kLedEnvelopeIn_{i}"  for i in range(nprof))
    out_ptrs_cpp = ", ".join(f"kLedEnvelopeOut_{i}" for i in range(nprof))

    in_tables_cpp  = "\n\n".join(
        format_array(f"kLedEnvelopeIn_{i}",  profile_tables_in[i])  for i in range(nprof)
    )
    out_tables_cpp = "\n\n".join(
        format_array(f"kLedEnvelopeOut_{i}", profile_tables_out[i]) for i in range(nprof)
    )

    header = f"""\
// Auto-generated by tools/gen_led_envelope.py — do not edit manually.
//
// Generation parameters:
//   on_db  = {args.on_db:.1f} dBFS (first-crossing — audio start)
//   off_db = {args.off_db:.1f} dBFS (last-crossing — audio fade-out)
//   profile bends = [{profile_list_str}]
//   breath_in : avg = {info_in_ref['avg_db']:.1f} dBFS, peak = {info_in_ref['peak_db']:.1f} dBFS,
//               active = [{info_in_ref['first_in_s']:.2f}s .. {info_in_ref['last_in_s']:.2f}s]
//   breath_out: avg = {info_out_ref['avg_db']:.1f} dBFS, peak = {info_out_ref['peak_db']:.1f} dBFS,
//               active = [{info_out_ref['first_in_s']:.2f}s .. {info_out_ref['last_in_s']:.2f}s]
//
// Both tables are sampled every {WINDOW_MS} ms. Within the active region the
// curve is a linear-in-time ramp whose local slope is modulated by the
// audio's per-window dBFS relative to its own active-region average. The
// `bend` knob controls how strongly that modulation bends the linear base.
//
// kLedEnvelopeIn_i  (0 → 255): held at 0 before the first threshold crossing,
//                              ramps to 255 by the last crossing, then held at 255.
// kLedEnvelopeOut_i (255 → 0): held at 255 before the first threshold crossing,
//                              falls to 0 by the last crossing, then held at 0.
//
// Firmware indexes kLedEnvelope{{In,Out}}Profiles[currentBendIdx] at runtime.
#pragma once
#include <stdint.h>

#define LED_ENV_NUM_PROFILES {nprof}
#define LED_ENV_WINDOW_MS   {WINDOW_MS}
#define LED_ENV_IN_N        {n_in}
#define LED_ENV_OUT_N       {n_out}
#define BREATH_IN_MS        (LED_ENV_IN_N  * LED_ENV_WINDOW_MS)
#define BREATH_OUT_MS       (LED_ENV_OUT_N * LED_ENV_WINDOW_MS)

{in_tables_cpp}

{out_tables_cpp}

static const uint8_t *const kLedEnvelopeInProfiles[LED_ENV_NUM_PROFILES] = {{
    {in_ptrs_cpp}
}};

static const uint8_t *const kLedEnvelopeOutProfiles[LED_ENV_NUM_PROFILES] = {{
    {out_ptrs_cpp}
}};

static const char *const kLedEnvelopeProfileLabels[LED_ENV_NUM_PROFILES] = {{
    {labels_cpp}
}};
"""

    out_path = os.path.join(INCLUDE_DIR, "led_envelope.h")
    with open(out_path, "w") as f:
        f.write(header)
    print(f"Wrote {out_path}")

    # --- Viz: one series per --compare-bends value ---
    def build_series(rms: list[float], ascending: bool) -> list[dict]:
        series = []
        n = len(args.compare_bends)
        for i, b in enumerate(args.compare_bends):
            tbl, _ = compute_envelope(rms, args.on_db, args.off_db, b, ascending=ascending)
            series.append({"bend": b, "color": _bend_color(i, n), "brightness": tbl})
        return series

    clips = [
        {
            "name":       "breath_in",
            "filename":   "breath_in.wav",
            "rms_db":     [round(v, 2) for v in info_in_ref["rms_db"]],
            "series":     build_series(rms_in, ascending=True),
            "avg_db":     round(info_in_ref["avg_db"],  2),
            "peak_db":    round(info_in_ref["peak_db"], 2),
            "first_in_s": round(info_in_ref["first_in_s"], 3),
            "last_in_s":  round(info_in_ref["last_in_s"],  3),
            "ascending":  True,
        },
        {
            "name":       "breath_out",
            "filename":   "breath_out.wav",
            "rms_db":     [round(v, 2) for v in info_out_ref["rms_db"]],
            "series":     build_series(rms_out, ascending=False),
            "avg_db":     round(info_out_ref["avg_db"],  2),
            "peak_db":    round(info_out_ref["peak_db"], 2),
            "first_in_s": round(info_out_ref["first_in_s"], 3),
            "last_in_s":  round(info_out_ref["last_in_s"],  3),
            "ascending":  False,
        },
    ]
    viz_path = os.path.join(SCRIPT_DIR, "envelope_viz.html")
    write_viz_html(viz_path, args.on_db, args.off_db, args.profile_bends, clips)
    print(f"Wrote {viz_path}")


if __name__ == "__main__":
    main()
