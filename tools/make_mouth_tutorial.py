#!/usr/bin/env python3
"""Build an editable TooD project that is itself a short tutorial.

The animation shows only the controls that matter for the Mouth effect:
press K, pick "Mouth (talk)", set Beats, click Animate. The last section is a
live tween, so the mouth opens/narrows/closes while a speech bubble is typed.

Run:
    python3 tools/make_mouth_tutorial.py mouth-tutorial.tood
    ./build/tood --render mouth-tutorial.tood /tmp/tood-tutorial-frames
    ffmpeg -framerate 10 -i /tmp/tood-tutorial-frames/frame_%04d.png \
        -c:v libx264 -pix_fmt yuv420p -movflags +faststart mouth-tutorial.mp4
"""

import json
import sys
from pathlib import Path


def c(r, g, b, a=255):
    return f"#{a:02x}{r:02x}{g:02x}{b:02x}"


INK = c(238, 244, 250)
DIM = c(150, 169, 188)
CARD = c(27, 38, 53)
CARD_LINE = c(61, 77, 97)
HILITE = c(255, 205, 90)
ORANGE = c(255, 154, 60)
BAR = c(70, 86, 110)
FANG = c(255, 255, 255, 0)

CARD_Y = [230, 340, 450]
BUBBLE_TEXT = "Hello there!"


def shape(sid, kind, x, y, w, h, fill=FANG, stroke=c(0, 0, 0, 0), sw=0, op=1.0, **extra):
    s = dict(id=sid, type=kind, x=x, y=y, w=w, h=h, fill=fill, stroke=stroke, sw=sw, op=op,
             rot=0, sides=5, inner=0.45)
    s.update(extra)
    return s


def label(sid, txt, x, y, w, h, size, fill=INK, bold=False, align=0, bubble=0):
    s = shape(sid, "text", x, y, w, h, fill=fill, al=align, bold=bold, bubble=bubble,
              bfill=c(255, 255, 255), font="DejaVu Sans", fs=size, it=False, text=txt)
    if bubble:
        s["tail"] = [-0.25, 1.05]
    return s


def frame(shapes):
    return {"dur": 100, "joints": [], "shapes": shapes}


def tween(count):
    return {"dur": 100, "joints": [], "shapes": [], "tweenN": count, "tweenEase": 0,
            "tweenPath": 0, "tweenSquash": 0, "tweenArc": 0}


def scene(hi_y, hi_op, phase, result=False, bubble_text="", play_x=830):
    """One complete frame. Shape ids stay stable so tweens move/animate them."""
    face = [
        shape(1, "ellipse", 330, 350, 230, 260, fill=c(142, 200, 230)),
        shape(2, "ellipse", 285, 320, 34, 42, fill=c(255, 255, 255)),
        shape(3, "ellipse", 288, 325, 15, 20, fill=c(35, 45, 60)),
        shape(4, "ellipse", 375, 320, 34, 42, fill=c(255, 255, 255)),
        shape(5, "ellipse", 378, 325, 15, 20, fill=c(35, 45, 60)),
        # The star of the show: Mouth (talk) = preset 7.
        shape(7, "effect", 330, 420, 112, 66, fill=c(105, 34, 48),
              stroke=c(52, 30, 40), sw=3, fx=7, fxt=phase, fxs=147, fxd=1,
              mb=5, inner=0.45, acc=c(238, 112, 138)),
    ]

    cards = [label(9, "1   Press K  —  Effect tool", 900, CARD_Y[0], 600, 64, 26, INK, align=1),
             label(11, "2   Pick  “Mouth (talk)”   •   Beats: 5", 900, CARD_Y[1], 600, 64, 25, INK, align=1),
             label(13, "3   Click  ▶ Animate", 900, CARD_Y[2], 600, 64, 26, INK, align=1)]
    back = [shape(8, "rect", 900, CARD_Y[0], 640, 84, fill=CARD, stroke=CARD_LINE, sw=2),
            cards[0],
            shape(10, "rect", 900, CARD_Y[1], 640, 84, fill=CARD, stroke=CARD_LINE, sw=2),
            cards[1],
            shape(12, "rect", 900, CARD_Y[2], 640, 84, fill=CARD, stroke=CARD_LINE, sw=2),
            cards[2],
            shape(16, "rect", 900, hi_y, 660, 100, fill=FANG, stroke=HILITE, sw=4, op=hi_op)]

    header = [label(20, "Talking mouth in 3 steps", 640, 60, 1100, 70, 46, INK, bold=True),
              label(21, "TooD  •  one effect, one live tween", 640, 116, 900, 40, 23, DIM)]

    if not result:
        return header + face + back

    n = len(bubble_text)
    bw = max(150, 90 + n * 17)
    bh = 92
    tail = [
        shape(30, "line", 1040, 620, 420, 0, stroke=BAR, sw=8),
        shape(31, "line", 1040, 620, 404, 0, stroke=ORANGE, sw=6),
        shape(32, "ellipse", 830, 620, 16, 16, fill=HILITE),
        shape(33, "ellipse", 1040, 620, 16, 16, fill=HILITE),
        shape(34, "ellipse", 1250, 620, 16, 16, fill=HILITE),
        shape(35, "rect", play_x, 620, 10, 38, fill=c(255, 255, 255)),
        label(36, "0%", 830, 658, 80, 34, 18, DIM),
        label(37, "100%", 1250, 658, 100, 34, 18, DIM),
        label(38, "The tween types the text and moves the mouth", 640, 168, 900, 36, 21, HILITE),
        label(22, bubble_text, 600, 300, bw, bh, 30, c(30, 34, 44), bold=True, bubble=1),
    ]
    return header + face + back + tail


def build():
    frames = []

    def hold(n, **kw):
        for _ in range(n):
            frames.append(frame(scene(**kw)))

    start = dict(hi_y=CARD_Y[0], hi_op=0, phase=0)
    hold(12, **start)                                   # title card
    frames.append(tween(6))                             # highlight appears
    hold(18, hi_y=CARD_Y[0], hi_op=1, phase=0)          # step 1
    frames.append(tween(6))                             # highlight moves down
    hold(18, hi_y=CARD_Y[1], hi_op=1, phase=0)          # step 2
    frames.append(tween(6))                             # highlight moves down
    hold(16, hi_y=CARD_Y[2], hi_op=1, phase=0)          # step 3
    hold(1, hi_y=CARD_Y[2], hi_op=1, phase=0, result=True, bubble_text="", play_x=830)
    frames.append(tween(28))                            # Phase 0→1, letters type, playhead moves
    end = dict(hi_y=CARD_Y[2], hi_op=1, phase=1, result=True, bubble_text=BUBBLE_TEXT, play_x=1250)
    hold(1, **end)
    hold(12, **end)                                     # hold the finished result

    return {
        "format": "tood", "version": 2, "width": 1280, "height": 720, "fps": 12,
        "bg": c(17, 26, 38), "images": {}, "nextId": 100, "activeChapter": 0,
        "chapters": [{"name": "Mouth tutorial", "frames": frames}],
    }


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "mouth-tutorial.tood")
    out.write_text(json.dumps(build(), ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"Wrote {out} ({len(json.loads(out.read_text())['chapters'][0]['frames'])} frames)")
