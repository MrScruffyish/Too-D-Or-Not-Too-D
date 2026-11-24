#!/usr/bin/env python3
"""Build a full TooD tutorial as an editable TooD project.

Seven chapters, each a short scene made only from TooD shapes: welcome, drawing,
text & bubbles, joints, frames & tweens, effects, and chapters & export. The
on-screen cards show just the controls that matter for each step.

    python3 tools/make_tutorial.py tutorial.tood
    ./build/tood --render tutorial.tood /tmp/tood-full-tutorial
    ffmpeg -framerate 10 -i /tmp/tood-full-tutorial/frame_%04d.png \
        -c:v libx264 -pix_fmt yuv420p -movflags +faststart tutorial.mp4
"""

import json
import math
import sys
from pathlib import Path

INK = "#fff0f5fb"
DIM = "#ff9aadc4"
CARD = "#ff1b2635"
LINE = "#ff3d4d61"
YELLOW = "#ffffcd5a"
ORANGE = "#ffff9a3c"
BAR = "#ff46566e"
CLEAR = "#00ffffff"
BG = "#ff111a26"


def shape(i, kind, x, y, w, h, fill=CLEAR, stroke=CLEAR, sw=0, op=1.0, **extra):
    s = dict(id=i, type=kind, x=x, y=y, w=w, h=h, fill=fill, stroke=stroke, sw=sw,
             op=op, rot=0, sides=5, inner=0.45)
    s.update(extra)
    return s


def label(i, text, x, y, w, h, size, fill=INK, bold=False, align=0, bubble=False):
    s = shape(i, "text", x, y, w, h, fill=fill, al=align, bold=bold, bubble=int(bubble),
              bfill="#ffffffff", font="DejaVu Sans", fs=size, it=False, text=text)
    if bubble:
        s["tail"] = [-0.25, 1.05]
    return s


def effect(i, preset, x, y, w, h, phase, seed=101, beats=5, spread=0.45, op=1.0):
    fills = ["#ffff8a1e", "#ffff5a00", "#ffb9bec8", "#ff4fc3ff", "#ffff5e8a", "#ff9b7bff", "#ffffaa28", "#ff6b2430"]
    accents = ["#ffe0d07a", "#ffd83d", "#ffe8eaee", "#ffffffff", "#ffd54f", "#fff3a0", "#fffffa d2".replace(" ", ""), "#ffee708a"]
    return shape(i, "effect", x, y, w, h, fill=fills[preset], stroke="#ff301f28", sw=3 if preset == 7 else 0,
                 op=op, fx=preset, fxt=phase, fxs=seed, fxd=1, mb=beats, inner=spread,
                 acc=accents[preset])


def frame(shapes):
    return {"dur": 100, "joints": [], "shapes": shapes}


def tween(count, path=0, arc=0):
    return {"dur": 100, "joints": [], "shapes": [], "tweenN": count, "tweenEase": 0,
            "tweenPath": path, "tweenSquash": 0, "tweenArc": arc}


def text_tween(shapes, shape_id=107, rate=1.0, linger=14, mouth=105, beats=0):
    """A first-class text tween frame: stores the full line, generates typing."""
    f = frame(shapes)
    f["tt"] = {"on": True, "shape": shape_id, "rate": rate, "linger": linger,
               "mouth": mouth, "beats": beats}
    return f


def chrome(chapter_no, total, title, sub, lines, hi=0, timeline=None):
    """Header, progress dots, control card, optional mini timeline."""
    out = [label(1, title, 620, 52, 1160, 60, 42, INK, bold=True, align=1),
           label(2, sub, 620, 108, 1160, 34, 21, DIM, align=1)]
    for k in range(total):
        out.append(shape(3 + k, "ellipse", 1040 + (k - (total - 1) / 2) * 34, 152, 14, 14,
                         fill=YELLOW if k == chapter_no - 1 else BAR))
    out.append(shape(10, "rect", 940, 230, 620, 60 + 66 * len(lines), fill=CARD, stroke=LINE, sw=2))
    for k, txt in enumerate(lines):
        out.append(label(11 + k, txt, 940, 270 + 66 * k, 570, 52, 24,
                         YELLOW if k == hi else INK, bold=(k == hi), align=1))
    out.append(shape(19, "rect", 940, 270 + 66 * hi, 600, 56, fill=CLEAR, stroke=YELLOW, sw=4,
                     op=1 if hi >= 0 else 0))
    if timeline is not None:
        p = max(0.0, min(1.0, timeline))
        x0, x1 = 250, 850
        out += [shape(20, "line", (x0 + x1) / 2, 640, x1 - x0, 0, stroke=BAR, sw=8),
                shape(21, "line", (x0 + x1) / 2, 640, (x1 - x0) - 16, 0, stroke=ORANGE, sw=6),
                shape(22, "ellipse", x0, 640, 16, 16, fill=YELLOW),
                shape(23, "ellipse", x1, 640, 16, 16, fill=YELLOW),
                shape(24, "rect", x0 + (x1 - x0) * p, 640, 10, 36, fill="#ffffffff"),
                label(25, "keyframe", x0, 678, 140, 30, 17, DIM, align=1),
                label(26, "keyframe", x1, 678, 140, 30, 17, DIM, align=1),
                label(27, "live tween", (x0 + x1) / 2, 678, 200, 30, 17, ORANGE, align=1)]
    return out


def face(b, phase=0, op=1.0):
    """A small character drawn from ordinary shapes."""
    return [shape(b + 0, "ellipse", 330, 330, 200, 220, fill="#ff8ecae6", op=op),
            shape(b + 1, "ellipse", 288, 305, 30, 38, fill="#ffffffff", op=op),
            shape(b + 2, "ellipse", 291, 310, 13, 18, fill="#ff232d3c", op=op),
            shape(b + 3, "ellipse", 372, 305, 30, 38, fill="#ffffffff", op=op),
            shape(b + 4, "ellipse", 375, 310, 13, 18, fill="#ff232d3c", op=op),
            effect(b + 5, 7, 330, 393, 100, 60, phase, seed=77, beats=5, op=op)]


# ---------------------------------------------------------------- chapter 1
def welcome(hi=0, reveal=0):
    f = face(100, op=1.0)
    ops = [1 if reveal >= k else 0 for k in range(1, 6)]
    # reveal is 0..5; the face fades in piece by piece
    for k, s in enumerate(f):
        s["op"] = ops[min(k, 4)] if reveal else (1 if k == 0 else 0)
    out = chrome(1, 7, "Welcome to TooD", "A 2D animation editor. Draw it, connect it, tween it, export it.",
                 ["V   Select things", "N   Add a frame", "Ctrl+Enter   Play"], hi)
    out += f
    out += [label(60, "TOOLS  left", 250, 240, 300, 40, 20, DIM, align=1),
            label(61, "FRAMES  bottom", 250, 530, 300, 40, 20, DIM, align=1),
            label(62, "PROPERTIES  right", 800, 200, 250, 36, 19, DIM, align=1)]
    return out


# ---------------------------------------------------------------- chapter 2
def drawing(grow=1.0, squash=1.0, hi=0):
    g = max(0.15, grow)
    out = chrome(2, 7, "Draw and reshape", "Shapes, text and freehand strokes live on the canvas.",
                 ["R E T P S   Draw shapes", "V   Select  (Alt = one part)", "W   Warp and squash"], hi,
                 timeline=grow)
    out += [shape(100, "ellipse", 330, 300, 70 + 110 * g, 70 + 120 * g, fill="#ff8ecae6", squash=squash),
            shape(101, "rect", 330, 450, 50 + 90 * g, 60 + 130 * g, fill="#ff5b8dd9"),
            shape(102, "ellipse", 285, 290, 12 + 18 * g, 16 + 22 * g, fill="#ff232d3c"),
            shape(103, "ellipse", 375, 290, 12 + 18 * g, 16 + 22 * g, fill="#ff232d3c"),
            label(110, "Drag the corner handles with Warp", 330, 570, 520, 40, 20, DIM, align=1)]
    return out


# ---------------------------------------------------------------- chapter 3
def words(txt, hi=0, timeline=None):
    w = max(210, 110 + len(txt) * 20)
    out = chrome(3, 7, "Text and speech bubbles", "A text tween types the letters, lingers, then cuts hard to the next line.",
                 ["X   Text", "G   Speech bubble", "Ctrl+Shift+T   Text tween"], hi, timeline)
    out += face(100)
    out += [label(107, txt, 590, 430, w, 96, 32, "#ff1e1e28", bold=True, bubble=True)]
    return out


# ---------------------------------------------------------------- chapter 4
def joints(angle, hi=0):
    a = math.radians(angle)
    sx, sy = 330, 380
    L = 150
    hx, hy = sx + L * math.cos(a), sy + L * math.sin(a)
    out = chrome(4, 7, "Joints and connections", "Pieces stay attached while they move.",
                 ["C   Connect two points", "I   Pin at an overlap", "Hinge / Free / Weld / Elastic"], hi)
    out += [shape(100, "rect", 330, 430, 120, 180, fill="#ff5b8dd9"),
            shape(101, "rect", (sx + hx) / 2, (sy + hy) / 2, L, 24, fill="#ffdc4c4c", rot=angle),
            shape(102, "ellipse", hx, hy, 46, 46, fill="#fff2d23f"),
            shape(103, "ellipse", sx, sy, 16, 16, fill=YELLOW),
            label(110, "pin", sx, sy - 36, 80, 28, 17, YELLOW, align=1),
            label(111, f"hinge angle  {angle:+.0f} deg", 330, 570, 420, 36, 20, DIM, align=1)]
    for k in range(6):
        t = (k + 0.5) / 6
        out.append(shape(120 + k, "ellipse", sx + (hx - sx) * t, sy + (hy - sy) * t, 5, 5, fill=YELLOW))
    return out


# ---------------------------------------------------------------- chapter 5
def moving(x, y, hi=0, play=0.0):
    out = chrome(5, 7, "Frames and tweening", "Two keyframes, one live tween. TooD fills the in-betweens.",
                 ["N   New frame", "Ctrl+T   Tween to next", "Easing / path / squash"], hi, play)
    out += [shape(100, "line", 330, 540, 700, 0, stroke=BAR, sw=6),
            shape(101, "ellipse", x, y, 54, 54, fill="#ffff9a3c"),
            shape(102, "rect", 660, 500, 10, 80, fill=YELLOW),
            shape(103, "triangle", 690, 470, 46, 46, fill=YELLOW),
            label(110, "arc path", 470, 250, 200, 34, 19, DIM, align=1)]
    return out


# ---------------------------------------------------------------- chapter 6
def effects(phase, hi=0, play=0.0):
    out = chrome(6, 7, "Effects", "Procedural effects are shapes with a Time value. Animate makes them play.",
                 ["K   Effect tool", "Animate  ->  keyframes + tween", "Time / Phase, seed, density"], hi, play)
    out += [effect(100, 1, 240, 380, 170, 240, phase, seed=12),
            effect(101, 0, 470, 400, 180, 180, phase, seed=34),
            effect(102, 7, 330, 240, 110, 64, phase, seed=77),
            label(110, "Fire (loops)", 240, 550, 220, 34, 19, DIM, align=1),
            label(111, "Explosion", 470, 550, 220, 34, 19, DIM, align=1),
            label(112, "Mouth", 330, 190, 220, 34, 19, DIM, align=1)]
    return out


# ---------------------------------------------------------------- chapter 7
def finish(progress=0.0, hi=0, wave=0):
    a = math.radians(-20 + wave * 40)
    sx, sy = 250, 400
    hx, hy = sx + 130 * math.cos(a), sy + 130 * math.sin(a)
    out = chrome(7, 7, "Chapters and export", "Split big films into chapters, then export the whole movie.",
                 ["Ctrl+Alt+N   New chapter", "Ctrl+Shift+E   Export", "MP4 / WebM / GIF / PNG / SVG"], hi)
    out += [shape(100, "rect", 230, 330, 110, 160, fill="#ff5b8dd9"),
            shape(101, "rect", (sx + hx) / 2, (sy + hy) / 2, 130, 20, fill="#ffdc4c4c", rot=a),
            shape(102, "ellipse", 250, 235, 150, 160, fill="#ff8ecae6"),
            effect(103, 7, 250, 280, 90, 54, (wave % 1.0), seed=77),
            label(110, "Chapters", 250, 560, 260, 34, 19, DIM, align=1)]
    for k, name in enumerate(["INTRO", "SCENE 2", "OUTRO"]):
        out.append(shape(120 + k, "rect", 220 + 160 * k, 620, 140, 46,
                         fill=CARD if k != hi else "#ff3a4d66",
                         stroke=YELLOW if k == hi else LINE, sw=3))
        out.append(label(130 + k, name, 220 + 160 * k, 620, 140, 46, 16,
                         YELLOW if k == hi else DIM, bold=(k == hi), align=1))
    out += [shape(140, "rect", 220, 690, 430 * progress, 12, fill=ORANGE),
            label(141, "EXPORT", 640, 690, 200, 34, 19, YELLOW, align=1)]
    return out


def build():
    chapters = []

    # 1 — Welcome: the character fades in piece by piece.
    fr = []
    for _ in range(10):
        fr.append(frame(welcome(0, 0)))
    fr.append(tween(8))
    for _ in range(8):
        fr.append(frame(welcome(0, 2)))
    fr.append(tween(8))
    for _ in range(10):
        fr.append(frame(welcome(1, 5)))
    fr.append(tween(8))
    for _ in range(14):
        fr.append(frame(welcome(2, 5)))
    chapters.append(("01 Welcome", fr))

    # 2 — Drawing: shapes grow, then squash.
    fr = []
    for _ in range(10):
        fr.append(frame(drawing(0.3, 1.0, 0)))
    fr.append(tween(16))
    for _ in range(12):
        fr.append(frame(drawing(1.0, 1.0, 1)))
    fr.append(tween(10))
    for _ in range(10):
        fr.append(frame(drawing(1.0, 1.28, 2)))
    fr.append(tween(10))
    for _ in range(12):
        fr.append(frame(drawing(1.0, 1.0, 2)))
    chapters.append(("02 Draw", fr))

    # 3 — Text: a first-class text tween types the line, then a hard cut to the next.
    fr = []
    for _ in range(12):
        fr.append(frame(words("Hi!", 0, 0)))
    for _ in range(6):
        fr.append(frame(words("Hi!", 1, 0)))
    fr.append(text_tween(words("Hello world!", 2, 0), rate=1.0, linger=14))
    for _ in range(16):
        fr.append(frame(words("Bye!", 2, 1)))
    chapters.append(("03 Text", fr))

    # 4 — Joints: explicit frames sweep the arm back and forth.
    fr = []
    for k in range(26):
        a = -40 + 80 * (k / 25.0)
        fr.append(frame(joints(a, 0)))
    for k in range(26):
        a = 40 - 80 * (k / 25.0)
        fr.append(frame(joints(a, 1)))
    for _ in range(12):
        fr.append(frame(joints(-20, 2)))
    chapters.append(("04 Joints", fr))

    # 5 — Frames & tween: a real arc tween with a moving playhead.
    fr = []
    for _ in range(12):
        fr.append(frame(moving(180, 510, 0, 0)))
    fr.append(tween(30, path=1, arc=150))
    for _ in range(14):
        fr.append(frame(moving(660, 510, 2, 1)))
    fr.append(tween(8))
    for _ in range(10):
        fr.append(frame(moving(660, 510, 2, 1)))
    chapters.append(("05 Tween", fr))

    # 6 — Effects: Phase 0 -> 100% plays all three.
    fr = []
    for _ in range(10):
        fr.append(frame(effects(0.0, 0, 0)))
    fr.append(tween(30))
    for _ in range(16):
        fr.append(frame(effects(1.0, 1, 1)))
    fr.append(tween(8))
    for _ in range(10):
        fr.append(frame(effects(1.0, 2, 1)))
    chapters.append(("06 Effects", fr))

    # 7 — Chapters & export.
    fr = []
    for k in range(14):
        fr.append(frame(finish(0.0, 0, k / 14.0)))
    for k in range(14):
        fr.append(frame(finish(0.1 + 0.45 * k / 13.0, 1, 1 + k / 14.0)))
    for k in range(20):
        fr.append(frame(finish(0.55 + 0.45 * k / 19.0, 2, 2 + k / 14.0)))
    for k in range(14):
        fr.append(frame(finish(1.0, 2, 3 + k / 14.0)))
    chapters.append(("07 Export", fr))

    return {
        "format": "tood", "version": 2, "width": 1280, "height": 720, "fps": 12,
        "bg": BG, "images": {}, "nextId": 1000, "activeChapter": 0,
        "chapters": [{"name": name, "frames": fs} for name, fs in chapters],
    }


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "tutorial.tood")
    doc = build()
    out.write_text(json.dumps(doc, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"Wrote {out}: {len(doc['chapters'])} chapters, "
          f"{sum(len(c['frames']) for c in doc['chapters'])} keyframes")
