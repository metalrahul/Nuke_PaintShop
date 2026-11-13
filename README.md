# RasterPaint for Nuke

A raster paint node. Strokes are baked into pixels as you draw, the way Photoshop
does it, instead of being stored and re-rendered like RotoPaint. A shot with ten
thousand strokes renders as fast as one with ten.

- Paint, erase and clone stamp, cloning across time or from a second input
- Round brush (size, hardness) or sampled tips loaded from Photoshop `.abr` files
- Layers, each with opacity, a blend mode and visibility
- Photoshop-style brush dynamics: shape, scattering and transfer
- Opacity and flow that behave like Photoshop's, plus spacing and tablet pressure
- A viewer toolbar, a brush picker with thumbnails, and a live stroke preview
- Undo and redo through Nuke's own Ctrl+Z
- One layer for all frames, or a separate layer per frame

## Requires a commercial Nuke licence

NDK plugins do not load in **Nuke Indie** or **Nuke Non-commercial**. Foundry's flavour
comparison lists "NDK / C++ SDK Support" and "Third Party / Custom C++ node / plug-in
support" as *No* for both, and the Indie FAQ states that "Custom C++ Compiled nodes will
not work or load correctly in Nuke Indie." The licence decides this, not the installed
build, so the same install running under a commercial licence is fine.

| Licence | Can load this plugin | Custom OFX plugins |
| --- | --- | --- |
| Nuke / NukeX / Nuke Studio (commercial) | Yes | Yes |
| Nuke Indie | No | Yes |
| Nuke Non-commercial | No | No |

To run it on Indie it would have to be rewritten as an **OFX** plugin. `src/core` has no
Nuke dependency and would carry over unchanged; only `src/nuke/RasterPaint.cpp` would be
replaced. The open question to settle first is whether Nuke's OFX host implements viewer
interacts (pen events and overlay drawing) — OFX specifies them, but Nuke's support for
them was not confirmed. Without that there is no in-viewer painting on Indie.

## Status: read this first

| Part | State |
| --- | --- |
| `src/core` (canvas, brush engine, `.abr` reader, undo, file format) | Compiled and tested here: unit tests pass, clean under AddressSanitizer and UBSan |
| `src/nuke/RasterPaint.cpp` (the Nuke node) | **Not compiled against the real NDK and never run in Nuke.** No Nuke install was available. The NDK calls it uses were checked against Foundry's NDK reference, and the file passes a syntax check against stand-in headers |

Expect to fix a few things on the first real build. The places most likely to
need attention are listed under "What to check first in Nuke".

## Build

Needs CMake 3.16+ and the compiler Foundry lists for your Nuke version, because NDK
plugins are ABI-sensitive: **Visual Studio 2022** for Nuke 16 and 17.

`NUKE_ROOT` must be the exact install folder, patch number included — a path that does
not exist is the usual cause of a `could not find DDImage` error, so check the folder
name first:

```
cmake -B build -DNUKE_ROOT="C:/Program Files/Nuke17.0v3"
cmake --build build --config Release
ctest --test-dir build        # core tests, no Nuke needed
```

On macOS point it at `<...>/NukeXX.YvZ.app/Contents/MacOS`. Without `-DNUKE_ROOT` only
the core tests are built.

Copy the built `RasterPaint.dll` (in `build/Release` on Windows) or `RasterPaint.so` /
`.dylib` and `python/menu.py` into your `.nuke` folder, or any folder on `NUKE_PATH`.
The node appears under Draw > RasterPaint.

Options, all optional:

- `-DRP_DDIMAGE=<path>` if the DDImage library is somewhere the build does not look.
- `-DRP_USE_NUKE_CMAKE=ON` to link through Nuke's own `NukeConfig.cmake`. Off by
  default because several Nuke releases ship that file in the install root instead of
  `cmake/`, which makes it compute the wrong import prefix (Foundry issue 502250).
- `-DRP_OLD_ABI=ON` for Nuke 14 and older on Linux (old libstdc++ string ABI).

## Use

Open the node's control panel and paint in the 2D viewer.

| Action | Result |
| --- | --- |
| Drag | Paint with the current tool |
| Ctrl+click | Set the clone source. The next clone stroke fixes the offset |
| Shift+drag left/right | Resize the brush |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo strokes (also the Undo / Redo buttons) |
| `Alt+B` / `Alt+E` / `Alt+S` | Brush / eraser / clone stamp |
| `Alt+C` | Pick the colour under the cursor |
| `Alt+M` | Toggle the brush menu |
| `Alt+L` | Toggle the layer list |
| `Alt+[` / `Alt+]` | Shrink / grow the brush |

Drag the grip in the brush menu's bottom-right corner to make the grid bigger or
smaller, up to 24 by 24 cells.

Every shortcut uses Alt deliberately. Nuke's 2D viewer already binds the bare letters
this node would otherwise want - `p` pauses the viewer, `b` shows the blue channel, `l`
plays forward, `i` marks an in point, and `[` and `]` toggle the toolbars - so taking
them would break the viewer. Alt is nearly free: Nuke uses only Alt+G, I, O, P, U, W
and Z, none of which are used here.

The eyedropper button on the toolbar arms a pick: the next click samples a colour and
disarms it. Either way the colour comes from what you can see - the input with every
visible layer composited on top - and the current colour shows as a chip on that button.
Alt is left alone, since the viewer uses it to pan.

The toolbar down the left of the viewer does the same things, and stays a constant size
as you zoom. The grid button opens a picker showing a thumbnail of every tip in the
loaded `.abr`; click one to select it. Bottom left is a live swatch of the current
brush, rendered by the paint engine itself rather than approximated, so it always shows
what a stroke will actually lay down.

The NDK has no API for adding a native viewer toolbar, so this one is drawn in OpenGL
and hit-tested by the plugin. Clicking it never paints.

To use Photoshop brushes, pick an `.abr` in **brush file**. Its brushes appear in the
**brush** menu after `round`, under their real names.

With **use brush file settings** on (the default), picking one also takes its size,
spacing, hardness, angle, roundness and dynamics from the file, so a brush behaves as it
does in Photoshop rather than just supplying a shape. Turn it off to keep your own
settings and use the file for tip shapes only.

**opacity** caps how opaque one stroke can get, however much it overlaps itself.
**flow** is how quickly each dab builds towards that cap.

## Layers

Paint goes onto the selected layer; the stack composites bottom to top over the node's
input. The **Layers** tab holds the stack: name, opacity, blend mode, visibility, lock,
and buttons to add, duplicate, delete, reorder, merge down and flatten. The layer
selector is also on the main tab, and the viewer shows the stack top right - click a row
to select it, click its box to toggle visibility, `Alt+L` to hide the list.

Fourteen blend modes: normal, multiply, screen, overlay, darken, lighten, colour dodge,
colour burn, hard light, soft light, difference, exclusion, plus and minus. Normal,
multiply, screen, plus, minus, difference, darken and lighten behave sensibly on
over-range (HDR) values; the rest are defined against a 0..1 range and will do odd
things above 1, which is inherent to the formulas rather than a bug.

Adding, deleting, reordering and merging are undoable with Ctrl+Z, the same as strokes.
A node always keeps at least one layer. **Merge down** covers every frame the two layers
hold paint on, not only the frame on screen.

Each layer keeps its own sidecar files, named by a layer id that never changes, so
reordering never rewrites anything:

```
<paint dir>/paint.L01.rpt        layer id 1, all frames
<paint dir>/paint.L02.0101.rpt   layer id 2, frame 101
```

The stack's metadata - names, opacity, blend, visibility - lives in a hidden knob in the
`.nk`, so it travels with the script while the pixels stay beside it. Paint written
before layers existed loads into the bottom layer. Files belonging to deleted layers are
removed when you next save.

Clone samples the input with every *other* visible layer composited onto it, frozen for
the stroke. The active layer's own blend mode is not applied to the clone source.

**brush blend** sets how a stroke combines with what is already on its layer - multiply
to shade, screen to lift, and so on. That is separate from the layer's own blend mode,
which controls how the whole layer sits over everything below; the two stack. Erase
ignores it. On an empty part of a layer every mode gives the plain colour, since
blending against nothing is just the source.

## Brush presets

**Save preset** stores every brush and dynamics setting under a name, along with the
loaded `.abr` and the selected tip. Picking one from the menu applies it. Presets live
in `~/.nuke/RasterPaint.presets` (`%USERPROFILE%\.nuke\...` on Windows), so they are
shared by every script rather than trapped in one node, and the file is written through
a temp file so a failure cannot truncate it.

**Reset brush** puts every brush and dynamics setting back to its default. Colour, the
loaded brush file and the layer stack are left alone.

## Cloning

**clone from** picks which image the clone tool copies: `A` is this node's input, `B` is
the second input, so a cleanplate or another element can be painted in. With nothing
connected to B it falls back to A.

**time offset** shifts the clone source in time - `-1` clones from the previous frame,
which is how a rig gets painted out using a frame where it isn't there. Clone pixels are
baked into the canvas as you paint rather than re-rendered, so the offset is read once
at stroke time and the result does not change if the source does later.

Either way the source is the chosen image with every *other* visible layer composited
onto it, frozen for the stroke, with this layer's pre-stroke pixels on top - which is
what stops a clone stroke smearing its own output.

## Brush dynamics

On the Dynamics tab, following Photoshop's model: each property has a **jitter** (a
random reduction), a **control** (off, fade over N dabs, or pen pressure) and a
**minimum** that floors the result.

- **Brush tip shape** - angle and roundness, flip x/y.
- **Shape dynamics** - size, angle and roundness jitter. Angle can also be set to
  `direction`, which turns the tip to follow the stroke; with a flat tip that gives a
  calligraphic nib.
- **Scattering** - throw each dab off the stroke by up to N brush diameters, across the
  stroke or both axes, with `count` dabs per spacing step.
- **Transfer** - opacity and flow jitter.

Every stroke replays the same random sequence from the **random seed**, so a stroke you
like is reproducible; change the seed for a different pattern. A size fade that reaches
zero stops the stroke rather than trailing sub-pixel specks.

Opacity jitter can only ever lighten a dab, never darken what is already down: the
engine keeps the highest alpha reached at each pixel, so a faint dab landing on a solid
one leaves it alone.

## How it stays fast

- **Sparse tiles.** The canvas is 128x128 float RGBA tiles, allocated only where
  there is paint. Tiles that are erased back to empty are freed.
- **Rendering is a lookup.** `engine()` passes untouched rows straight through
  without copying. Painted rows are one "over" of the tile on the input.
- **No stroke replay.** Opacity is handled by blending each dab against a snapshot of
  the tile from before the stroke, with a per-stroke coverage mask. The same snapshots
  are the undo history, so undo costs no extra work while painting.
- **Brush tips are mip-mapped** once when the `.abr` loads, so a 2500 px tip painted
  at 40 px is still one bilinear lookup per pixel.
- **Clone reads the input once per tile** per stroke, not once per dab.

Measured on the core alone, single thread, 2-core cloud machine (your workstation
will be faster). Each "mouse move" is about 10 px of travel:

| Brush | Time per mouse move |
| --- | --- |
| Round 50 px, 10% spacing | 0.13 ms |
| Round 300 px, 10% spacing | 0.49 ms |
| Sampled 1024 px tip at 300 px | 0.9 ms |
| Clone 300 px | 0.67 ms |

These numbers cover stamping only. In Nuke, the viewer refresh after each move also
re-evaluates whatever is downstream of the node, and that will dominate.

## Proxy and downres

The canvas always holds **full-resolution** pixels, whatever scale the viewer is at. On
the way out the stack is sampled scaled, so paint appears in the right place and at the
right size with proxy on or off, and switching between them changes nothing but
sharpness.

Painting while proxied is refused rather than half-supported. The reason is specific to
a raster paint node: clone and smudge sample the image, and at proxy scale that means
baking proxy-resolution pixels permanently into full-resolution paint. RotoPaint avoids
the problem by storing vector strokes and re-rasterising them; a raster node cannot.
A warning is honest, silently baking soft pixels is not.

## Where the paint is stored

Paint lives in sidecar files, not in the `.nk`:

```
<script folder>/RasterPaint/<script name>_<node name>/paint.rpt        (all frames)
<script folder>/RasterPaint/<script name>_<node name>/paint.0101.rpt   (single frame)
```

Files are written when the script is saved or autosaved, or with **Save paint now**.
They are lossless and compressed (a test canvas of 2.8 MB in memory saved as 0.41 MB).
Before the script has a name, paint goes to a temp folder and is copied next to the
script on the first save. "Save As" and renaming the node copy the files across.

Send the `RasterPaint` folder along with the script to the farm. Copy-pasting the
node does not copy its paint.

## Limits

- `.abr`: what is read from the file is the brush list with names, tip bitmaps, size,
  spacing, hardness, angle, roundness, flips, and the shape, scattering and transfer
  dynamics. Texture, Dual Brush, Colour Dynamics, Brush Pose and the mixer-brush
  settings (wetness, mix) are ignored. Dynamics driven by pen tilt or the stylus wheel
  fall back to off, since Nuke does not report those. Computed brushes have no bitmap
  and load as round-brush presets, which is exactly what they are. Files in the older
  v1/v2 format carry no settings beyond spacing, so those brushes come in at defaults.
- No Texture, Dual Brush, Colour Dynamics, Wet Edges, Noise or Airbrush build-up.
- No smudge, blur or sharpen brushes yet.
- Layers have no masks, groups or adjustment layers, and cannot be dragged to reorder -
  use Move up / Move down.
- No rotation or roundness for tips, no smudge / blur / dodge tools, no blend modes.
- Painted and cloned pixels get alpha 1, like RotoPaint. Turn off **paint into
  alpha** to pass the input alpha through.
- Proxy / downrez mode and non-square pixels are not handled: paint coordinates are
  full-resolution pixels.
- Clone offsets are whole pixels (sharp, no resampling). The source is the image as
  it was before the stroke started, as in Photoshop.
- The undo history is kept in memory (1 GB budget) and is lost on reload.

## What to check first in Nuke

1. **It compiles.** Signatures were taken from the NDK reference, but small
   differences between Nuke versions are likely.
2. **`paint_dir` expansion.** The code assumes `File_knob` hands the Op the path with
   its TCL expression already evaluated. If paint always lands in the temp folder,
   that assumption is wrong and `paintDir()` needs to evaluate the knob itself.
3. **Viewer refresh while dragging** (`touch()`). It bumps a hidden knob and calls
   `asapUpdate()`. If the viewer lags or does not refresh, this is the place.
4. **Ctrl+Z** (`commitHistory()` / `syncHistory()`). It relies on Nuke calling
   `knob_changed` for the hidden `stroke_index` knob when undoing.
5. **Saving on script save** (`GlueKnob::to_script`). Confirm `.rpt` files appear when
   you save. **Save paint now** works regardless.
6. **Clone from the input** (`fetchInputTile()`). It pulls input pixels on the UI
   thread, outside a normal render. Test on a heavy tree.
7. **Modifier keys.** Ctrl+click and Shift+drag may collide with viewer shortcuts on
   your platform. They are a few lines in `handle()`.
8. **Tablet pressure.** `ViewerContext::pressure()` has no documented range. The code
   treats values outside (0, 1] as "no pressure".

## Layout

```
src/core/Canvas.*        sparse tiled canvas, row compositing, file format
src/core/Brush.*         dab rasteriser (rotation, roundness), .abr reader
src/core/Descriptor.*    Photoshop descriptor reader, for .abr brush settings
src/core/PaintEngine.*   strokes, dynamics, clone, erase, undo/redo
src/core/Preview.*       stroke swatch and tip thumbnails, run through the real engine
src/nuke/Overlay.h       viewer toolbar, brush picker, swatch, cursor preview
src/nuke/RasterPaint.cpp the Nuke node
tests/test_core.cpp      unit tests and benchmark for src/core
src/core/Blend.*         blend modes and premultiplied compositing
tests/test_overlay.cpp   overlay layout and hit-testing
tests/test_layers.cpp    layer metadata round trip and file naming
                         (both need NUKE_ROOT and are not built by default - build the
                         rp_test_overlay / rp_test_layers targets)
python/menu.py           menu entry
```
