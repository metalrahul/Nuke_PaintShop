# NPaintShop for Nuke

A raster paint node for Nuke. Strokes are baked into pixels as you draw, the way
Photoshop does it, instead of being stored and re-rendered like RotoPaint. A shot with
ten thousand strokes renders as fast as one with ten.

- Paint, erase, clone stamp and smudge, cloning across time or from a second input
- Round brush or sampled tips loaded from Photoshop `.abr` files, with an installable
  brush library
- Photoshop-style brush dynamics: shape, scattering and transfer, with tablet pressure
- Layers, each with opacity, one of fourteen blend modes, visibility and lock
- A viewer toolbar, a brush picker with thumbnails, and a live stroke preview
- Undo and redo through Nuke's own Ctrl+Z
- One layer for all frames, or separate paint per frame
- A "paint only" output, so the paint can be rendered out and comped by people who do
  not have the plugin

> **Beta.** Tested on Windows with Nuke 17.0v3. The Linux build is produced
> automatically and has not yet been tested inside Nuke on Linux - reports welcome.

## Requirements

- **Nuke 17.0**, Windows or Linux. An NDK plugin only loads in the Nuke version it was
  built for, so these builds will not load in Nuke 16 or earlier.
- **A commercial Nuke, NukeX or Nuke Studio licence.** See below.

### Nuke Indie and Non-commercial cannot load it

NDK plugins do not load in **Nuke Indie** or **Nuke Non-commercial**. Foundry's flavour
comparison lists "NDK / C++ SDK Support" and "Third Party / Custom C++ node / plug-in
support" as *No* for both, and the Indie FAQ states that "Custom C++ Compiled nodes will
not work or load correctly in Nuke Indie." The licence decides this, not the installed
build, so the same install running under a commercial licence is fine.

| Licence | Can load this plugin |
| --- | --- |
| Nuke / NukeX / Nuke Studio (commercial) | Yes |
| Nuke Indie | No |
| Nuke Non-commercial | No |

## Install

Download the zip for your platform from the
[Releases](https://github.com/metalrahul/Nuke_PaintShop/releases) page:

- `NPaintShop-<version>-Nuke17.0-win64.zip` for Windows
- `NPaintShop-<version>-Nuke17.0-linux64.zip` for Linux

Each contains the plugin, `menu.py`, this README and the licence.

### Windows

1. Close Nuke.
2. Open your `.nuke` folder: `C:\Users\<your name>\.nuke` (paste `%USERPROFILE%\.nuke`
   into the Explorer address bar).
3. Copy `NPaintShop.dll` into it.
4. **If you have no `menu.py` there**, copy the one from the zip. **If you already have
   one, do not overwrite it** - open it and add these two lines at the end:

   ```python
   import nuke
   nuke.menu("Nodes").addCommand("Draw/NPaintShop", "nuke.createNode('NPaintShop')")
   ```

5. Start Nuke. The node is under **Draw > NPaintShop**, or press Tab and type
   `NPaintShop`.

### Linux

1. Close Nuke.
2. Copy the plugin into your `.nuke` folder:

   ```bash
   mkdir -p ~/.nuke
   cp NPaintShop.so ~/.nuke/
   ```

3. **If you have no `~/.nuke/menu.py`**, copy the one from the zip. **If you already
   have one, do not overwrite it** - add the two lines shown in the Windows steps to the
   end of it.
4. Start Nuke. The node is under **Draw > NPaintShop**, or press Tab and type
   `NPaintShop`.

### For a studio or a shared install

Instead of each artist's `.nuke`, put `NPaintShop.dll` / `NPaintShop.so` and `menu.py`
in any folder on `NUKE_PATH`. Brush sets placed in an `NPaintShop/brushes` folder on
`NUKE_PATH` appear for everyone (see Brush library).

### If the node does not appear

- Check the Nuke version is 17.0 and the licence is commercial (see Requirements).
- Check the plugin file is directly in `.nuke`, not in a subfolder from the zip.
- Look in Nuke's Script Editor or terminal for a line mentioning `NPaintShop` - it
  usually says why it did not load. Please include it when reporting a problem.

## Use

Open the node's control panel and paint in the 2D viewer.

| Action | Result |
| --- | --- |
| Drag | Paint with the current tool |
| Ctrl+click | Set the clone source. The next clone stroke fixes the offset |
| Shift+drag left/right | Resize the brush |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo strokes (also the Undo / Redo buttons) |
| `Alt+B` / `Alt+E` / `Alt+S` / `Alt+U` | Brush / eraser / clone stamp / smudge |
| `Alt+C` | Pick the colour under the cursor |
| `Alt+[` / `Alt+]` | Smaller / larger brush |
| `Alt+M` | Toggle the brush menu |
| `Alt+L` | Toggle the layer list |

The same list is on the Settings tab under **shortcuts**.

Every shortcut uses Alt deliberately. Nuke's 2D viewer already binds the bare letters
this node would otherwise want - `p` pauses the viewer, `b` shows the blue channel, `l`
plays forward, `i` marks an in point, and `[` and `]` toggle the toolbars.

The toolbar down the left of the viewer does the same things and stays a constant size
as you zoom. The grid button opens a picker with a thumbnail of every brush tip; drag
its bottom-right grip to resize it. Bottom left is a live swatch of the current brush,
rendered by the paint engine itself, so it shows exactly what a stroke will lay down.
Clicking the toolbar never paints.

**opacity** caps how opaque one stroke can get, however much it overlaps itself.
**flow** is how quickly each dab builds towards that cap.

## Brushes

Pick a Photoshop `.abr` in **brush file** (Settings tab). Its brushes appear in the
**brush** menu after `round`, under their real names. With **use brush file settings**
on (the default), picking one also takes its size, spacing, hardness, angle, roundness
and dynamics from the file, so a brush behaves as it does in Photoshop. Turn it off to
use the file for tip shapes only.

### Brush library

**Install** (Settings tab) copies the loaded `.abr` into your brush library, and from
then on it is one pick away in the **brush set** menu on the main tab. **Remove**
deletes the selected set from your library.

Your library lives in `~/.nuke/NPaintShop/brushes` (`%USERPROFILE%\.nuke\...` on
Windows), or wherever the `NPAINTSHOP_BRUSHES` environment variable points. Sets in an
`NPaintShop/brushes` folder anywhere on `NUKE_PATH` are listed too, so a studio can
publish brushes to everyone; those cannot be removed from inside Nuke.

### Brush presets

**Save preset** (Settings tab) stores every brush and dynamics setting under a name,
along with the loaded `.abr` and the selected tip. Pick a preset from the menu on the
main tab to apply it. Presets live in `~/.nuke/NPaintShop.presets`, shared by every
script. **Reset brush** puts every brush and dynamics setting back to its default.

## Brush dynamics

On the Dynamics tab, following Photoshop's model: each property has a **jitter** (a
random reduction), a **control** (off, fade over N dabs, or pen pressure) and a
**minimum** that floors the result.

- **Brush tip shape** - angle and roundness, flip x/y.
- **Shape dynamics** - size, angle and roundness jitter. Angle can also follow the
  stroke direction, which with a flat tip gives a calligraphic nib.
- **Scattering** - throw each dab off the stroke by up to N brush diameters, with
  `count` dabs per spacing step.
- **Transfer** - opacity and flow jitter.

Every stroke replays the same random sequence from the **random seed**, so a stroke you
like is reproducible.

## Layers

Paint goes onto the selected layer; the stack composites bottom to top over the node's
input. The **Layers** tab holds the stack: name, opacity, blend mode, visibility, lock,
and buttons to add, duplicate, delete, reorder, merge down and flatten. The viewer shows
the stack top right - click a row to select it, click its box to toggle visibility.

Fourteen blend modes: normal, multiply, screen, overlay, darken, lighten, colour dodge,
colour burn, hard light, soft light, difference, exclusion, plus and minus. Normal,
multiply, screen, plus, minus, difference, darken and lighten behave sensibly on
over-range (HDR) values; the rest are defined for 0-1 and do odd things above 1, which
is inherent to the formulas.

**brush blend** sets how a stroke combines with what is already on its layer. That is
separate from the layer's own blend mode, and the two stack.

## Cloning

**clone from** picks the image the clone tool copies: `A` is this node's input, `B` the
second input, so a cleanplate can be painted in. **time offset** shifts the source in
time - `-1` clones from the previous frame, which is how a rig gets painted out using a
frame where it isn't there. The source is read once at stroke time and baked in, so the
result does not change if the source does later.

## Rendering the paint for someone without the plugin

Set **output** (main tab) to **paint only**. The node then outputs every visible layer
composited on transparent black, premultiplied, with alpha. Put a Write node after it
and render an EXR; anyone can Merge (over) that onto the plate and get the same picture
without NPaintShop installed.

That match is exact for layers in the normal blend mode. A layer set to multiply, screen
and so on is defined by what lies under it, plate included, so in paint-only output it
blends only with the layers below it.

## Settings tab

- **Brush library, presets** - as described above.
- **Storage** - the paint directory, **Save paint now** and **Reload from disk**.
- **Memory** - limits that matter on very large plates:
  - **undo memory** - megabytes of undo history (default 1024).
  - **clone cache** - megabytes of clone source kept in memory (default 512).
  - **other frames** - megabytes of paint kept for frames you are not on, in
    single-frame mode (default 2048). Past it, the furthest frames are written to disk
    and released; they reload when you go back. 0 never releases unsaved frames.
  - **in use** - what the node is holding right now.
  - **Free memory** - empties the clone cache and undo history and releases other
    frames. Paint is not lost, but earlier strokes can no longer be undone.

  These limits describe the machine rather than the shot, so they are not saved in the
  script. Set `NPAINTSHOP_UNDO_MB`, `NPAINTSHOP_CLONE_CACHE_MB` or `NPAINTSHOP_FRAME_MB`
  to change the defaults.
- **Performance** - **last stroke** reports where the time went on the last stroke.
  Please include it when reporting a slow brush.

### Large plates on 16 GB machines

A fully painted layer on a 16k latlong is about 2 GB, and paint is your work, not a
cache, so it is never thrown away. Paint is sparse, so a few retouches cost far less.
With 16 GB of RAM and very large plates, lower Nuke's own cache (Preferences >
Performance > Caching) to around 40%, keep the clone cache at 512 MB, and watch the
**in use** line.

## Where the paint is stored

Paint lives in sidecar files next to the script, not in the `.nk`:

```
<script folder>/RasterPaint/<script name>_<node name>/paint.L01.rpt        all frames
<script folder>/RasterPaint/<script name>_<node name>/paint.L01.0101.rpt   frame 101
```

(The folder keeps its original `RasterPaint` name so scripts from before the rename
still find their paint.)

Files are written when the script is saved or autosaved, or with **Save paint now**.
They are lossless and compressed. Before the script has a name, paint goes to a temp
folder and is copied next to the script on the first save. "Save As" and renaming the
node copy the files across.

**Send the `RasterPaint` folder along with the script** to the farm or to another
artist. Copy-pasting the node does not copy its paint.

## Proxy

The canvas always holds full-resolution pixels; with proxy on, paint is displayed
scaled to the right place and size. Painting while proxied is refused, because clone
and smudge would bake proxy-resolution pixels permanently into full-resolution paint.

## Limits

- `.abr`: brush names, tip bitmaps, size, spacing, hardness, angle, roundness, flips,
  and the shape, scattering and transfer dynamics are read. Texture, Dual Brush, Colour
  Dynamics, Brush Pose, Wet Edges, Noise, Airbrush and mixer-brush settings are not.
  Dynamics driven by pen tilt or the stylus wheel fall back to off, since Nuke does not
  report those.
- No blur, sharpen or dodge/burn brushes yet.
- Layers have no masks, groups or adjustment layers, and are reordered with Move up /
  Move down rather than by dragging.
- Clone offsets are whole pixels (sharp, no resampling).
- The undo history is kept in memory and is lost when the script is reloaded.
- Very large brushes on very large plates (a 2000 px brush on a 16k latlong) can still
  lag. This is being worked on.

## Reporting problems

Please open an issue on the [Issues](https://github.com/metalrahul/Nuke_PaintShop/issues)
page with:

- your Nuke version (e.g. 17.0v3) and operating system,
- what you did and what happened,
- for slowness, the **last stroke** and **in use** lines from the Settings tab,
- for crashes, whether it happens again and what you were doing just before.

## Build from source

Needs CMake 3.16+ and the compiler Foundry lists for your Nuke version, because NDK
plugins are ABI-sensitive:

- **Windows:** Visual Studio 2022.
- **Linux:** GCC 11 on Rocky Linux 9 (what Foundry builds Nuke 15-17 with). Building on
  a newer distribution or compiler can produce a plugin Nuke refuses to load.

`NUKE_ROOT` must be the exact install folder, patch number included:

```
cmake -B build -DNUKE_ROOT="C:/Program Files/Nuke17.0v3"
cmake --build build --config Release
ctest --test-dir build -C Release      # core tests, no Nuke needed
```

On Linux, e.g. `-DNUKE_ROOT=/opt/Nuke17.0v3 -DCMAKE_BUILD_TYPE=Release`. Without
`-DNUKE_ROOT` only the core tests are built.

The plugin is `build/Release/NPaintShop.dll` on Windows and `build/NPaintShop.so` on
Linux. On Windows the build also writes `NPaintShop.pdb`; keep it alongside each
release you make, since crash dumps can only be read against the matching one.

Options, all optional:

- `-DRP_DDIMAGE=<path>` if the DDImage library is somewhere the build does not look.
- `-DRP_USE_NUKE_CMAKE=ON` to link through Nuke's own `NukeConfig.cmake`.
- `-DRP_OLD_ABI=ON` for Nuke 14 and older on Linux (old libstdc++ string ABI).

The Linux release is built by `.github/workflows/linux.yml` in a Rocky Linux 9
container. It downloads Nuke at build time from the link in the `NUKE_LINUX_URL`
repository variable; Nuke itself is never stored in this repository.

`RasterPaint.tcl` is only needed by scripts saved before the node was renamed: put it
next to the plugin and those scripts load as before.

## Layout

```
src/core/Canvas.*         sparse tiled canvas, row compositing, file format
src/core/Brush.*          dab rasteriser, .abr reader
src/core/Descriptor.*     Photoshop descriptor reader, for .abr brush settings
src/core/PaintEngine.*    strokes, dynamics, clone, smudge, erase, undo/redo
src/core/Blend.h          blend modes and premultiplied compositing
src/core/Preview.*        stroke swatch and tip thumbnails, run through the real engine
src/nuke/NPaintShop.cpp   the Nuke node
src/nuke/Overlay.h        viewer toolbar, brush picker, swatch, cursor preview
src/nuke/BrushLibrary.h   installed brush sets
tests/test_core.cpp       unit tests and benchmark for src/core
tests/test_library.cpp    brush library
tests/test_overlay.cpp    overlay layout and hit-testing  (need NUKE_ROOT; build the
tests/test_layers.cpp     layer metadata round trip        rp_test_* targets by name)
python/menu.py            menu entry
```

## Licence

MIT - see [LICENSE](LICENSE). Nuke and the NDK are Foundry's and are not part of this
repository.

## Credits

Rahul Venugopal - [www.rkvfx.com](https://www.rkvfx.com)

Built using Claude Code.
