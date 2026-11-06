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
| F | Freehand | Drag to draw; the stroke is simplified and smoothed. Ending near the start closes it. The tool stays active for more strokes. |
| B Z | Polyline, Curve | Click points: straight segments, or a smooth curve through them. Double-click or Enter finishes; click the first point to close; Backspace removes the last point; Esc cancels. |
| X | Text | Font, size, bold, italic and alignment are in Properties. |
| G | Speech bubble | Click to place text in a bubble and type it. Drag the yellow handle to point the tail. Style (speech, thought, shout, box) and colours in Properties; any text can get a bubble there. |
| O | Point | Helper point. Visible while editing, never exported. |
| C | Connect | Click a point on shape A, then a point on shape B; B moves so the points meet. Click the same spot twice to join overlapping shapes. |
| I | Pin | Click where shapes overlap to connect them at that exact point (nothing moves). "Pin joins" in the toolbar: the **top two** shapes, or **all** shapes under the spot (Ctrl+click = the other mode). Uses the "Connect as" type; the lower shape is the parent. Handy after drawing, e.g. a circle on top of a rectangle. |
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
- **Ctrl+G / Ctrl+Shift+G** groups and ungroups (also toolbar buttons and the canvas right-click menu). A group acts as one entity: clicking any member selects all of it, and you can save it to the library.
- **Right-click** on the canvas: cut/copy/duplicate/delete, group, select connected, add to library, order, remove connections.
- Copy/paste and Ctrl+D keep the joints, glued ends and groups that lie inside the selection. Pasting into another frame keeps the shapes' identity, so tweening matches them.

## Shape options
- **Ellipse:** Radius X/Y, or "Circle" to lock them together.
- **Any shape:** Lock aspect, Stretch % (squash/stretch along an angle) and Reset warp.
- **Eye:** fill (white of the eye), stroke (outline) and a separate **pupil** colour.
- **Path** (freehand, polyline, curve): drag its points with the Select tool to reshape it. *Closed* (closed paths can be filled) and *Straight / Smooth* are in Properties. A tween between two keyframes morphs the shape when both have the same number of points.

## Library
Save objects for reuse with **Add Selection to Library** (Ctrl+B). Select a few connected items plus some standalone ones (Ctrl+L extends the selection to everything connected), then give them a name. Joints, glued lines, groups and images are kept. The **Library** panel shows thumbnails: double-click an item to insert it at the view centre, or drag it onto the canvas. The library is shared by all projects and stored as small `.tood` files in `~/.local/share/TooD/TooD/library/` (open the folder with the "…" button).

## Frames / animation
- **New Frame (copy)** adds a copy of the current frame for you to modify. **Duplicate ×N** adds held copies. Each frame has its own **Duration**.
- **Tween** (Ctrl+T) generates in-between frames from the current keyframe to the next. Connected shapes follow their joint angles, so they stay attached. Shapes that exist in only one of the two frames fade in or out.
  - **Dynamic** (default): inserts one live tween item (◆, orange) in the frame strip. Its in-betweens are recalculated from the keyframes before and after it, so editing those keyframes, or inserting new ones, updates it immediately. Select it to change its settings, or **Bake** it into normal frames. On the canvas it shows its middle frame, read-only.
  - **Static**: creates normal frames right away, which you can then edit individually.
  - **Easing:** linear, ease in-out (accelerate, then decelerate), ease in, ease out, strong in-out, overshoot, bounce.
  - **Path:** straight; *arc* (curves sideways by a set height); *thrown* (gravity parabola with constant sideways speed, like a basketball); or *smooth through keyframes* (a spline). For a throw with exact control, add a keyframe at the top of the arc and give both tweens the smooth path: together they form one smooth curve.
  - **Squash & stretch** stretches unconnected shapes along their direction of motion.
  - **Motion paths** (Ctrl+M): on a tween, or on a keyframe next to one, a dashed path with dots and faint outlines shows where the shapes go.
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

Projects are saved as `.tood` files: **gzip-compressed JSON**. Images are embedded once (as the original file bytes, so a JPEG stays a JPEG) and shared by all frames, so using an image in many frames costs almost nothing. Images much larger than the canvas are scaled to 2× the canvas size when inserted. **File > Optimize Images** shrinks each image to 2× the largest size it is shown at in the project; Undo restores the originals. Recent projects are listed under File and in the start dialog.

The files can be inspected and edited with standard tools:
```sh
zcat project.tood | jq . | less          # view
zcat project.tood > project.json         # decompress, edit by hand…
gzip -c project.json > project.tood      # …and compress again
```
Plain (uncompressed) JSON `.tood` files also open, so files from older versions and hand-edited ones still work. They are saved compressed the next time.

## Logs and crashes
- **Crash log (always on):** if TooD crashes, it appends the backtrace and the last 64 operations to `~/.local/share/TooD/TooD/crash.log` (also printed to the terminal). The backtrace shows function names; resolve `+0x…` offsets to source lines with `addr2line -f -C -e build/tood 0x…`.
- **Session log (off by default):** `./build/tood --log-session` records every operation of the session (edits, undo/redo, frame/chapter/tool/selection changes, file and export actions, messages and Qt warnings) to `~/.local/share/TooD/TooD/session-<date>.log`. Use `--log-session=FILE` to choose the file. Each line is written immediately, so the log is complete even after a crash.
- `./build/tood --help` lists the options.

## Code
| File | Contents |
|---|---|
| `src/model.*` | Document, chapters, frames, shapes, joints; rendering; file format |
| `src/solver.*` | Connection constraints, elastic springs, stretchy lines, tweening |
| `src/canvas.*` | The editing view (QGraphicsView): tools, snapping, handles |
| `src/mainwindow.*` | UI: panels, timeline, library, undo, file I/O, export |
| `src/gifwriter.*` | Animated GIF encoder (median-cut palette, LZW) |
| `src/oplog.*` | Operation log, session log and crash handler |

## License
GPL-3.0, see [LICENSE](LICENSE).
