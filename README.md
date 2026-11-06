# Too-D-Or-Not-Too-D
Simple 2D animation thing.

TooD is a 2D image and frame-animation editor for Linux, written in C++17 with Qt 6 Widgets (QGraphicsView). You place shapes, connect them with joints (hinge, free, weld, elastic) or with stretchy lines, and build animations frame by frame. Tweening can fill in the in-between frames, and you export the result as MP4, GIF, WebM, a PNG sequence, or single images.

## Build

```sh
sudo apt install qt6-base-dev libqt6svg6-dev cmake ninja-build ffmpeg   # ffmpeg only needed for MP4/WebM
cmake -S . -B build -G Ninja
cmake --build build
./build/tood [project.tood]
```

On startup a dialog lets you create a new project (preset or custom size, background, FPS) or open an existing or recent one. Passing a `.tood` file on the command line skips the dialog.

To run the headless smoke test: `./build/tood --selftest /some/dir`

## Layout
- **Left:** tools.
- **Top:** fill/stroke/width, connection type, and toggles (snap points, ortho, sticky ends, grid, onion skin).
- **Right:** **Colours** (palette plus recent colours; left-click = fill, right-click = stroke), with **Properties** and **Library** as tabs below.
- **Bottom:** **Frames** (chapter selector, go-to-frame, thumbnail size, frame strip) and **Info**. Info shows what the hovered tool does, step-by-step prompts ("click the second point…"), and a message log.

## Tools
| Key | Tool | |
|---|---|---|
| V | Select | Click selects a whole group; Alt+click selects one member. Shift/Ctrl+click adds or removes. Drag on empty space to box-select. |
| W | Warp | Drag the corner handles to skew, taper or distort any shape, including images and text. |
| R E T P S | Rect, Ellipse, Triangle, Polygon, Star | Shift while drawing = square / circle. |
| D H Y | Diamond, Hourglass, Eye | Diamond = n-gon with every corner connected. Hourglass waist and eye iris size are set in Properties. |
| L A | Line, Arrow | Ctrl = 15° steps, Ortho (F8) = 90°. Style: straight or spring. |
| X | Text | Font, size, bold, italic and alignment are in Properties. |
| O | Point | Helper point. Visible while editing, never exported. |
| C | Connect | Click a point on shape A, then a point on shape B; B moves so the points meet. Click the same spot twice to join overlapping shapes. |
| M | Midpoint | Click two points to place a helper point halfway between them. |
| U J | Perpendicular, Parallel | Click a reference line or edge, then the start point, then the end point. |
| Q | Measure | Click two points to show their distance and angle. |
| Ctrl+I | Insert image | You can also drag image files onto the canvas. |

**Precise points:** point-picking tools snap to **corners** (□), **midpoints** (△), **centres** (⊕) and any point **on an edge** (×). Markers show these points on nearby shapes, and the cursor label names the snap. **Hold Shift** for a free, unsnapped point. Toggle with Snap points (F9).

**View:** the wheel zooms, middle-drag or Space+drag pans, and Ctrl+0 fits the canvas. Arrow keys nudge the selection (Shift = 10 px).

## Connections
- **Hinge** (orange): the child only rotates around the pin, optionally within angle limits. Moving the parent carries the child along.
- **Free** (green): the shape at the other end swings along behind when you drag.
- **Weld** (red): the two shapes are rigid, like one piece.
- **Elastic** (purple): works like a rubber band. The other shape lags behind while you drag and springs back when you let go. Stiffness is set in Properties.
- **Stretchy lines:** with *Sticky ends* on, the ends of a line or arrow drawn on (or dropped onto) shapes are glued to them. The line then stretches and turns when those shapes move. Use the spring style for a coil.

Click a joint marker to select it. Drag the marker to move the pivot. In Properties you can change the type and limits, reverse the direction (which side stays put), or delete it. Loops are not allowed: every shape has at most one parent.

## Selecting, grouping, copying
- **Ctrl+L** selects everything connected to the selection, through joints and glued lines.
- **Ctrl+G / Ctrl+Shift+G** groups and ungroups.
- Copy/paste and Ctrl+D keep the joints, glued ends and groups that lie inside the selection. Pasting into another frame keeps the shapes' identity, so tweening matches them.

## Shape options
- **Ellipse:** Radius X/Y, or "Circle" to lock them together.
- **Any shape:** Lock aspect, Stretch % (squash/stretch along an angle) and Reset warp.
- **Eye:** fill (white of the eye), stroke (outline) and a separate **pupil** colour.

## Library
Save objects for reuse with **Add Selection to Library** (Ctrl+B). Select a few connected items plus some standalone ones (Ctrl+L extends the selection to everything connected), then give them a name. Joints, glued lines, groups and images are kept. The **Library** panel shows thumbnails: double-click an item to insert it at the view centre, or drag it onto the canvas. The library is shared by all projects and stored as small `.tood` files in `~/.local/share/TooD/TooD/library/` (open the folder with the "…" button).

## Frames / animation
- **New Frame (copy)** adds a copy of the current frame for you to modify. **Duplicate ×N** adds held copies. Each frame has its own **Duration**.
- **Tween** generates in-between frames from the current frame to the next, with easing. Connected shapes follow their joint angles, so they stay attached. **Squash & stretch** stretches unconnected shapes along their direction of motion. Shapes that exist in only one of the two frames fade in or out.
- **Onion skin** (Ctrl+K) shows the previous frame faintly. **Play** previews the animation.

| Key | Action |
|---|---|
| N (or Ctrl+Shift+N) | New frame (copy of current), and switch to it |
| Shift+N | Blank frame |
| Ctrl+Shift+D | Duplicate ×N |
| Shift+Del | Delete frame(s) |
| PgUp / PgDn or , / . | Previous / next frame |
| Alt+PgUp / Alt+PgDn or Alt+, / Alt+. | Move frame left / right |
| Ctrl+T | Tween to next frame |
| Ctrl+Enter or Ctrl+Space | Play / stop |

## Chapters (large projects)
A project is split into **chapters**, and you work on one at a time (Chapter selector above the frames, Ctrl+PgUp/PgDn, Chapters menu). Every chapter has its own frame strip, so hundreds or thousands of frames stay manageable. Thumbnails are rendered gradually, visible ones first. Use the thumbnail-size slider to fit more frames on screen, and **Go to frame** to jump.
- **New Chapter** (Ctrl+Alt+N) can start with a copy of the current frame for continuity.
- **Export Chapter as Project** and **Import Chapters from Project** let you split a big project into separate files, or merge files made separately.

## Export
- **Export Animation** (Ctrl+Shift+E) offers **MP4** (H.264), WebM (VP9), GIF or a PNG sequence. Choose the whole movie (all chapters merged), the current chapter, or a frame range. GIF keeps each frame's exact duration (built-in encoder). MP4/WebM use a constant frame rate and repeat frames to match durations (needs ffmpeg).
- **Export Current Frame** (Ctrl+E) offers PNG, JPG (with a scale factor) or SVG.

Projects are saved as `.tood` files (JSON). Images are embedded once and shared by all frames. Recent projects are listed under File and in the start dialog.

## Code
| File | Contents |
|---|---|
| `src/model.*` | Document, chapters, frames, shapes, joints; rendering; file format |
| `src/solver.*` | Connection constraints, elastic springs, stretchy lines, tweening |
| `src/canvas.*` | The editing view (QGraphicsView): tools, snapping, handles |
| `src/mainwindow.*` | UI: panels, timeline, library, undo, file I/O, export |
| `src/gifwriter.*` | Animated GIF encoder (median-cut palette, LZW) |

## License
GPL-3.0, see [LICENSE](LICENSE).
