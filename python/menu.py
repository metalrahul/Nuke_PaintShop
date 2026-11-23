# Put this file and the compiled NPaintShop plugin in a folder on NUKE_PATH
# (or merge these lines into your own menu.py).
#
# Scripts saved before the node was renamed still say "RasterPaint". Those keep
# working without an entry here: RasterPaint.tcl points Nuke at this plugin, and the
# plugin registers the old name as an alias of the same node. The menu only needs to
# offer the current name.
#
# Nuke Indie and Non-commercial cannot load the NDK plugin, and asking them to (this
# menu entry, or Tab > NPaintShop) brings up "Custom plugins are not supported". There
# the entry is left out, so Tab finds the OpenFX version instead. In commercial Nuke
# both versions are listed, so this one is labelled (NDK) to tell them apart.
import nuke

if not (nuke.env.get("indie") or nuke.env.get("nc")):
    nuke.menu("Nodes").addCommand("Draw/NPaintShop (NDK)", "nuke.createNode('NPaintShop')")
