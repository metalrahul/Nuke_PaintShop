# NPaintShop OFX

The OpenFX version of NPaintShop. It uses the same paint engine as the main (NDK) plugin,
but it loads in **Nuke Indie**, which the NDK plugin cannot.

Use the NDK plugin in Nuke commercial and the OFX plugin in Nuke Indie. Don't install
both into one Nuke Indie setup: an NDK plugin in `.nuke` makes Indie show
"Custom plugins are not supported".

## What it does (beta)

- Paint, erase, clone and smudge, with **pen pressure** (Windows Ink or Wintab, chosen automatically).
- Layers: add, duplicate, delete, reorder, merge down, flatten, with per-layer opacity and blend modes.
- The viewer toolbar from the NDK version: tools, the brush grid, the layer list, undo/redo and the stroke swatch.
- Round brush and Photoshop `.abr` brushes, brush sets, and **all brush dynamics**:
  shape, scattering, transfer, colour dynamics, noise and wet edges.
- Brush presets. They're shared with the NDK version through `~/.nuke/NPaintShop.presets`.
- Paint on all frames or on single frames, into rgba, rgb or alpha, and "paint only" output.
- Undo with Ctrl+Z.
- **Saving.** Paint is saved when the script is saved, in one of two places, set under
  Settings > storage:
  - **sidecar files** (default): one folder per node under `~/.nuke/NPaintShop/paint`,
    or under your own **paint directory**. Scripts stay small. Copy that folder along
    with the script.
  - **inside the script**: the paint is written into the `.nk`. You only have one file
    to move, but heavy paint makes the script large.
- The same viewer shortcuts as the NDK version: Alt+B / E / S / U for the tools, Alt+C to
  pick a colour, Alt+[ and Alt+] to resize, Alt+M and Alt+L for the brush grid and layer
  list, Ctrl+click for the clone source, Shift+drag, Shift+click and Ctrl+Shift+drag.

- Clone from a second input (B), memory limits, Free memory and Reload from disk, as in
  the NDK version.

On Linux, pen pressure isn't read yet; strokes paint at full pressure.

## Build (Windows)

You don't need Nuke installed. The OpenFX SDK is downloaded during configure.

```
cmake -S ofx -B build-ofx
cmake --build build-ofx --config Release
```

This produces `build-ofx\NPaintShopOFX.ofx.bundle`.

## Install

1. Copy the whole `NPaintShopOFX.ofx.bundle` folder into, for example,
   `C:\Users\<you>\.nuke\ofx`.
2. Set the environment variable `OFX_PLUGIN_PATH` to that folder. On Windows: Start >
   "Edit environment variables for your account".
3. Start Nuke, press Tab, and type `NPaintShop`.

If it doesn't appear, delete the `ofxplugincache` file in your `.nuke` folder and restart
Nuke.
