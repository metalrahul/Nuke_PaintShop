# Put this file and the compiled NPaintShop plugin in a folder on NUKE_PATH
# (or merge these lines into your own menu.py).
#
# Scripts saved before the node was renamed still say "RasterPaint". Those keep
# working without an entry here: RasterPaint.tcl points Nuke at this plugin, and the
# plugin registers the old name as an alias of the same node. The menu only needs to
# offer the current name.
import nuke

nuke.menu("Nodes").addCommand("Draw/NPaintShop", "nuke.createNode('NPaintShop')")
