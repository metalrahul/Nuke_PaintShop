# The node was renamed to NPaintShop. Nuke loads a plugin lazily by looking for a file
# named after the node class, so a script that still says "RasterPaint" would otherwise
# fail to find it. This tells Nuke where that class now lives; the plugin registers the
# old name as an alias, so those nodes load exactly as before.
load NPaintShop
