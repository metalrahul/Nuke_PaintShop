// NPaintShop - raster paint node for Nuke (NDK).
//
// Strokes are baked straight into a sparse tiled canvas; nothing is stored per stroke,
// so render cost does not grow with the amount of painting. All pixel work lives in
// src/core (no Nuke dependency, unit tested). This file is the Nuke glue only:
// knobs, viewer interaction, rendering and persistence.

static const char* const CLASS = "NPaintShop";
static const char* const HELP =
    "Raster paint. Paint, erase, clone and smudge directly into pixels - strokes are not "
    "stored, "
    "so the node stays fast however much you paint.\n\n"
    "With the control panel open, in the 2D viewer:\n"
    "  drag                 paint with the current tool\n"
    "  Ctrl+click        set the clone source\n"
    "  Shift+drag       resize the brush\n"
    "  Alt+B / E / S    brush, eraser, clone stamp\n"
    "  Alt+U             smudge\n"
    "  Alt+C             pick the colour under the cursor\n"
    "  Alt+[ / Alt+]    smaller / larger brush\n"
    "  Alt+M / Alt+L   brush menu, layer list\n\n"
    "Shortcuts all use Alt because Nuke's viewer already owns the bare letters.\n\n"
    "Paint is stored in sidecar files in 'paint directory' and is written whenever the "
    "script is saved (or with 'Save paint now').\n\n"
    "By Rahul Venugopal - www.rkvfx.com. Built using Claude Code.";

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <list>
#include <mutex>
#include <ostream>

#include "DDImage/Enumeration_KnobI.h"
#include "DDImage/ImagePlane.h"
#include "DDImage/Iop.h"
#include "DDImage/Knob.h"
#include "DDImage/Knobs.h"
#include "DDImage/Row.h"
#include "DDImage/ViewerContext.h"
#include "DDImage/gl.h"

#include "BrushLibrary.h"
#include "Overlay.h"
#include "PaintEngine.h"

using namespace DD::Image;
namespace fs = std::filesystem;

// Debug tracing, off unless RASTERPAINT_TRACE=1 is set in the environment.
// Every line is flushed, so when it is on the last line printed is the one
// immediately before a crash. Launch Nuke from a shell to see the output.
static bool rpTraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("RASTERPAINT_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}
#define RPT(...)                             \
  do {                                       \
    if (rpTraceOn()) {                       \
      std::fprintf(stderr, "[RP] ");         \
      std::fprintf(stderr, __VA_ARGS__);     \
      std::fprintf(stderr, "\n");            \
      std::fflush(stderr);                   \
    }                                        \
  } while (0)

class NPaintShop;

// ------------------------------------------------------------------------------
// Everything that belongs to the *node* rather than to one Op instance. Nuke makes
// several Op instances per node (one per frame/view context), so the canvases must
// live outside the Op. Stores are found through the Node pointer.
// ------------------------------------------------------------------------------
// One paint layer. The pixels live in a canvas per frame key, so a layer works the
// same whether the node is in "all frames" or "single frame" mode. `id` is stable for
// the life of the layer and names its files, so reordering never renames anything.
struct Layer {
  int id = 1;
  std::string name = "Paint 1";
  float opacity = 1.0f;
  int blend = rp::kNormal;
  bool visible = true, locked = false;
  std::map<int, std::shared_ptr<rp::Canvas>> canvases;

  bool contributes() const { return visible && opacity > 0.0f; }
};

// A memory limit in megabytes, taken from the environment when it is set there. The
// limits describe the machine rather than the shot, so they are deliberately not saved
// in the script: a script from a 64 GB workstation must not carry its limits onto a
// 16 GB laptop. A facility sets the variables once; everyone else gets the defaults.
static int envMB(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const long n = std::strtol(v, &end, 10);
  if (end == v || n < 0) return fallback;
  return int(std::min(n, 65536L));
}

struct PaintStore {
  static constexpr int kAllFrames = INT_MIN;

  std::mutex mutex;  // guards layers + dir
  std::vector<Layer> layers;
  int active = 0;
  int nextId = 1;
  std::string dir;        // directory the canvases are loaded from / saved to
  bool dirReset = false;  // the user pointed paint_dir somewhere else on purpose

  // UI-thread only from here on
  rp::PaintEngine engine;
  rp::AbrFile abr;
  std::string abrLoaded;
  int strokeIndex = 0;
  NPaintShop* uiOp = nullptr;  // the Op instance the viewer is currently showing
  int overlayHoverX = 0, overlayHoverY = 0;
  bool overlayHover = false;

  rpui::Overlay overlay;
  bool pickerOpen = false;
  bool layersOpen = true;
  bool colorPicking = false;  // armed by the eyedropper button

  enum Drag { kNone, kStroke, kSize, kGrid };
  Drag drag = kNone;
  bool haveCursor = false, cloneSrcPending = false;
  float cursorX = 0, cursorY = 0, cloneSrcX = 0, cloneSrcY = 0;
  int dragMouseX = 0, dragMouseY = 0, dragCols = 6, dragRows = 4;
  float dragSize = 0;

  std::atomic<uint64_t> edits{0};
  const uint64_t nonce = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());

  static std::string fileFor(const std::string& d, int layerId, int key) {
    char buf[96];
    if (key == kAllFrames) std::snprintf(buf, sizeof(buf), "/paint.L%02d.rpt", layerId);
    else std::snprintf(buf, sizeof(buf), "/paint.L%02d.%04d.rpt", layerId, key);
    return d + buf;
  }

  // Paint written before layers existed. Loaded into the first layer, then saved under
  // the new name, so an older script keeps its paint.
  static std::string legacyFileFor(const std::string& d, int key) {
    if (key == kAllFrames) return d + "/paint.rpt";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/paint.%04d.rpt", key);
    return d + buf;
  }

  Layer& activeLayer() {
    if (layers.empty()) addLayer("Paint 1");
    if (active < 0 || active >= int(layers.size())) active = int(layers.size()) - 1;
    return layers[size_t(active)];
  }

  Layer& addLayer(const std::string& name) {
    Layer l;
    l.id = nextId++;
    l.name = name.empty() ? ("Paint " + std::to_string(l.id)) : name;
    layers.push_back(std::move(l));
    return layers.back();
  }

  // Called with mutex held. Follows the paint directory when it changes: after
  // "Save As" or a node rename the existing files are copied across, so paint is
  // never left behind; after a deliberate change the new directory is loaded instead.
  void setDir(const std::string& d) {
    if (d == dir) return;
    if (dir.empty()) { dir = d; return; }
    if (dirReset) {
      for (Layer& l : layers) l.canvases.clear();
      engine.clearHistory();
    } else {
      std::error_code ec;
      fs::create_directories(d, ec);
      if (fs::is_directory(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
          if (e.path().extension() != ".rpt") continue;
          const fs::path to = fs::path(d) / e.path().filename();
          if (!fs::exists(to, ec)) fs::copy_file(e.path(), to, ec);
        }
      }
      for (Layer& l : layers)
        for (auto& kv : l.canvases) kv.second->dirty = true;
    }
    dirReset = false;
    dir = d;
  }

  // ---- layer metadata, serialised into a hidden knob so it travels in the .nk while
  // the pixels stay in sidecar files.
  // Shared with the preset file, which has the same escaping needs.
  static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
      const unsigned char u = (unsigned char)c;
      if (u > 32 && u < 127 && c != '%' && c != '|' && c != ';' && c != ',') o += c;
      else { char b[8]; std::snprintf(b, sizeof(b), "%%%02X", u); o += b; }
    }
    return o;
  }
  static std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '%' && i + 2 < s.size()) {
        o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
        i += 2;
      } else o += s[i];
    }
    return o;
  }

  std::string serialize() {
    std::lock_guard<std::mutex> lock(mutex);
    std::string o = "v1;" + std::to_string(nextId) + ";" + std::to_string(active);
    for (const Layer& l : layers) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "|%d,%.6g,%d,%d,%d,", l.id, l.opacity, l.blend,
                    l.visible ? 1 : 0, l.locked ? 1 : 0);
      o += buf;
      o += esc(l.name);
    }
    return o;
  }

  void deserialize(const std::string& s) {
    std::lock_guard<std::mutex> lock(mutex);
    if (s.rfind("v1;", 0) != 0) return;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= s.size()) {
      const size_t q = s.find('|', p);
      parts.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
      if (q == std::string::npos) break;
      p = q + 1;
    }
    if (parts.empty()) return;
    int n = 1, a = 0;
    if (std::sscanf(parts[0].c_str(), "v1;%d;%d", &n, &a) != 2) return;
    std::vector<Layer> loaded;
    for (size_t i = 1; i < parts.size(); ++i) {
      Layer l;
      int vis = 1, lock2 = 0;
      char nameBuf[512] = {0};
      if (std::sscanf(parts[i].c_str(), "%d,%f,%d,%d,%d,%511[^\n]", &l.id, &l.opacity, &l.blend,
                      &vis, &lock2, nameBuf) < 5)
        continue;
      l.visible = vis != 0;
      l.locked = lock2 != 0;
      l.blend = std::max(0, std::min(l.blend, int(rp::kBlendCount) - 1));
      l.name = unesc(nameBuf);
      if (l.name.empty()) l.name = "Paint " + std::to_string(l.id);
      // Keep any pixels already loaded for a layer with this id.
      for (Layer& old : layers)
        if (old.id == l.id) { l.canvases = std::move(old.canvases); break; }
      loaded.push_back(std::move(l));
    }
    if (loaded.empty()) return;
    layers = std::move(loaded);
    nextId = std::max(n, 1);
    active = std::max(0, std::min(a, int(layers.size()) - 1));
  }

  // The canvas for one layer at one frame, loading it from disk on first use.
  std::shared_ptr<rp::Canvas> canvasFor(Layer& l, int key, const std::string& d) {
    std::lock_guard<std::mutex> lock(mutex);
    setDir(d);
    auto it = l.canvases.find(key);
    if (it != l.canvases.end()) return it->second;
    auto c = std::make_shared<rp::Canvas>();
    if (!c->load(fileFor(dir, l.id, key)) && &l == &layers.front())
      c->load(legacyFileFor(dir, key));  // pre-layers paint lands in the bottom layer
    l.canvases[key] = c;
    evict(key, true, frameBudget ? frameBudget : SIZE_MAX);
    return c;
  }

  // Raw clone-source tiles straight off the input, cached across strokes.
  //
  // This is the one cache that matters for how cloning feels. Reading the input is by
  // far the most expensive part of a clone stroke - a 128-row band of a 4K EXR means
  // decoding full-width scanlines upstream - and a per-stroke cache threw all of it
  // away on every mouse-down, so each new stroke paid full price again.
  //
  // Only the untouched input goes in here. The layer stack is composited on top per
  // stroke, because layers change as the user paints and stale layer pixels in a cache
  // would be far harder to notice than a slow stroke.
  // Tiles are shared, not owned outright: a stroke in progress holds references to the
  // ones it is using, so eviction here can never pull them out from under it. Handing
  // out bare pointers and then clearing the map on a full cache is what produced black
  // tiles on a 16k plate - 512 MB is about 1900 tiles, which a 4K image never reaches
  // but a 16k latlong blows through inside a stroke.
  //
  // Eviction is least-recently-used, one tile at a time. Clearing the lot meant the
  // next stroke re-read everything from the input, which on a large plate is the most
  // expensive thing the node does.
  struct InputCache {
    using Ptr = std::shared_ptr<rp::SrcTile>;
    std::unordered_map<uint64_t, std::pair<Ptr, std::list<uint64_t>::iterator>> tiles;
    std::list<uint64_t> lru;  // front = most recently used
    DD::Image::Hash guard;    // what the cached tiles were read from
    bool guardSet = false;
    size_t budget = size_t(512) << 20;

    // Throws the cache away when the thing it was read from has changed - a different
    // frame, a different input, an upstream edit.
    void validate(const DD::Image::Hash& h) {
      if (guardSet && h == guard) return;
      clear();
      guard = h;
      guardSet = true;
    }
    void clear() { tiles.clear(); lru.clear(); }
    Ptr find(uint64_t k) {
      auto it = tiles.find(k);
      if (it == tiles.end()) return nullptr;
      lru.splice(lru.begin(), lru, it->second.second);
      return it->second.first;
    }
    Ptr insert(uint64_t k, Ptr t) {
      while (!lru.empty() && (tiles.size() + 1) * sizeof(rp::SrcTile) > budget) {
        tiles.erase(lru.back());
        lru.pop_back();
      }
      lru.push_front(k);
      tiles[k] = {t, lru.begin()};
      return t;
    }
    size_t bytes() const { return tiles.size() * sizeof(rp::SrcTile); }
  };
  InputCache inputCache;

  // Snapshot of every layer's canvas for this frame, bottom to top, for rendering.
  struct Shot {
    std::shared_ptr<rp::Canvas> canvas;
    float opacity = 1.0f;
    int blend = rp::kNormal;
  };
  std::vector<Shot> stackFor(int key, const std::string& d, const Layer* skip = nullptr) {
    std::vector<Shot> out;
    std::lock_guard<std::mutex> lock(mutex);
    setDir(d);
    bool loadedAny = false;
    for (Layer& l : layers) {
      if (!l.contributes() || &l == skip) continue;
      auto it = l.canvases.find(key);
      std::shared_ptr<rp::Canvas> c;
      if (it != l.canvases.end()) {
        c = it->second;
      } else {
        c = std::make_shared<rp::Canvas>();
        if (!c->load(fileFor(dir, l.id, key)) && &l == &layers.front())
          c->load(legacyFileFor(dir, key));
        l.canvases[key] = c;
        loadedAny = true;
      }
      if (!c->empty()) out.push_back(Shot{c, l.opacity, l.blend});
    }
    // This is the path a render takes, and it used to load a canvas per frame without
    // ever letting one go, so a long per-frame render held every painted frame it had
    // passed. It runs on render threads, so it only drops frames that are already on
    // disk; writing unsaved paint is left to the UI thread in canvasFor.
    if (loadedAny) evict(key, false, frameBudget ? frameBudget : SIZE_MAX);
    return out;
  }

  // Paint held in memory for frames that are not being shown or painted. Past this,
  // those frames are written to disk if they need it and released. 0 means no limit.
  size_t frameBudget = size_t(2048) << 20;

  // Writes one canvas to its file. Called with mutex held, and with the canvas either
  // locked by the caller or referenced by nobody else.
  bool writeCanvas(const Layer& l, int key, rp::Canvas& c) {
    std::error_code ec;
    const std::string f = fileFor(dir, l.id, key);
    if (c.empty()) {
      fs::remove(f, ec);
      c.dirty = false;
      return true;
    }
    fs::create_directories(dir, ec);
    if (!c.save(f)) return false;
    c.dirty = false;
    return true;
  }

  // Keeps memory bounded. Called with mutex held. Only ever touches canvases nobody
  // else holds: not the frame being shown (`keep`), not one a render is reading, not
  // one a stroke or an undo step still refers to. Returns how many frames it wrote.
  //
  // Two rules. The old one is by count: with more than 24 canvases loaded, clean idle
  // ones are dropped. The second is by size, because on a 16k plate one painted frame
  // can be 2 GB and twenty of them is not a number that fits in anything: when idle
  // frames hold more than `budget` bytes, the ones furthest from the current frame go
  // first. A frame with unsaved paint is written before it is released, and only when
  // `mayWrite` is set - if the write fails it simply stays in memory, so paint is
  // never dropped unless it is safely on disk.
  int evict(int keep, bool mayWrite, size_t budget) {
    auto idle = [keep](const std::pair<const int, std::shared_ptr<rp::Canvas>>& kv) {
      return kv.first != keep && kv.second.use_count() == 1;
    };
    size_t total = 0;
    for (const Layer& l : layers) total += l.canvases.size();
    if (total > 24)
      for (Layer& l : layers)
        for (auto it = l.canvases.begin(); it != l.canvases.end() && total > 16;) {
          if (idle(*it) && !it->second->dirty) { it = l.canvases.erase(it); --total; } else ++it;
        }
    if (budget == SIZE_MAX) return 0;

    struct Cand { Layer* layer; int key; size_t bytes; long long away; };
    std::vector<Cand> cands;
    size_t held = 0;
    for (Layer& l : layers)
      for (auto& kv : l.canvases) {
        if (!idle(kv)) continue;
        const size_t b = kv.second->memoryBytes();
        const long long dist = (long long)kv.first - (long long)keep;
        held += b;
        cands.push_back(Cand{&l, kv.first, b, dist < 0 ? -dist : dist});
      }
    if (held <= budget) return 0;
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.away > y.away; });
    int written = 0;
    for (const Cand& c : cands) {
      if (held <= budget) break;
      auto it = c.layer->canvases.find(c.key);
      if (it == c.layer->canvases.end()) continue;
      rp::Canvas& cv = *it->second;
      if (cv.dirty) {
        if (!mayWrite || dir.empty() || !writeCanvas(*c.layer, c.key, cv)) continue;
        ++written;
      }
      c.layer->canvases.erase(it);
      held -= c.bytes;
    }
    return written;
  }

  // What the node is holding, for the memory readout. UI thread, never during a stroke.
  struct MemStats {
    size_t paint = 0, undo = 0, clone = 0;
    int canvases = 0;
  };
  MemStats memStats() {
    MemStats m;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (Layer& l : layers)
        for (auto& kv : l.canvases) {
          std::shared_lock<std::shared_mutex> cl(kv.second->mutex);
          m.paint += kv.second->memoryBytes();
          if (!kv.second->empty()) ++m.canvases;
        }
    }
    m.undo = engine.historyBytes();
    m.clone = inputCache.bytes();
    return m;
  }

  // Writes every canvas with unsaved changes. Returns the number of files written, -1 on error.
  int saveDirty() {
    std::lock_guard<std::mutex> lock(mutex);
    if (dir.empty()) return 0;
    int written = 0;
    bool failed = false;
    for (Layer& l : layers)
      for (auto& kv : l.canvases) {
        rp::Canvas& c = *kv.second;
        std::shared_lock<std::shared_mutex> cl(c.mutex);
        if (!c.dirty) continue;
        const bool hadPaint = !c.empty();
        if (!writeCanvas(l, kv.first, c)) failed = true;
        else if (hadPaint) ++written;
      }
    return failed ? -1 : written;
  }

  // Removes files belonging to layers that no longer exist, so a deleted layer does not
  // come back on reload. Only called once the user saves.
  void pruneFiles() {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    std::vector<std::string> keep;
    for (const Layer& l : layers) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "paint.L%02d.", l.id);
      keep.push_back(buf);
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::string n = e.path().filename().generic_string();
      if (n.rfind("paint.L", 0) != 0) continue;
      bool wanted = false;
      for (const std::string& k : keep) wanted = wanted || n.rfind(k, 0) == 0;
      if (!wanted) fs::remove(e.path(), ec);
    }
  }

  void reload() {
    std::lock_guard<std::mutex> lock(mutex);
    for (Layer& l : layers) l.canvases.clear();
    engine.clearHistory();
    strokeIndex = 0;
  }

  PaintStore() { RPT("    PaintStore ctor done nonce=%llx", (unsigned long long)nonce); }

  // One store per node, created on first use and intentionally never freed: a node's
  // store is a few hundred bytes, and leaking it is far safer than tying its lifetime
  // to Op instances that Nuke creates and destroys freely. Deliberately no shared_ptr,
  // no weak_ptr and no pruning - this is called from render threads during hashing.
  static PaintStore* forNode(const void* node) {
    RPT("   forNode enter node=%p", node);
    static std::mutex m;
    static std::map<const void*, PaintStore*> all;
    RPT("   forNode statics ready");
    std::lock_guard<std::mutex> lock(m);
    RPT("   forNode locked, map size=%zu", all.size());
    PaintStore*& slot = all[node];
    RPT("   forNode lookup gave %p", (void*)slot);
    if (!slot) {
      RPT("   forNode allocating PaintStore (%zu bytes)", sizeof(PaintStore));
      slot = new PaintStore();
      RPT("   forNode allocated %p", (void*)slot);
    }
    RPT("   forNode returning");
    return slot;
  }
};

// ------------------------------------------------------------------------------

// Every brush setting, with its default. One table drives both "Reset brush" and the
// preset format, so the two can never drift apart.
struct BrushSetting { const char* knob; double def; };
static const BrushSetting kBrushSettings[] = {
    {"size", 50}, {"hardness", 0.8}, {"opacity", 1}, {"flow", 1}, {"spacing", 0.1},
    {"brush_blend", 0}, {"smoothing", 0},
    {"pressure_size", 1}, {"pressure_flow", 0},
    {"dyn_angle", 0}, {"dyn_roundness", 1}, {"dyn_flip_x", 0}, {"dyn_flip_y", 0},
    {"size_jitter", 0}, {"size_control", 0}, {"size_fade", 25}, {"size_minimum", 0},
    {"angle_jitter", 0}, {"angle_control", 0}, {"angle_fade", 25},
    {"round_jitter", 0}, {"round_control", 0}, {"round_fade", 25}, {"round_minimum", 0},
    {"flip_x_jitter", 0}, {"flip_y_jitter", 0},
    {"scatter", 0}, {"scatter_both", 0}, {"scatter_count", 1}, {"count_jitter", 0},
    {"opacity_jitter", 0}, {"opacity_control", 0}, {"opacity_fade", 25}, {"opacity_minimum", 0},
    {"flow_jitter", 0}, {"flow_control", 0}, {"flow_fade", 25}, {"flow_minimum", 0},
    {"dyn_seed", 1},
};


// Brush presets live in one text file beside the user's other Nuke settings, so they
// are shared by every script rather than trapped in one node. One line per preset:
//   <escaped name>\t<knob>=<value>\t...\tabr=<escaped path>\tbrush=<index>
// Kept at file scope, free of any Op, so the file handling can be tested on its own.
struct Preset {
  std::string name, line;
};

struct BrushPresets {
  static std::string defaultFile() {
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    std::error_code ec;
    if (!home || !*home)
      return (fs::temp_directory_path(ec) / "NPaintShop.presets").generic_string();
    const fs::path dir = fs::path(home) / ".nuke";
    fs::create_directories(dir, ec);
    const fs::path current = dir / "NPaintShop.presets";
    // Presets saved before the node was renamed live under the old name. Keep using
    // that file until there is a new one, so nobody loses the brushes they set up.
    if (!fs::exists(current, ec)) {
      const fs::path legacy = dir / "RasterPaint.presets";
      if (fs::exists(legacy, ec)) return legacy.generic_string();
    }
    return current.generic_string();
  }

  // Overridable so tests do not touch the real settings file.
  static std::string& path() {
    static std::string p = defaultFile();
    return p;
  }

  static std::vector<Preset> read() {
    std::vector<Preset> out;
    std::ifstream f(path());
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const size_t tab = line.find('\t');
      if (tab == std::string::npos || tab == 0) continue;
      out.push_back(Preset{PaintStore::unesc(line.substr(0, tab)), line.substr(tab + 1)});
    }
    return out;
  }

  // Written to a temp file and renamed, so a failure cannot truncate the real one.
  static bool write(const std::vector<Preset>& ps) {
    const std::string p = path();
    {
      std::ofstream f(p + ".tmp");
      if (!f) return false;
      for (const Preset& e : ps) f << PaintStore::esc(e.name) << '\t' << e.line << '\n';
      if (!f) return false;
    }
    std::error_code ec;
    fs::remove(p, ec);
    fs::rename(p + ".tmp", p, ec);
    return !ec;
  }

  // Replaces a preset of the same name, otherwise appends. Returns its index.
  static size_t put(std::vector<Preset>& ps, const std::string& name, const std::string& line) {
    for (size_t i = 0; i < ps.size(); ++i)
      if (ps[i].name == name) { ps[i].line = line; return i; }
    ps.push_back(Preset{name, line});
    return ps.size() - 1;
  }

  // Pulls one key out of a preset line. Returns false when the key is not present.
  static bool field(const std::string& line, const std::string& key, std::string& out) {
    size_t p = 0;
    while (p < line.size()) {
      const size_t tab = line.find('\t', p);
      const std::string f =
          line.substr(p, tab == std::string::npos ? std::string::npos : tab - p);
      p = tab == std::string::npos ? line.size() : tab + 1;
      const size_t eq = f.find('=');
      if (eq != std::string::npos && f.compare(0, eq, key) == 0) {
        out = f.substr(eq + 1);
        return true;
      }
    }
    return false;
  }
};

static const char* const kPaintChannels[] = {"rgba", "rgb", "alpha", nullptr};
static const char* const kCloneInputs[] = {"A (this input)", "B (second input)", nullptr};
static const char* const kNoPresets[] = {"(none)", nullptr};
static const char* const kNoBrushSets[] = {"(none)", nullptr};
static const char* const kNoLayers[] = {"Paint 1", nullptr};  // replaced once layers exist
static const char* const kControl[] = {"off", "fade", "pen pressure", nullptr};
static const char* const kAngleCtl[] = {"off", "fade", "pen pressure", "direction", nullptr};
static const char* const kTools[] = {"paint", "erase", "clone", "smudge", nullptr};
static const char* const kBrushes[] = {"round", nullptr};
static const char* const kLifetime[] = {"all frames", "single frame", nullptr};
static const char* const kOutput[] = {"paint over input", "paint only", nullptr};


class NPaintShop : public Iop {
  // knob storage
  int tool_ = 0, brushBlend_ = 0;
  float color_[3] = {1.0f, 1.0f, 1.0f};
  float size_ = 50.0f, hardness_ = 0.8f, opacity_ = 1.0f, flow_ = 1.0f, spacing_ = 0.1f;
  float smoothing_ = 0.0f;
  bool pressureSize_ = true, pressureFlow_ = false, paintAlpha_ = true;
  int paintChannels_ = 0;  // 0 rgba, 1 rgb, 2 alpha
  float cloneOffset_[2] = {50.0f, 0.0f};
  int cloneInput_ = 0;
  float cloneTime_ = 0.0f;
  float smudgeStrength_ = 0.6f, smudgeRate_ = 0.35f;
  // Memory limits. Defaults come from the environment so they follow the machine.
  int cloneCacheMB_ = envMB("NPAINTSHOP_CLONE_CACHE_MB", 512);
  int undoMB_ = envMB("NPAINTSHOP_UNDO_MB", 1024);
  int frameMB_ = envMB("NPAINTSHOP_FRAME_MB", 2048);
  int refreshMs_ = 40;
  int brush_ = 0;
  bool useFileSettings_ = true;
  const char* abrFile_ = "";
  int brushSet_ = 0;
  std::vector<std::string> brushSets_;  // installed sets, matching the knob menu
  // layers
  int presetSel_ = 0;
  const char* presetName_ = "";
  int layerSel_ = 0, layerBlend_ = 0;
  const char* layerName_ = "";
  float layerOpacity_ = 1.0f;
  bool layerVisible_ = true, layerLocked_ = false;
  const char* layerData_ = "";  // hidden: the whole stack's metadata, saved in the .nk

  // dynamics
  float dynAngle_ = 0.0f, dynRound_ = 1.0f;
  bool dynFlipX_ = false, dynFlipY_ = false;
  float sizeJitter_ = 0.0f, sizeMin_ = 0.0f;
  int sizeControl_ = 0, sizeFade_ = 25;
  float angleJitter_ = 0.0f;
  int angleControl_ = 0, angleFade_ = 25;
  float roundJitter_ = 0.0f, roundMin_ = 0.0f;
  int roundControl_ = 0, roundFade_ = 25;
  bool flipXJitter_ = false, flipYJitter_ = false;
  float scatter_ = 0.0f, countJitter_ = 0.0f;
  bool scatterBoth_ = false;
  int count_ = 1;
  float opacityJitter_ = 0.0f, opacityMin_ = 0.0f;
  int opacityControl_ = 0, opacityFade_ = 25;
  float flowJitter_ = 0.0f, flowMin_ = 0.0f;
  int flowControl_ = 0, flowFade_ = 25;
  int dynSeed_ = 1;

  int lifetime_ = 0;
  int output_ = 0;  // 0 paint over input, 1 paint only
  const char* paintDir_ =
      // Also unchanged by the rename, and deliberately: this is where painted pixels
      // already live on disk. Pointing new nodes somewhere else would quietly orphan
      // the work in every project that has been painted so far.
      "[file dirname [value root.name]]/RasterPaint/[file rootname [file tail [value root.name]]]_[value name]";
  int strokeIndexKnob_ = 0, paintRev_ = 0;

  mutable PaintStore* store_ = nullptr;  // owned by PaintStore::forNode, never freed here
  std::vector<PaintStore::Shot> stack_;       // set by _validate, read by engine
  std::vector<PaintStore::Shot> cloneStack_;  // frozen for the duration of a clone stroke

 public:
  explicit NPaintShop(Node* node) : Iop(node) {}
  ~NPaintShop() override {
    if (store_ && store_->uiOp == this) store_->uiOp = nullptr;
  }

  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 2; }
  bool test_input(int n, Op* op) const override {
    return n == 0 ? Iop::test_input(n, op) : (op == nullptr || dynamic_cast<Iop*>(op) != nullptr);
  }
  // The second input is optional: with nothing plugged in, cloning falls back to input A.
  Op* default_input(int n) const override { return n == 0 ? Iop::default_input(0) : nullptr; }
  const char* input_label(int n, char*) const override { return n == 0 ? "" : "B"; }

  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  static const Iop::Description d, dLegacy;

  PaintStore& store() const {
    if (!store_) {
      RPT("  store(): creating, node=%p", (void*)node());
      store_ = PaintStore::forNode(node());
      RPT("  store(): created %p", (void*)store_);
    }
    return *store_;
  }

  // Nuke's proxy scale for this context: 1 when proxy is off. The canvas always holds
  // full-resolution pixels, so these convert between canvas space and whatever space
  // the viewer and the render are currently working in.
  double proxyX() const {
    const double s = double(outputContext().scale_x());
    return (s > 1e-6 && s == s) ? s : 1.0;
  }
  double proxyY() const {
    const double s = double(outputContext().scale_y());
    return (s > 1e-6 && s == s) ? s : 1.0;
  }
  bool proxied() const {
    return std::fabs(proxyX() - 1.0) > 1e-6 || std::fabs(proxyY() - 1.0) > 1e-6;
  }

  int frameKey() const {
    return lifetime_ == 0 ? PaintStore::kAllFrames : int(std::lround(outputContext().frame()));
  }

  // paint_dir with its expression already expanded by the knob. Until the script has
  // been saved the default expression is not an absolute path, so use a temp folder;
  // the files are copied next to the script on the first save.
  std::string paintDir() const {
    RPT("  paintDir(): raw=%s", paintDir_ ? paintDir_ : "(null)");
    const std::string d = paintDir_ ? paintDir_ : "";
    std::error_code ec;
    if (!d.empty() && fs::path(d).is_absolute()) return d;
    fs::path tmp = fs::temp_directory_path(ec);
    char id[32];
    std::snprintf(id, sizeof(id), "%llx", (unsigned long long)store().nonce);
    return (tmp / "RasterPaint" / id).generic_string();  // see paint_dir above
  }

  // ---------------------------------------------------------------- knobs
  void knobs(Knob_Callback f) override {
    Enumeration_knob(f, &layerSel_, kNoLayers, "layer");
    SetFlags(f, Knob::SAVE_MENU | Knob::KNOB_CHANGED_ALWAYS);
    Tooltip(f, "The layer strokes go onto. Manage the stack on the Layers tab.");
    Bool_knob(f, &layerVisible_, "layer_visible", "visible");
    Tooltip(f, "Hide this layer without deleting its paint.");

    Enumeration_knob(f, &tool_, kTools, "tool");
    Tooltip(f, "paint: lay down colour.  erase: remove paint (never the input).  "
               "clone: copy the image from the clone offset. Ctrl+click in the viewer sets the source.");
    Color_knob(f, color_, "color");
    Enumeration_knob(f, &brushBlend_, rp::blendNames(), "brush_blend", "brush blend");
    Tooltip(f, "How the stroke combines with what is already on this layer. Separate "
               "from the layer's own blend mode, which controls how the whole layer "
               "sits over everything below. Erase ignores it.");
    Enumeration_knob(f, &brush_, kBrushes, "brush");
    SetFlags(f, Knob::SAVE_MENU);
    Tooltip(f, "'round' is the built-in brush (size + hardness). Load an .abr on the Settings tab for sampled tips.");
    Bool_knob(f, &useFileSettings_, "use_file_settings", "use brush file settings");
    Tooltip(f, "Picking a brush from the file also takes its size, spacing, hardness, "
               "angle, roundness and dynamics. Turn off to keep your own settings and "
               "use the file only for tip shapes.");
    Enumeration_knob(f, &brushSet_, kNoBrushSets, "brush_set", "brush set");
    SetFlags(f, Knob::SAVE_MENU);
    Tooltip(f, "An installed brush set. The script stores the set's name rather than a path, "
               "so it keeps working when the .abr moves, is renamed, or sits somewhere else "
               "on another machine. Sets live in your own library and in any NPaintShop/brushes "
               "folder on NUKE_PATH, so a facility can publish one set to everybody.");
    Float_knob(f, &size_, "size");
    SetRange(f, 1, 500);
    Tooltip(f, "Brush diameter in pixels. Shift+drag in the viewer to change it.");
    Float_knob(f, &hardness_, "hardness");
    Tooltip(f, "Edge hardness of the round brush.");
    Float_knob(f, &opacity_, "opacity");
    Tooltip(f, "Maximum opacity of one stroke, however much it overlaps itself.");
    Float_knob(f, &flow_, "flow");
    Tooltip(f, "How fast each dab builds up towards the stroke opacity.");
    Float_knob(f, &spacing_, "spacing");
    SetRange(f, 0.01, 1);
    Tooltip(f, "Distance between dabs as a fraction of the brush size. Larger is faster and dottier.");
    Float_knob(f, &smoothing_, "smoothing");
    SetRange(f, 0, 1);
    Tooltip(f, "Makes the brush chase the pointer instead of following it exactly, "
               "which takes the wobble out of a freehand line. The stroke still ends "
               "where you lifted off.");
    Bool_knob(f, &pressureSize_, "pressure_size", "pressure > size");
    Bool_knob(f, &pressureFlow_, "pressure_flow", "pressure > flow");
    Enumeration_knob(f, &cloneInput_, kCloneInputs, "clone_input", "clone from");
    Tooltip(f, "Which image the clone tool copies from. 'B' uses the second input, so a "
               "cleanplate or another element can be painted in. Falls back to A when "
               "nothing is connected to B.");
    Float_knob(f, &cloneTime_, "clone_time", "time offset");
    SetRange(f, -24, 24);
    Tooltip(f, "Frames to offset the clone source by, so a rig can be painted out using "
               "a frame where it is not there. -1 clones from the previous frame.");
    XY_knob(f, cloneOffset_, "clone_offset", "clone offset");
    SetFlags(f, Knob::NO_HANDLES);
    Tooltip(f, "Clone source relative to the brush, in pixels. Set by Ctrl+click then painting.");

    Float_knob(f, &smudgeStrength_, "smudge_strength", "smudge strength");
    SetRange(f, 0, 1);
    Tooltip(f, "How much of the carried colour the smudge brush lays down on each dab. "
               "Low values barely disturb the image; high values drag it a long way.");

    Float_knob(f, &smudgeRate_, "smudge_rate", "pickup");
    SetRange(f, 0, 1);
    Tooltip(f, "How quickly the smudge brush replaces the colour it is carrying with "
               "whatever it is passing over. 0 keeps the colour it started on and drags "
               "it the whole way, which is Photoshop's finger painting; 1 picks up fresh "
               "colour at every dab, so almost nothing travels.");

    Divider(f, "brush presets");
    Enumeration_knob(f, &presetSel_, kNoPresets, "preset");
    SetFlags(f, Knob::SAVE_MENU | Knob::KNOB_CHANGED_ALWAYS);
    Tooltip(f, "Saved brush settings. Picking one applies it immediately. Save and "
               "delete presets on the Settings tab.");
    Button(f, "brush_reset", "Reset brush");
    Tooltip(f, "Puts every brush and dynamics setting back to its default. Colour, the "
               "loaded brush file and the layer stack are left alone.");

    Divider(f, "history");
    Button(f, "undo_paint", "Undo");
    Button(f, "redo_paint", "Redo");
    Button(f, "clear_paint", "Clear frame");
    Tooltip(f, "Removes all paint on the current frame (undoable).");

    Divider(f, "paint target");
    Enumeration_knob(f, &lifetime_, kLifetime, "lifetime");
    Tooltip(f, "all frames: one painted layer shown on every frame.\n"
               "single frame: a separate layer per frame.");
    Enumeration_knob(f, &paintChannels_, kPaintChannels, "paint_channels", "paint into");
    Tooltip(f, "Which channels strokes affect. 'rgb' leaves the input alpha untouched; "
               "'alpha' paints a matte without changing the colour.");
    Bool_knob(f, &paintAlpha_, "paint_alpha");
    SetFlags(f, Knob::INVISIBLE | Knob::DO_NOT_WRITE);  // kept so older scripts still load

    Divider(f, "output");
    Enumeration_knob(f, &output_, kOutput, "output");
    Tooltip(f, "paint over input: the normal result.\n"
               "paint only: every visible layer composited together on transparent black, "
               "premultiplied, with alpha. Put a Write node after this one to render it "
               "(EXR keeps the float values and the alpha as they are); a plain Merge (over) "
               "on top of the plate then gives the same picture on a machine that does not "
               "have NPaintShop.\n"
               "That match is exact when layers use the normal blend mode. A layer set to "
               "multiply, screen and so on is defined by what lies under it, the plate "
               "included, and a standalone image cannot carry that - in 'paint only' such "
               "layers blend with the layers below them and nothing else. 'paint into' is "
               "ignored here: the output is always full RGBA.");

    Tab_knob(f, "Layers");
    Text_knob(f, "Strokes go onto the selected layer. The stack composites bottom to top "
                 "over the node's input.");
    Enumeration_knob(f, &layerSel_, kNoLayers, "layer2", "layer");
    Tooltip(f, "The layer being painted on. Strokes land here, and everything below shows "
               "through wherever it is transparent.");
    SetFlags(f, Knob::SAVE_MENU | Knob::KNOB_CHANGED_ALWAYS);
    String_knob(f, &layerName_, "layer_name", "name");
    Tooltip(f, "Renames the selected layer. Only a label - it has no effect on the image.");
    Float_knob(f, &layerOpacity_, "layer_opacity", "opacity");
    SetRange(f, 0, 1);
    Enumeration_knob(f, &layerBlend_, rp::blendNames(), "layer_blend", "blend");
    Tooltip(f, "How this layer combines with everything below it. Normal, multiply, "
               "screen, plus, minus, difference, darken and lighten are safe on "
               "over-range values; the others assume a 0..1 range.");
    Bool_knob(f, &layerLocked_, "layer_locked", "locked");
    Tooltip(f, "Stops strokes landing on this layer by accident.");
    Divider(f);
    Button(f, "layer_add", "Add");
    Tooltip(f, "Adds an empty layer above the selected one and makes it active.");
    Button(f, "layer_duplicate", "Duplicate");
    Tooltip(f, "Copies the selected layer, including its pixels, opacity and blend mode.");
    Button(f, "layer_delete", "Delete");
    Tooltip(f, "Deletes the selected layer and its paint. A node always keeps at least one.");
    Newline(f);
    Button(f, "layer_up", "Move up");
    Tooltip(f, "Moves the selected layer up the stack, so it covers the ones below it.");
    Button(f, "layer_down", "Move down");
    Newline(f);
    Button(f, "layer_merge", "Merge down");
    Tooltip(f, "Composites this layer into the one below using its blend mode and "
               "opacity, then removes it. Covers every frame the layers have paint on.");
    Button(f, "layer_flatten", "Flatten visible");
    Tooltip(f, "Merges every visible layer down into one, applying their opacities and blend "
               "modes. Hidden layers are left alone. Undoable, but the separate layers are "
               "gone once you save.");
    Text_knob(f, "Layer changes are undoable with Ctrl+Z, like strokes.");

    Tab_knob(f, "Dynamics");
    BeginGroup(f, "tip_shape", "brush tip shape");
    Float_knob(f, &dynAngle_, "dyn_angle", "angle");
    SetRange(f, -180, 180);
    Tooltip(f, "Rotates the tip. Only visible on a non-round tip or with roundness below 1.");
    Float_knob(f, &dynRound_, "dyn_roundness", "roundness");
    SetRange(f, 0, 1);
    Tooltip(f, "Squashes the tip's short axis. 1 is circular.");
    Bool_knob(f, &dynFlipX_, "dyn_flip_x", "flip x");
    Tooltip(f, "Mirrors the tip left to right. Does nothing to a symmetrical tip.");
    Bool_knob(f, &dynFlipY_, "dyn_flip_y", "flip y");
    Tooltip(f, "Mirrors the tip top to bottom. Does nothing to a symmetrical tip.");
    EndGroup(f);

    BeginGroup(f, "shape_dynamics", "shape dynamics");
    Float_knob(f, &sizeJitter_, "size_jitter", "size jitter");
    SetRange(f, 0, 1);
    Tooltip(f, "Randomly shrinks each dab by up to this fraction.");
    Enumeration_knob(f, &sizeControl_, kControl, "size_control", "control");
    Tooltip(f, "What drives dab size along the stroke. 'fade' ramps it down over the next few dabs; "
               "'pen pressure' takes it from the tablet, and does nothing with a mouse.");
    Int_knob(f, &sizeFade_, "size_fade", "fade steps");
    Tooltip(f, "How many dabs the fade takes to reach the minimum below. Only used when control "
               "is set to fade.");
    SetRange(f, 1, 200);
    Float_knob(f, &sizeMin_, "size_minimum", "minimum");
    SetRange(f, 0, 1);
    Tooltip(f, "Floor for the size factor, so a fade or jitter cannot shrink past this.");
    Divider(f);
    Float_knob(f, &angleJitter_, "angle_jitter", "angle jitter");
    SetRange(f, 0, 1);
    Tooltip(f, "1 randomises the tip angle over a full turn.");
    Enumeration_knob(f, &angleControl_, kAngleCtl, "angle_control", "control");
    Tooltip(f, "'direction' turns the tip to follow the stroke, which is what makes a flat "
               "tip behave like a calligraphic nib.");
    Int_knob(f, &angleFade_, "angle_fade", "fade steps");
    Tooltip(f, "How many dabs the fade takes to reach the minimum below. Only used when control "
               "is set to fade.");
    SetRange(f, 1, 200);
    Divider(f);
    Float_knob(f, &roundJitter_, "round_jitter", "roundness jitter");
    Tooltip(f, "Randomly squashes each dab by up to this fraction, so a round tip stops looking "
               "stamped.");
    SetRange(f, 0, 1);
    Enumeration_knob(f, &roundControl_, kControl, "round_control", "control");
    Tooltip(f, "What drives roundness along the stroke. 'fade' ramps it down over the next few dabs; "
               "'pen pressure' takes it from the tablet, and does nothing with a mouse.");
    Int_knob(f, &roundFade_, "round_fade", "fade steps");
    Tooltip(f, "How many dabs the fade takes to reach the minimum below. Only used when control "
               "is set to fade.");
    SetRange(f, 1, 200);
    Float_knob(f, &roundMin_, "round_minimum", "minimum");
    Tooltip(f, "Floor for roundness, so jitter and fade cannot squash a dab flatter than this.");
    SetRange(f, 0, 1);
    Divider(f);
    Bool_knob(f, &flipXJitter_, "flip_x_jitter", "flip x jitter");
    Tooltip(f, "Mirrors each dab left to right at random, which breaks up the repetition of a "
               "distinctive tip.");
    Bool_knob(f, &flipYJitter_, "flip_y_jitter", "flip y jitter");
    Tooltip(f, "Mirrors each dab top to bottom at random.");
    EndGroup(f);

    BeginGroup(f, "scattering", "scattering");
    Float_knob(f, &scatter_, "scatter", "scatter");
    SetRange(f, 0, 5);
    Tooltip(f, "Throws each dab off the stroke by up to this many brush diameters.");
    Bool_knob(f, &scatterBoth_, "scatter_both", "both axes");
    Tooltip(f, "Scatter along the stroke as well as across it.");
    Int_knob(f, &count_, "scatter_count", "count");
    SetRange(f, 1, 16);
    Tooltip(f, "Dabs laid down at every spacing step.");
    Float_knob(f, &countJitter_, "count_jitter", "count jitter");
    Tooltip(f, "Randomly drops some of the scattered copies, so the scatter thins out instead of "
               "laying down the same number every time.");
    SetRange(f, 0, 1);
    EndGroup(f);

    BeginGroup(f, "transfer", "transfer");
    Float_knob(f, &opacityJitter_, "opacity_jitter", "opacity jitter");
    Tooltip(f, "Randomly fades each dab by up to this fraction. Opacity is the ceiling for the "
               "whole stroke, so this varies how solid each dab can get.");
    SetRange(f, 0, 1);
    Enumeration_knob(f, &opacityControl_, kControl, "opacity_control", "control");
    Tooltip(f, "What drives opacity along the stroke. 'fade' ramps it down over the next few dabs; "
               "'pen pressure' takes it from the tablet, and does nothing with a mouse.");
    Int_knob(f, &opacityFade_, "opacity_fade", "fade steps");
    Tooltip(f, "How many dabs the fade takes to reach the minimum below. Only used when control "
               "is set to fade.");
    SetRange(f, 1, 200);
    Float_knob(f, &opacityMin_, "opacity_minimum", "minimum");
    Tooltip(f, "Floor for opacity, so a fade or jitter cannot take a dab below this.");
    SetRange(f, 0, 1);
    Divider(f);
    Float_knob(f, &flowJitter_, "flow_jitter", "flow jitter");
    Tooltip(f, "Randomly reduces how much paint each dab puts down. Unlike opacity jitter this "
               "accumulates where dabs overlap, so it reads as a grainier stroke.");
    SetRange(f, 0, 1);
    Enumeration_knob(f, &flowControl_, kControl, "flow_control", "control");
    Tooltip(f, "What drives flow along the stroke. 'fade' ramps it down over the next few dabs; "
               "'pen pressure' takes it from the tablet, and does nothing with a mouse.");
    Int_knob(f, &flowFade_, "flow_fade", "fade steps");
    Tooltip(f, "How many dabs the fade takes to reach the minimum below. Only used when control "
               "is set to fade.");
    SetRange(f, 1, 200);
    Float_knob(f, &flowMin_, "flow_minimum", "minimum");
    Tooltip(f, "Floor for flow, so a fade or jitter cannot take a dab below this.");
    SetRange(f, 0, 1);
    EndGroup(f);

    Int_knob(f, &dynSeed_, "dyn_seed", "random seed");
    SetRange(f, 1, 9999);
    Tooltip(f, "Every stroke replays the same random sequence from this seed, so a stroke "
               "you like is reproducible. Change it for a different scatter pattern.");

    Tab_knob(f, "Settings");
    Divider(f, "brush library");
    File_knob(f, &abrFile_, "abr_file", "brush file");
    Tooltip(f, "Photoshop brush file (.abr, versions 1, 2, 6, 7, 10). Sampled tips are loaded; "
               "computed brushes and brush dynamics are ignored. Use this to try a file where "
               "it sits; use Install to add it to the library so scripts stop depending on "
               "this path. Whichever of the two was set last is the one in use.");

    Button(f, "brush_install", "Install");
    Tooltip(f, "Copy the brush file above into your brush library and select it, so this "
               "script no longer depends on where that file happens to live.");
    Button(f, "brush_remove", "Remove");
    Tooltip(f, "Delete the set picked under 'brush set' on the main tab from your own library. Sets published on NUKE_PATH "
               "are left alone.");

    Divider(f, "brush presets");
    String_knob(f, &presetName_, "preset_name", "name");
    Tooltip(f, "Name to save the current settings under. Delete removes the preset "
               "picked on the main tab.");
    Button(f, "preset_save", "Save preset");
    Button(f, "preset_delete", "Delete preset");

    Divider(f, "storage");
    File_knob(f, &paintDir_, "paint_dir", "paint directory");
    Tooltip(f, "Folder holding this node's paint files. Follows the script and node name by default.");
    Button(f, "save_paint", "Save paint now");
    Button(f, "reload_paint", "Reload from disk");
    Tooltip(f, "Discards unsaved paint and the undo history, and reloads the files.");

    Divider(f, "memory");
    Int_knob(f, &undoMB_, "undo_memory", "undo memory");
    SetRange(f, 64, 8192);
    SetFlags(f, Knob::NO_ANIMATION | Knob::DO_NOT_WRITE);
    Tooltip(f, "Megabytes of undo history to keep. Each undo step holds a copy of the "
               "tiles its stroke changed, 256 KB apiece, so strokes over existing paint "
               "on a very large plate use this up quickly. The oldest steps are dropped "
               "first and the most recent stroke can always be undone, even when it is "
               "bigger than this on its own. Not saved in the script: it describes the "
               "machine, not the shot. Set NPAINTSHOP_UNDO_MB to change the default.");

    Int_knob(f, &cloneCacheMB_, "clone_cache", "clone cache");
    SetRange(f, 128, 8192);
    SetFlags(f, Knob::NO_ANIMATION | Knob::DO_NOT_WRITE);
    Tooltip(f, "Megabytes of clone source kept in memory. Each 128x128 tile costs 272 KB, "
               "so the default holds about 1900 tiles - plenty for HD or 4K, but a 16k "
               "latlong is over 8000 tiles and will evict while you work. Raise this if "
               "cloning on a very large plate keeps re-reading the input; the 'clone source' "
               "field under performance shows how many tiles had to be read rather than "
               "reused. Not saved in the script. Set NPAINTSHOP_CLONE_CACHE_MB to change "
               "the default.");

    Int_knob(f, &frameMB_, "frame_memory", "other frames");
    SetRange(f, 0, 16384);
    SetFlags(f, Knob::NO_ANIMATION | Knob::DO_NOT_WRITE);
    Tooltip(f, "Megabytes of paint to keep in memory for frames other than the one you "
               "are on. Past this, the frames furthest from the current one are released; "
               "any with unsaved paint are written to the paint directory first, exactly "
               "as saving the script would, so nothing is lost and they reload when you "
               "go back to them. Frames that an undo step still refers to are kept. "
               "0 never releases unsaved frames. Only matters with lifetime set to "
               "'single frame'. Not saved in the script. Set NPAINTSHOP_FRAME_MB to "
               "change the default.");

    // No storage pointer, on purpose. With String_knob(f, &member, ...) Nuke owns the
    // memory the member points at ("Nuke will manage the memory used by the string",
    // Knobs.h) and may free it when it stores a new value. These fields used to point
    // the member at a std::string of our own; once two instances of this Op had each
    // done that with different text, Nuke freed a buffer that was not its to free and
    // the process died with heap corruption inside this very call. Nothing reads the
    // value back, so the status fields simply have no member at all.
    String_knob(f, "memory_status", "in use");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION | Knob::DO_NOT_WRITE | Knob::OUTPUT_ONLY);
    Tooltip(f, "What this node is holding right now. 'paint' is the layers themselves - "
               "256 KB for every 128x128 tile with paint on it, so a fully covered 16k "
               "layer is 2 GB - and is your work, not a cache: it is only released for "
               "frames you are not on. 'undo' and 'clone cache' are bounded by the limits "
               "above. Updated after each stroke and when the panel opens.");

    Button(f, "free_memory", "Free memory");
    Tooltip(f, "Empties the clone cache, discards this node's undo history, and writes "
               "and releases paint on every frame except the current one. Nothing painted "
               "is lost, but strokes made before this can no longer be undone.");

    Divider(f, "performance");
    Int_knob(f, &refreshMs_, "refresh_ms", "refresh every");
    SetRange(f, 0, 250);
    SetFlags(f, Knob::NO_ANIMATION);
    Tooltip(f, "Milliseconds between viewer refreshes while a stroke is being drawn. Every "
               "mouse move used to start a render and cancel the one before it, so on a very "
               "large plate none of them ever finished. Raise this if painting on a huge "
               "image still feels heavy; 0 refreshes on every mouse move, as it used to.");

    String_knob(f, "clone_status", "clone source");  // no storage pointer, see memory_status
    // OUTPUT_ONLY keeps updating this from marking the script modified, so a clone
    // stroke does not leave an unsaved-changes asterisk behind.
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION | Knob::DO_NOT_WRITE | Knob::OUTPUT_ONLY);
    Tooltip(f, "What the last clone stroke found behind the brush. Pixels with no source - "
               "outside the image, or an offset pointing off the plate - are left untouched "
               "rather than painted, so a clone stroke can never lay down a flat patch of "
               "nothing.");

    String_knob(f, "stroke_status", "last stroke");  // no storage pointer, see memory_status
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION | Knob::DO_NOT_WRITE | Knob::OUTPUT_ONLY);
    Tooltip(f, "Where the last stroke's time went. 'engine' is this node laying pixels "
               "down, 'fetch' is reading the clone source, and 'elsewhere' is everything "
               "outside the node - mostly Nuke re-rendering and re-validating the tree. "
               "If 'elsewhere' dominates, the plate and the upstream tree are the cost, "
               "not the brush.");

    // Closed by default: a reference to look up, not something to scroll past every time.
    BeginClosedGroup(f, "shortcuts_group", "shortcuts");
    // Label and text, so the keys line up in the panel's own label column.
    Text_knob(f, "Alt+B", "brush");
    Text_knob(f, "Alt+E", "erase");
    Text_knob(f, "Alt+S", "clone stamp");
    Text_knob(f, "Alt+U", "smudge");
    Text_knob(f, "Alt+C", "pick colour");
    Text_knob(f, "Alt+[", "smaller brush");
    Text_knob(f, "Alt+]", "larger brush");
    Text_knob(f, "Alt+M", "brush menu");
    Text_knob(f, "Alt+L", "layer list");
    Text_knob(f, "Drag", "paint");
    Text_knob(f, "Ctrl+click", "set the clone source");
    Text_knob(f, "Shift+drag", "resize the brush");
    EndGroup(f);

    // About. The build date comes from the compiler, so a tester's screenshot of this
    // panel says exactly which DLL they were running when they report a problem.
    Divider(f, "about");
    Text_knob(f, "author", "Rahul Venugopal");
    Text_knob(f, "website", "<a href=\"https://www.rkvfx.com\">www.rkvfx.com</a>");
    Text_knob(f, "build", "beta, " __DATE__);
    Text_knob(f, "", "<i>Built using Claude Code</i>");

    // Hidden plumbing: stroke_index hooks painting into Nuke's own undo (Ctrl+Z),
    // paint_rev makes Nuke re-render after each change.
    String_knob(f, &layerData_, "layer_data");
    SetFlags(f, Knob::INVISIBLE | Knob::NO_ANIMATION);
    Int_knob(f, &strokeIndexKnob_, "stroke_index");
    SetFlags(f, Knob::INVISIBLE | Knob::DO_NOT_WRITE | Knob::KNOB_CHANGED_ALWAYS);
    Int_knob(f, &paintRev_, "paint_rev");
    SetFlags(f, Knob::INVISIBLE | Knob::DO_NOT_WRITE | Knob::NO_UNDO | Knob::NO_ANIMATION);
    CustomKnob1(GlueKnob, f, this, "paint_handle");
  }

  int knob_changed(Knob* k) override {
    RPT("knob_changed knob=%p", (void*)k);
    PaintStore& s = store();
    if (k == &Knob::showPanel) {
      refreshBrushSets();  // sets may have been installed since the panel last opened
      loadBrushes(false);
      loadLayers();
      refreshPresetMenu(-1);
      applyMemoryLimits();
      reportMemory();
      return 1;
    }
    if (k->is("undo_memory") || k->is("frame_memory") || k->is("clone_cache")) {
      if (!s.engine.stroking()) { applyMemoryLimits(); reportMemory(); }
      return 1;
    }
    if (k->is("free_memory")) {
      if (s.engine.stroking()) return 1;
      s.inputCache.clear();
      strokeSrc_.clear();
      cloneStack_.clear();  // holds references to this frame's canvases from the last clone stroke
      s.engine.clearHistory();
      int failedDir = 0;
      {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.setDir(paintDir());
        s.evict(uiFrameKey(), true, 0);
        failedDir = s.dir.empty() ? 1 : 0;
      }
      if (failedDir) warning("NPaintShop: no paint directory, unsaved frames were kept in memory");
      reportMemory();
      return 1;
    }
    if (k->is("abr_file")) {
      // Choosing a file by hand means that file, not whatever set was selected.
      if (Knob* bs = knob("brush_set")) bs->set_value(0);
      loadBrushes(true);
      return 1;
    }
    if (k->is("brush_set")) { loadBrushes(true); return 1; }
    if (k->is("brush_install")) {
      Knob* fk = knob("abr_file");
      const char* txt = fk ? fk->get_text() : nullptr;
      std::string err;
      const std::string name = npaint::BrushLibrary::install(txt ? txt : "", &err);
      if (name.empty()) {
        warning("NPaintShop: %s", err.c_str());
        return 1;
      }
      refreshBrushSets();
      const auto it = std::find(brushSets_.begin(), brushSets_.end(), name);
      if (it != brushSets_.end())
        if (Knob* bs = knob("brush_set"))
          bs->set_value(double(it - brushSets_.begin() + 1));
      loadBrushes(true);
      return 1;
    }
    if (k->is("brush_remove")) {
      const std::string name = selectedSet();
      if (name.empty()) {
        warning("NPaintShop: pick a brush set to remove first.");
        return 1;
      }
      std::string err;
      if (!npaint::BrushLibrary::remove(name, &err)) {
        warning("NPaintShop: %s", err.c_str());
        return 1;
      }
      refreshBrushSets();
      if (Knob* bs = knob("brush_set")) bs->set_value(0);
      loadBrushes(true);
      return 1;
    }
    if (k->is("tool")) {
      // Selecting Clone changes what _request has to declare of the inputs, and a
      // request is only re-run when the op is invalidated.
      // Clone and smudge both read the input at stroke time, so both change what
      // _request has to declare, and a request is only re-run when the op is
      // invalidated.
      auto reads = [](int t) {
        return t == int(rp::Tool::Clone) || t == int(rp::Tool::Smudge);
      };
      const bool wasClone = reads(tool_);
      const bool isClone = reads(int(knobValue("tool", tool_)));
      if (wasClone != isClone) invalidate();
      return 1;
    }
    if (k->is("brush")) { applyBrushFileSettings(); touchAll(); return 1; }
    if (k->is("layer_data")) { loadLayers(true); return 1; }
    if (k->is("layer") || k->is("layer2")) { selectLayer(int(k->get_value())); return 1; }
    if (k->is("layer_name") || k->is("layer_opacity") || k->is("layer_blend") ||
        k->is("layer_visible") || k->is("layer_locked")) {
      pullLayerKnobs();
      return 1;
    }
    if (k->name().rfind("layer_", 0) == 0 && layerOp(k->name().c_str())) return 1;
    if (k->is("preset")) { applyPreset(int(k->get_value())); return 1; }
    if (k->is("preset_save")) { savePreset(); return 1; }
    if (k->is("preset_delete")) { deletePreset(); return 1; }
    if (k->is("brush_reset")) { resetBrush(); return 1; }
    if (k->is("stroke_index")) { syncHistory(int(k->get_value())); return 1; }
    if (k->is("undo_paint")) { if (s.engine.canUndo()) stepHistory(-1); return 1; }
    if (k->is("redo_paint")) { if (s.engine.canRedo()) stepHistory(+1); return 1; }
    if (k->is("clear_paint")) {
      Layer& L = s.activeLayer();
      const rp::IRect dirty = s.engine.clear(s.canvasFor(L, uiFrameKey(), paintDir()));
      if (!dirty.empty()) { commitHistory(); touch(dirty); }
      return 1;
    }
    if (k->is("save_paint")) {
      writeLayerData();
      const int n = s.saveDirty();
      if (n < 0) warning("NPaintShop: could not write paint files to %s", s.dir.c_str());
      else s.pruneFiles();
      reportMemory();
      return 1;
    }
    if (k->is("reload_paint")) { s.reload(); touchAll(); reportMemory(); return 1; }
    if (k->is("paint_dir")) {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.dirReset = true;
      return 1;
    }
    return Iop::knob_changed(k);
  }

  // Reads the stack out of the hidden metadata knob. Called when the panel opens and
  // when the knob itself changes, which is how a script load brings layers back.
  void loadLayers(bool force = false) {
    PaintStore& s = store();
    Knob* k = knob("layer_data");
    const char* txt = k ? k->get_text() : nullptr;
    const std::string data = txt ? txt : "";
    bool empty = false;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      empty = s.layers.empty();
    }
    if (!data.empty() && (force || empty || data != s.serialize())) s.deserialize(data);
    syncLayerKnobs();
    if (data.empty()) writeLayerData();
  }

  // Installed set names, kept in the knob's menu with "(none)" first so the stored
  // index still means something when a set is added or removed.
  void refreshBrushSets() {
    brushSets_ = npaint::BrushLibrary::list();
    std::vector<std::string> menu{"(none)"};
    menu.insert(menu.end(), brushSets_.begin(), brushSets_.end());
    if (Knob* k = knob("brush_set"))
      if (Enumeration_KnobI* e = k->enumerationKnob()) e->menu(menu);
  }

  // The name of the set currently selected, or empty for "(none)".
  std::string selectedSet() {
    const int sel = int(knobValue("brush_set", brushSet_));
    if (sel <= 0 || size_t(sel - 1) >= brushSets_.size()) return "";
    return brushSets_[size_t(sel - 1)];
  }

  void loadBrushes(bool force) {
    PaintStore& s = store();
    std::string path;
    const std::string set = selectedSet();
    if (!set.empty()) {
      path = npaint::BrushLibrary::pathFor(set);
      // A script can name a set this machine does not have. Say so once and carry on
      // with no tips rather than failing to open the script.
      if (path.empty() && s.abrLoaded != set) {
        warning("NPaintShop: brush set '%s' is not installed on this machine. Install it, "
                "or pick a brush file.",
                set.c_str());
        s.abrLoaded = set;
        s.abr = rp::AbrFile();
        if (Knob* bk = knob("brush"))
          if (Enumeration_KnobI* e = bk->enumerationKnob())
            e->menu(std::vector<std::string>{"round"});
        return;
      }
    } else {
      Knob* fk = knob("abr_file");
      const char* txt = fk ? fk->get_text() : nullptr;
      path = txt ? txt : "";
    }
    if (!force && path == s.abrLoaded && (path.empty() || !s.abr.empty())) return;
    s.abrLoaded = path;
    s.abr = rp::AbrFile();
    if (!path.empty()) {
      std::string err;
      s.abr = rp::loadABR(path, &err);
      if (s.abr.empty()) warning("NPaintShop: %s: %s", path.c_str(), err.c_str());
    }
    std::vector<std::string> names{"round"};
    for (const rp::BrushPreset& pr : s.abr.presets) {
      std::string n = pr.name;
      if (pr.tip < 0) n += "  (round)";
      names.push_back(n);
    }
    if (Knob* bk = knob("brush"))
      if (Enumeration_KnobI* e = bk->enumerationKnob()) e->menu(names);
  }

  // Copies a brush file's own settings into the knobs. Called when the brush changes,
  // unless the user has turned that off.
  void applyBrushFileSettings() {
    PaintStore& s = store();
    const int sel = int(knobValue("brush", brush_));
    const bool use = knobValue("use_file_settings", useFileSettings_ ? 1 : 0) != 0;
    if (!use || sel <= 0) return;
    if (size_t(sel - 1) >= s.abr.presets.size()) return;
    const rp::BrushPreset& pr = s.abr.presets[size_t(sel - 1)];
    if (!pr.fromFile) return;
    const rp::BrushDynamics& d = pr.dyn;
    auto set = [&](const char* n, double v) { if (Knob* k = knob(n)) k->set_value(v); };
    set("size", pr.diameter);
    set("spacing", pr.spacing);
    set("hardness", pr.hardness);
    set("dyn_angle", d.angle);
    set("dyn_roundness", d.roundness);
    set("dyn_flip_x", d.flipX ? 1 : 0);
    set("dyn_flip_y", d.flipY ? 1 : 0);
    set("size_jitter", d.size.jitter);
    set("size_control", d.size.control);
    set("size_fade", d.size.fadeSteps);
    set("size_minimum", d.size.minimum);
    set("angle_jitter", d.angleJitter);
    set("angle_control", d.angleControl);
    set("angle_fade", d.angleFadeSteps);
    set("round_jitter", d.round.jitter);
    set("round_control", d.round.control);
    set("round_fade", d.round.fadeSteps);
    set("round_minimum", d.round.minimum);
    set("flip_x_jitter", d.flipXJitter ? 1 : 0);
    set("flip_y_jitter", d.flipYJitter ? 1 : 0);
    set("scatter", d.scatter);
    set("scatter_both", d.scatterBoth ? 1 : 0);
    set("scatter_count", d.count);
    set("count_jitter", d.countJitter);
    set("opacity_jitter", d.opacity.jitter);
    set("opacity_control", d.opacity.control);
    set("opacity_fade", d.opacity.fadeSteps);
    set("opacity_minimum", d.opacity.minimum);
    set("flow_jitter", d.flow.jitter);
    set("flow_control", d.flow.control);
    set("flow_fade", d.flow.fadeSteps);
    set("flow_minimum", d.flow.minimum);
  }

  // ---------------------------------------------------------------- brush presets

  void refreshPresetMenu(int select) {
    const std::vector<Preset> ps = BrushPresets::read();
    std::vector<std::string> names{"(none)"};
    for (const Preset& p : ps) names.push_back(p.name);
    if (Knob* k = knob("preset")) {
      if (Enumeration_KnobI* e = k->enumerationKnob()) e->menu(names);
      if (select >= 0) k->set_value(double(select));
    }
  }

  void savePreset() {
    std::string name;
    if (Knob* k = knob("preset_name")) {
      const char* t = k->get_text();
      name = t ? t : "";
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) {
      warning("NPaintShop: type a name before saving a preset.");
      return;
    }
    std::string line;
    char buf[128];
    for (const BrushSetting& bs : kBrushSettings) {
      Knob* k = knob(bs.knob);
      if (!k) continue;
      std::snprintf(buf, sizeof(buf), "%s=%.6g\t", bs.knob, k->get_value());
      line += buf;
    }
    if (Knob* k = knob("abr_file")) {
      const char* t = k->get_text();
      line += "abr=" + PaintStore::esc(t ? t : "") + "\t";
    }
    if (Knob* k = knob("brush")) {
      std::snprintf(buf, sizeof(buf), "brush=%d", int(k->get_value()));
      line += buf;
    }
    std::vector<Preset> ps = BrushPresets::read();
    const size_t at = BrushPresets::put(ps, name, line);
    if (!BrushPresets::write(ps)) {
      warning("NPaintShop: could not write %s", BrushPresets::path().c_str());
      return;
    }
    refreshPresetMenu(int(at) + 1);
  }

  void deletePreset() {
    const int sel = int(knobValue("preset", presetSel_)) - 1;
    std::vector<Preset> ps = BrushPresets::read();
    if (sel < 0 || sel >= int(ps.size())) {
      warning("NPaintShop: pick a preset to delete first.");
      return;
    }
    ps.erase(ps.begin() + sel);
    if (!BrushPresets::write(ps)) {
      warning("NPaintShop: could not write %s", BrushPresets::path().c_str());
      return;
    }
    refreshPresetMenu(0);
  }

  void applyPreset(int sel) {
    const std::vector<Preset> ps = BrushPresets::read();
    if (sel <= 0 || sel > int(ps.size())) return;
    const std::string line = ps[size_t(sel - 1)].line;
    std::string abr;
    int brushIdx = -1;
    size_t p = 0;
    while (p < line.size()) {
      const size_t tab = line.find('\t', p);
      const std::string field = line.substr(p, tab == std::string::npos ? std::string::npos : tab - p);
      p = tab == std::string::npos ? line.size() : tab + 1;
      const size_t eq = field.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = field.substr(0, eq), val = field.substr(eq + 1);
      if (key == "abr") { abr = PaintStore::unesc(val); continue; }
      if (key == "brush") { brushIdx = std::atoi(val.c_str()); continue; }
      if (Knob* k = knob(key.c_str())) k->set_value(std::atof(val.c_str()));
    }
    // The tip is only meaningful once its file is loaded, so do that first.
    if (!abr.empty()) {
      Knob* k = knob("abr_file");
      const char* cur = k ? k->get_text() : nullptr;
      if (k && (!cur || abr != cur)) { k->set_text(abr.c_str()); loadBrushes(true); }
    }
    if (brushIdx >= 0)
      if (Knob* k = knob("brush")) k->set_value(double(brushIdx));
    touchAll();
  }

  void resetBrush() {
    for (const BrushSetting& bs : kBrushSettings)
      if (Knob* k = knob(bs.knob)) k->set_value(bs.def);
    if (Knob* k = knob("preset")) k->set_value(0);
    touchAll();
  }

  // ---------------------------------------------------------------- layers

  static std::shared_ptr<rp::Canvas> cloneCanvas(const rp::Canvas& src) {
    auto c = std::make_shared<rp::Canvas>();
    std::shared_lock<std::shared_mutex> lock(src.mutex);
    for (const auto& kv : src.tiles) c->tiles[kv.first].reset(new rp::Tile(*kv.second));
    c->dirty = true;
    return c;
  }

  // Every frame this layer has paint on, in memory or on disk. Merging has to cover
  // all of them, not just the frame that happens to be on screen.
  std::vector<int> frameKeys(PaintStore& s, const Layer& l) {
    std::vector<int> keys;
    for (const auto& kv : l.canvases) keys.push_back(kv.first);
    std::error_code ec;
    if (fs::is_directory(s.dir, ec)) {
      char prefix[32];
      std::snprintf(prefix, sizeof(prefix), "paint.L%02d.", l.id);
      for (const auto& e : fs::directory_iterator(s.dir, ec)) {
        const std::string n = e.path().filename().generic_string();
        if (n.rfind(prefix, 0) != 0) continue;
        const std::string rest = n.substr(std::strlen(prefix));
        int key = PaintStore::kAllFrames;
        if (rest != "rpt" && std::sscanf(rest.c_str(), "%d", &key) != 1) continue;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
      }
    }
    if (keys.empty()) keys.push_back(frameKey());
    return keys;
  }

  // Applies a change to the layer stack and records it so Ctrl+Z walks back through it
  // alongside the strokes. Copying the stack is cheap: the canvases are shared pointers.
  void structuralChange(const std::function<void(PaintStore&)>& apply) {
    PaintStore& s = store();
    auto before = std::make_shared<std::vector<Layer>>();
    auto after = std::make_shared<std::vector<Layer>>();
    int beforeActive, beforeNext, afterActive, afterNext;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      *before = s.layers; beforeActive = s.active; beforeNext = s.nextId;
    }
    apply(s);
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      *after = s.layers; afterActive = s.active; afterNext = s.nextId;
    }
    PaintStore* sp = &s;
    auto atAfter = std::make_shared<bool>(true);
    s.engine.pushStructural([sp, before, after, atAfter, beforeActive, beforeNext, afterActive,
                             afterNext]() {
      std::lock_guard<std::mutex> lock(sp->mutex);
      const bool goBack = *atAfter;
      sp->layers = goBack ? *before : *after;
      sp->active = goBack ? beforeActive : afterActive;
      sp->nextId = goBack ? beforeNext : afterNext;
      *atAfter = !goBack;
    });
    commitHistory();
    syncLayerKnobs();
    touchAll();
  }

  // Pushes the stack into the knobs: menus, the active layer's fields, and the hidden
  // metadata string that saves with the script.
  void syncLayerKnobs() {
    PaintStore& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.layers.empty()) s.addLayer("Paint 1");
    if (s.active < 0 || s.active >= int(s.layers.size())) s.active = int(s.layers.size()) - 1;
    std::vector<std::string> names;
    for (const Layer& l : s.layers) {
      std::string n = l.name;
      if (!l.visible) n += "  (hidden)";
      else if (l.locked) n += "  (locked)";
      names.push_back(n);
    }
    for (const char* kn : {"layer", "layer2"})
      if (Knob* k = knob(kn)) {
        if (Enumeration_KnobI* e = k->enumerationKnob()) e->menu(names);
        k->set_value(double(s.active));
      }
    const Layer& l = s.layers[size_t(s.active)];
    if (Knob* k = knob("layer_name")) k->set_text(l.name.c_str());
    if (Knob* k = knob("layer_opacity")) k->set_value(l.opacity);
    if (Knob* k = knob("layer_blend")) k->set_value(double(l.blend));
    if (Knob* k = knob("layer_visible")) k->set_value(l.visible ? 1 : 0);
    if (Knob* k = knob("layer_locked")) k->set_value(l.locked ? 1 : 0);
  }

  void writeLayerData() {
    const std::string data = store().serialize();
    if (Knob* k = knob("layer_data")) k->set_text(data.c_str());
  }

  // Takes the per-layer knob values back into the active layer.
  // Reads a knob's live value. Inside knob_changed the member variables a knob writes
  // into have not necessarily been stored yet, so reading them there picks up the
  // PREVIOUS value - which showed up as a blend mode change doing nothing until the
  // next edit flushed it. The Knob itself is always current.
  double knobValue(const char* name, double fallback) const {
    Knob* k = knob(name);
    return k ? k->get_value() : fallback;
  }

  void pullLayerKnobs() {
    PaintStore& s = store();
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (s.layers.empty()) return;
      if (s.active < 0 || s.active >= int(s.layers.size())) return;
      Layer& l = s.layers[size_t(s.active)];
      if (Knob* k = knob("layer_name")) {
        const char* t = k->get_text();
        if (t && *t) l.name = t;
      }
      l.opacity = float(knobValue("layer_opacity", l.opacity));
      l.blend = std::max(0, std::min(int(knobValue("layer_blend", l.blend)),
                                     int(rp::kBlendCount) - 1));
      l.visible = knobValue("layer_visible", l.visible ? 1 : 0) != 0;
      l.locked = knobValue("layer_locked", l.locked ? 1 : 0) != 0;
    }
    syncLayerKnobs();
    writeLayerData();
    touchAll();
  }

  void selectLayer(int idx) {
    PaintStore& s = store();
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (s.layers.empty()) return;
      s.active = std::max(0, std::min(idx, int(s.layers.size()) - 1));
    }
    syncLayerKnobs();
    writeLayerData();
  }

  bool layerOp(const char* name) {
    PaintStore& s = store();
    if (!strcmp(name, "layer_add")) {
      structuralChange([](PaintStore& st) {
        Layer l;
        l.id = st.nextId++;
        l.name = "Paint " + std::to_string(l.id);
        st.layers.insert(st.layers.begin() + st.active + 1, std::move(l));
        ++st.active;
      });
    } else if (!strcmp(name, "layer_duplicate")) {
      structuralChange([](PaintStore& st) {
        Layer l = st.layers[size_t(st.active)];
        l.id = st.nextId++;
        l.name += " copy";
        for (auto& kv : l.canvases) kv.second = cloneCanvas(*kv.second);
        st.layers.insert(st.layers.begin() + st.active + 1, std::move(l));
        ++st.active;
      });
    } else if (!strcmp(name, "layer_delete")) {
      if (s.layers.size() <= 1) {
        warning("NPaintShop: a node always keeps at least one layer. Use 'Clear frame' "
                "to empty it.");
        return true;
      }
      structuralChange([](PaintStore& st) {
        st.layers.erase(st.layers.begin() + st.active);
        if (st.active >= int(st.layers.size())) st.active = int(st.layers.size()) - 1;
      });
    } else if (!strcmp(name, "layer_up") || !strcmp(name, "layer_down")) {
      const int dir = name[6] == 'u' ? 1 : -1;
      const int from = s.active, to = s.active + dir;
      if (to < 0 || to >= int(s.layers.size())) return true;
      structuralChange([from, to](PaintStore& st) {
        std::swap(st.layers[size_t(from)], st.layers[size_t(to)]);
        st.active = to;
      });
    } else if (!strcmp(name, "layer_merge")) {
      if (s.active <= 0) {
        warning("NPaintShop: the bottom layer has nothing to merge down into.");
        return true;
      }
      mergeLayers(s.active, s.active - 1, false);
    } else if (!strcmp(name, "layer_flatten")) {
      mergeLayers(-1, 0, true);
    } else {
      return false;
    }
    writeLayerData();
    return true;
  }

  // Merges `from` into `into` across every frame they hold paint on. With all=true,
  // every visible layer is merged down into the bottom one.
  void mergeLayers(int from, int into, bool all) {
    PaintStore& s = store();
    const std::string dir = paintDir();
    // Collect the work before mutating anything, so the undo snapshot is clean.
    struct Job { int src, dst; };
    std::vector<Job> jobs;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (all) {
        for (int i = int(s.layers.size()) - 1; i > 0; --i)
          if (s.layers[size_t(i)].contributes()) jobs.push_back(Job{i, 0});
      } else if (from > 0 && into >= 0) {
        jobs.push_back(Job{from, into});
      }
    }
    if (jobs.empty()) return;

    structuralChange([&](PaintStore& st) {
      for (const Job& j : jobs) {
        Layer& src = st.layers[size_t(j.src)];
        Layer& dst = st.layers[size_t(j.dst)];
        std::vector<int> keys = frameKeys(st, src);
        for (int key : keys) {
          std::shared_ptr<rp::Canvas> sc, dc;
          {
            // canvasFor takes the store mutex, which this lambda already holds in the
            // caller, so load directly here instead.
            auto it = src.canvases.find(key);
            if (it != src.canvases.end()) sc = it->second;
            else {
              sc = std::make_shared<rp::Canvas>();
              sc->load(PaintStore::fileFor(st.dir, src.id, key));
              src.canvases[key] = sc;
            }
            auto it2 = dst.canvases.find(key);
            if (it2 != dst.canvases.end()) dc = it2->second;
            else {
              dc = std::make_shared<rp::Canvas>();
              if (!dc->load(PaintStore::fileFor(st.dir, dst.id, key)) && j.dst == 0)
                dc->load(PaintStore::legacyFileFor(st.dir, key));
              dst.canvases[key] = dc;
            }
          }
          if (sc->empty()) continue;
          // The destination is replaced rather than edited in place, so the undo
          // snapshot keeps pointing at the original pixels.
          auto merged = cloneCanvas(*dc);
          std::shared_lock<std::shared_mutex> sl(sc->mutex);
          sc->mergeInto(*merged, src.opacity, src.blend);
          dst.canvases[key] = merged;
        }
      }
      std::vector<int> drop;
      for (const Job& j : jobs) drop.push_back(j.src);
      std::sort(drop.begin(), drop.end(), std::greater<int>());
      for (int i : drop) st.layers.erase(st.layers.begin() + i);
      st.active = std::max(0, std::min(st.active, int(st.layers.size()) - 1));
    });
    (void)dir;
  }

  // ---------------------------------------------------------------- history
  // Each finished stroke bumps the hidden stroke_index knob inside a Nuke undo step.
  // When Nuke undoes/redoes that knob, knob_changed() lands in syncHistory(), which
  // swaps the matching tile snapshots back in.
  void commitHistory() {
    PaintStore& s = store();
    if (Knob* k = knob("stroke_index")) {
      s.strokeIndex = int(k->get_value()) + 1;
      k->new_undo("NPaintShop stroke");
      k->set_value(s.strokeIndex);
    }
  }
  void stepHistory(int delta) {
    PaintStore& s = store();
    const int target = s.strokeIndex + delta;
    if (Knob* k = knob("stroke_index")) k->set_value(target);
    syncHistory(target);
  }
  void syncHistory(int target) {
    PaintStore& s = store();
    rp::IRect dirty, d;
    bool any = false;
    while (s.strokeIndex > target && s.engine.undo(&d)) { --s.strokeIndex; dirty.merge(d); any = true; }
    while (s.strokeIndex < target && s.engine.redo(&d)) { ++s.strokeIndex; dirty.merge(d); any = true; }
    s.strokeIndex = target;
    if (any) { syncLayerKnobs(); writeLayerData(); touch(dirty); }
  }

  // Tell Nuke the image changed: new hash (via paint_rev) and a priority update of the box.
  // Ask the viewer to show what has been painted.
  //
  // During a stroke this is rate-limited. Every mouse move used to kick off a fresh
  // render and cancel the one before it, which on a 16k plate means nothing ever
  // finishes: the cost is not the dab (a 2000 px brush is under 2 ms) but Nuke
  // re-rendering the frame tens of times a second. Coalescing the updates lets a
  // render actually complete, and the stroke still appears as it is drawn. The dirty
  // area accumulates meanwhile, so nothing is lost - and the end of a stroke always
  // flushes, so the final image is never stale.
  // Refresh everything. For layer changes, reloads, and the end of a stroke, where
  // the whole frame may have changed.
  void touchAll() {
    pendingWhole_ = true;
    flushViewer();
  }

  // Ask the viewer to show the region a stroke has just painted.
  //
  // An empty rect means the move placed no dab - most mouse moves, since a dab only
  // lands once the brush has travelled a spacing step. That used to fall through to a
  // full-frame refresh, so on a 16k plate every mouse wiggle re-rendered the entire
  // image. Now it does nothing, which is what "nothing changed" deserves.
  //
  // The region sent is the union of everything this stroke has painted, not just the
  // part since the last refresh. Renders get cancelled constantly while dragging, so
  // a region refreshed earlier may never have finished; asking for the whole stroke
  // each time repairs those instead of leaving a hard-edged rectangle of fresh pixels
  // in a field of stale ones.
  void touch(const rp::IRect& dirty) {
    if (dirty.empty()) return;
    strokeDirty_.merge(dirty);
    pendingDirty_.merge(dirty);

    PaintStore& s = store();
    if (s.drag == PaintStore::kStroke) {
      const int ms = std::max(0, int(knobValue("refresh_ms", refreshMs_)));
      const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - lastUpdate_)
                             .count();
      if (ms > 0 && since < ms) return;  // fold into the next one
      pendingDirty_ = strokeDirty_;
    }
    flushViewer();
  }

  void flushViewer() {
    lastUpdate_ = std::chrono::steady_clock::now();
    ++flushes_;
    const uint64_t e = ++store().edits;
    if (Knob* k = knob("paint_rev")) k->set_value(double(e % 1000000000ull));
    if (pendingWhole_ || pendingDirty_.empty()) asapUpdate();
    else
      asapUpdate(Box(pendingDirty_.x0, pendingDirty_.y0, pendingDirty_.x1, pendingDirty_.y1));
    pendingDirty_ = rp::IRect();
    pendingWhole_ = false;
  }

  rp::IRect pendingDirty_, strokeDirty_;
  bool pendingWhole_ = false;
  std::chrono::steady_clock::time_point lastUpdate_;

  // ---------------------------------------------------------------- viewer interaction
  void build_handles(ViewerContext* ctx) override {
    RPT("build_handles");
    store().uiOp = this;  // this instance is the one on screen: right frame, right inputs
    build_input_handles(ctx);
    build_knob_handles(ctx);
  }

  NPaintShop* uiOp() { PaintStore& s = store(); return s.uiOp ? s.uiOp : this; }
  int uiFrameKey() { return uiOp()->frameKey(); }

  static const Channel* rgbaChannels() {
    static const Channel ch[4] = {Chan_Red, Chan_Green, Chan_Blue, Chan_Alpha};
    return ch;
  }

  // Everything about the clone source that does not change during a stroke, worked out
  // once on mouse-down. Resolving the op, validating it and reading its bbox per tile
  // was pure overhead, and with a time offset it meant an inputAt() lookup per tile.
  //
  // Clone pixels are baked into the canvas at stroke time rather than re-rendered, so
  // the time offset can resolve a context directly instead of going through
  // split_input / inputContext, which exist to declare a *rendering* dependency.
  struct CloneSrc {
    Iop* op = nullptr;
    Box rod;
    bool hasAlpha = true;
    bool rowsLegal = false;  // _request declared this op, so row.get can see the pixels
  };
  CloneSrc cloneSrc_;

  void resolveCloneSource() {
    cloneSrc_ = CloneSrc();
    const int which = cloneInput_ == 1 && input(1) ? 1 : 0;
    if (!input(which)) return;
    const bool sameTime = std::fabs(double(cloneTime_)) < 1e-6;
    Iop* op = sameTime ? (which == 0 ? &input0() : dynamic_cast<Iop*>(input(1)))
                       : dynamic_cast<Iop*>(
                             inputAt(which, OutputContext(outputContext(),
                                                          outputContext().frame() + double(cloneTime_))));
    if (!op) return;
    try {
      op->validate(true);
      cloneSrc_.op = op;
      cloneSrc_.rod = op->info();
      cloneSrc_.hasAlpha = op->channels().contains(Chan_Alpha);
      // Only the untimed inputs are declared in _request. A time-offset source is a
      // different op in a different context, so rows there would read as zero.
      cloneSrc_.rowsLegal = sameTime;
    } catch (...) {
      cloneSrc_ = CloneSrc();
    }
  }

  // Pull a box of input pixels through the planar API. fetchPlane runs its own
  // validate / request / engine cycle for exactly the box asked for, so it can read the
  // input anywhere, at any frame. row.get cannot: it only sees the area declared in
  // _request before the input op was opened, and Interest::at is documented to return
  // zero outside "the intersection of the requested() area of the iop and the arguments
  // passed to the constructor". A clone offset points somewhere the viewer never asked
  // for, so the old row.get path read zeros - which the clone blend made opaque black.
  // `box` must already be clipped to the input's region of definition; fetchPlane
  // requires that.
  //
  // This is the correct-but-expensive route. Iop.h is explicit that it is uncached -
  // "multiple identical calls to fetchPlane will result in multiple identical calls to
  // doFetchPlane" - so every tile re-runs the upstream tree for its own box, bypassing
  // the row cache the viewer has already filled. Hence it is the fallback, not the
  // default.
  bool fetchInputPlane(Iop& in, const Box& box, int x, int y, float* rgba) {
    try {
      ImagePlane plane(box, false, Mask_RGBA);
      in.fetchPlane(plane);
      if (aborted()) { ++abortedFetches_; return false; }
      if (!plane.readable()) return false;
      const int stride = plane.colStride();
      for (int c = 0; c < 4; ++c) {
        const int cn = plane.chanNo(rgbaChannels()[c]);
        if (cn < 0) continue;
        for (int yy = box.y(); yy < box.t(); ++yy) {
          float* o = rgba + size_t(yy - y) * rp::TILE * 4 + size_t(box.x() - x) * 4 + c;
          // One index computation per row instead of per pixel: at() multiplies out
          // row/col/channel strides on every call, which at 65k pixels a tile is most
          // of the cost of the copy.
          const float* p = &plane.at(box.x(), yy, cn);
          for (int n = box.r() - box.x(); n > 0; --n, p += stride, o += 4) *o = *p;
        }
      }
      return true;
    } catch (...) {
      return false;  // "could not read", never black
    }
  }

  // Is a row read of `box` from `in` actually backed by data? Two things have to hold:
  // the box must lie inside what was requested, and every channel the input has must
  // have been requested too - an unrequested channel reads as zero, and for alpha that
  // would make the clone tool erase instead of paint.
  bool rowsTrustworthy(Iop& in, const Box& box) const {
    const Box& req = in.requestedBox();
    if (box.x() < req.x() || box.y() < req.y() || box.r() > req.r() || box.t() > req.t())
      return false;
    ChannelMask have = in.channels(), asked = in.requested_channels();
    for (int c = 0; c < 4; ++c) {
      const Channel z = rgbaChannels()[c];
      if (have.contains(z) && !asked.contains(z)) return false;
    }
    return true;
  }

  // The fast route, and the default for a same-frame source. row.get comes out of the
  // cache the viewer's own render has just populated, so during a drag these are mostly
  // hits. It is only legal because _request declares the area the clone offset reaches -
  // without that it silently reads zeros, which is what used to paint black.
  bool fetchInputRows(Iop& in, const Box& box, int x, int y, float* rgba) {
    try {
      Row row(box.x(), box.r());
      for (int yy = box.y(); yy < box.t(); ++yy) {
        row.get(in, yy, box.x(), box.r(), Mask_RGBA);
        if (aborted()) { ++abortedFetches_; return false; }
        float* base = rgba + size_t(yy - y) * rp::TILE * 4 + size_t(box.x() - x) * 4;
        for (int c = 0; c < 4; ++c) {
          const float* p = row[rgbaChannels()[c]] + box.x();
          float* o = base + c;
          for (int n = box.r() - box.x(); n > 0; --n, ++p, o += 4) *o = *p;
        }
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  // One tile of clone source. Pixels outside the input's region of definition are
  // reported invalid rather than filled with black - there is genuinely no image there,
  // and Photoshop likewise refuses to clone from undefined area.
  bool fetchInputTile(int tx, int ty, float* rgba, uint8_t* valid) {
    Iop* src = cloneSrc_.op;  // resolved and validated once per stroke
    if (!src) return false;
    Iop& in = *src;
    const int x = tx * rp::TILE, y = ty * rp::TILE;
    std::memset(rgba, 0, sizeof(float) * rp::TILE_PIXELS * 4);
    std::memset(valid, 0, rp::TILE_PIXELS);

    const Box& rod = cloneSrc_.rod;
    const bool haveAlpha = cloneSrc_.hasAlpha;
    Box box(std::max(x, rod.x()), std::max(y, rod.y()), std::min(x + rp::TILE, rod.r()),
            std::min(y + rp::TILE, rod.t()));
    // Wholly outside the image. That is a definite answer, not a failure, so it is
    // worth caching: every pixel stays invalid and the dab skips the lot.
    if (box.r() <= box.x() || box.t() <= box.y()) return true;

    // Cheap path first, but only where it can be trusted. row.get cannot distinguish
    // "this pixel is black" from "nobody requested this pixel", so asking it for
    // something outside the requested box is how the black blocks happened. Iop tells
    // us where that line is: requestedBox() is documented as the area beyond which
    // "the engine() will never be called". Inside it the rows come from the cache the
    // viewer just filled, which is the whole point of preferring this path.
    if (cloneSrc_.rowsLegal && rowsTrustworthy(in, box) && fetchInputRows(in, box, x, y, rgba))
      ++rowFetches_;
    else if (fetchInputPlane(in, box, x, y, rgba)) ++planeFetches_;
    else { ++failedFetches_; return false; }

    // A read that comes back completely empty - every channel zero, alpha included -
    // is a read that silently did nothing, not a black part of the plate. A genuinely
    // black region of an opaque image still has alpha 1, so this cannot mistake one
    // for the other, and an input that really is transparent there would clone as a
    // no-op anyway. Treating these as unreadable is what stops them landing as black.
    bool anyData = false;
    for (int yy = box.y(); yy < box.t() && !anyData; ++yy) {
      const float* o = rgba + size_t(yy - y) * rp::TILE * 4;
      for (int xx = box.x(); xx < box.r(); ++xx) {
        const float* px = o + size_t(xx - x) * 4;
        if (px[0] != 0.0f || px[1] != 0.0f || px[2] != 0.0f || px[3] != 0.0f) {
          anyData = true;
          break;
        }
      }
    }
    if (!anyData) { ++emptyFetches_; return false; }

    for (int yy = box.y(); yy < box.t(); ++yy) {
      uint8_t* v = valid + size_t(yy - y) * rp::TILE;
      float* o = rgba + size_t(yy - y) * rp::TILE * 4;
      for (int xx = box.x(); xx < box.r(); ++xx) {
        v[xx - x] = 1;
        // A plate with no alpha channel is opaque, not empty. Without this the clone
        // tool would erase on every RGB-only input instead of painting.
        if (!haveAlpha) o[(xx - x) * 4 + 3] = 1.0f;
      }
    }
    return true;
  }

  int planeFetches_ = 0, rowFetches_ = 0, failedFetches_ = 0, cacheHits_ = 0;
  int abortedFetches_ = 0, emptyFetches_ = 0;
  long long dabUs_ = 0;      // time inside the paint engine for this stroke
  int moves_ = 0, flushes_ = 0;
  std::chrono::steady_clock::time_point strokeStart_;

  static long long us(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  }
  long long fetchUs_ = 0, compositeUs_ = 0;
  std::string cloneReport_;   // backs the clone_status knob
  std::string strokeReport_;  // backs the stroke_status knob
  std::string memReport_;     // backs the memory_status knob

  // Tiles with the layer stack composited on, made fresh for each stroke.
  std::unordered_map<uint64_t, std::shared_ptr<rp::SrcTile>> strokeSrc_;

  // What the cached input tiles were read from. Anything that changes the pixels the
  // clone source produces has to appear here, or the cache would go stale.
  DD::Image::Hash cloneSourceHash() const {
    DD::Image::Hash h;
    h.reset();
    h.append(cloneInput_);
    h.append(cloneTime_);
    h.append(outputContext().frame());
    if (cloneSrc_.op) h.append(cloneSrc_.op->hash());
    return h;
  }

  // Input tile with the other visible layers composited onto it. Returns nullptr when
  // the input exists but could not be read, so the engine skips those pixels rather
  // than painting a hole - handing back a zeroed tile is what painted black.
  std::shared_ptr<const rp::SrcTile> fetchSourceTile(int tx, int ty) {
    const uint64_t key = rp::tileKey(tx, ty);
    auto it = strokeSrc_.find(key);
    if (it != strokeSrc_.end()) return it->second;

    PaintStore& s = store();
    std::shared_ptr<rp::SrcTile> raw;
    if (cloneSrc_.op) {
      raw = s.inputCache.find(key);
      if (raw) {
        ++cacheHits_;
      } else {
        const auto t0 = std::chrono::steady_clock::now();
        auto t = std::make_shared<rp::SrcTile>();
        const bool ok = fetchInputTile(tx, ty, t->px.px, t->ok);
        fetchUs_ += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        if (!ok) return nullptr;
        raw = s.inputCache.insert(key, std::move(t));
      }
      // Nothing to composite: hand back the cached tile directly. This is the usual
      // case - one paint layer - and it keeps a clone stroke free of any copying.
      if (cloneStack_.empty()) return raw;
    }

    const auto t1 = std::chrono::steady_clock::now();
    auto out = std::make_shared<rp::SrcTile>();
    if (raw) {
      *out = *raw;
    } else {
      // Nothing connected: transparent is the honest answer, and cloning still works
      // off whatever is painted on the other layers.
      std::memset(out->ok, 1, sizeof(out->ok));
    }
    for (const PaintStore::Shot& sh : cloneStack_) {
      std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
      const rp::Tile* t = sh.canvas->find(tx, ty);
      if (!t) continue;
      for (int i = 0; i < rp::TILE_PIXELS; ++i) {
        rp::blendPixel(sh.blend, sh.opacity, t->px + i * 4, out->px.px + i * 4);
        // Paint on a layer is a source in its own right, so an opaque painted pixel
        // makes a position cloneable even where the input has nothing.
        if (!out->ok[i] && t->px[i * 4 + 3] * sh.opacity >= 0.999f) out->ok[i] = 1;
      }
    }
    compositeUs_ += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t1)
                        .count();
    return strokeSrc_[key] = std::move(out);
  }

  // Says in the properties panel what the last clone stroke actually found behind the
  // brush. Photoshop refuses outright when the clone source is undefined; this is the
  // same information, after the fact, and it is the only way to tell "nothing happened
  // because the source is off the plate" from "nothing happened because of a bug".
  // Where a stroke's time actually went, in the properties panel. Without this the
  // only way to tell a slow dab loop from a slow viewer is to guess, and guessing has
  // cost enough already: "engine" is time inside this node laying pixels down, "fetch"
  // is reading the clone source, and the gap between those and "wall" is everything
  // else - Nuke re-rendering, re-validating, and the host's own event handling.
  // Pushes the memory knobs into the store. Cheap; called when a stroke starts, when
  // the panel opens and when one of the knobs changes. Never during a stroke.
  void applyMemoryLimits() {
    PaintStore& s = store();
    s.engine.setUndoBudget(size_t(std::max(64, int(knobValue("undo_memory", undoMB_)))) << 20);
    s.inputCache.budget = size_t(std::max(128, int(knobValue("clone_cache", cloneCacheMB_)))) << 20;
    const int frames = std::max(0, int(knobValue("frame_memory", frameMB_)));
    std::lock_guard<std::mutex> lock(s.mutex);
    s.frameBudget = size_t(frames) << 20;
  }

  // What the node is holding, in the properties panel. This is the measure-first rule
  // applied to memory: a tester on a 16 GB machine can report a number instead of
  // "it got slow", and the limits above stop being guesswork.
  void reportMemory() {
    PaintStore& s = store();
    if (s.engine.stroking()) return;
    const PaintStore::MemStats m = s.memStats();
    char b[192];
    std::snprintf(b, sizeof(b), "paint %zu MB (%d layer-frames), undo %zu MB, clone cache %zu MB",
                  m.paint >> 20, m.canvases, m.undo >> 20, m.clone >> 20);
    memReport_ = b;
    setStatus("memory_status", memReport_);
  }

  // Shows text in one of the read-only status fields. Compared against what the knob
  // already holds rather than a copy kept here: several instances of this Op share the
  // one knob, and a per-instance copy goes stale as soon as another instance writes.
  void setStatus(const char* name, const std::string& text) {
    Knob* k = knob(name);
    if (!k) return;
    const char* cur = k->get_text();
    if (!cur || text != cur) k->set_text(text.c_str());
  }

  void reportStroke() {
    const long long wallUs = us(strokeStart_);
    PaintStore& s = store();
    char b[384];
    std::snprintf(b, sizeof(b),
                  "%d moves in %.0f ms: engine %.0f (tip %.0f, scan %.0f, setup %.0f, "
                  "pixels %.0f), fetch %.0f, elsewhere %.0f  (%d refreshes, %zu tiles)",
                  moves_, wallUs / 1000.0, dabUs_ / 1000.0, s.engine.rasterUs() / 1000.0,
                  s.engine.scanUs() / 1000.0, s.engine.gatherUs() / 1000.0,
                  s.engine.pixelUs() / 1000.0, fetchUs_ / 1000.0,
                  std::max(0.0, (wallUs - dabUs_ - fetchUs_) / 1000.0), flushes_,
                  s.engine.canvasTiles());
    strokeReport_ = b;
    setStatus("stroke_status", strokeReport_);
    RPT("stroke: %s", strokeReport_.c_str());
    reportMemory();
    if (tool_ == int(rp::Tool::Clone)) reportCloneStroke();
  }

  void reportCloneStroke() {
    PaintStore& s = store();
    const int skipped = s.engine.bgMisses();
    const int fetched = planeFetches_ + rowFetches_;
    char buf[256];
    if (fetched == 0 && cacheHits_ == 0 && skipped == 0) {
      std::snprintf(buf, sizeof(buf), "no source needed");
    } else {
      // The timings are the point: they say whether a slow stroke is the input being
      // read, the layer stack being composited, or neither - in which case the cost is
      // somewhere else and guessing again would be a waste.
      int n = std::snprintf(buf, sizeof(buf), "%d tile%s read in %.0f ms", fetched,
                            fetched == 1 ? "" : "s", double(fetchUs_) / 1000.0);
      if (cacheHits_)
        n += std::snprintf(buf + n, sizeof(buf) - n, ", %d cached", cacheHits_);
      if (compositeUs_ > 1000)
        n += std::snprintf(buf + n, sizeof(buf) - n, ", %.0f ms layers",
                           double(compositeUs_) / 1000.0);
      if (skipped)
        n += std::snprintf(buf + n, sizeof(buf) - n, ", %d tile%s could not be read", skipped,
                           skipped == 1 ? "" : "s");
      if (emptyFetches_)
        std::snprintf(buf + n, sizeof(buf) - n, " (%d came back empty)", emptyFetches_);
    }
    cloneReport_ = buf;
    setStatus("clone_status", cloneReport_);  // only touches the knob when the text changes
    RPT("clone stroke: %s  (plane=%d rows=%d cached=%d failed=%d aborted=%d empty=%d "
        "retries=%d cache=%zu MB)",
        cloneReport_.c_str(), planeFetches_, rowFetches_, cacheHits_, failedFetches_,
        abortedFetches_, emptyFetches_, s.engine.bgRetries(), s.inputCache.bytes() >> 20);
  }

  static float pressure() {
    const float p = ViewerContext::pressure();
    return (p > 0.0f && p <= 1.0f) ? p : 1.0f;  // a mouse reports no pressure
  }

  // The brush as the knobs currently describe it. Used for painting and for the swatch,
  // so the preview can never drift from what a stroke actually lays down.
  rp::BrushParams currentParams() const {
    PaintStore& s = store();
    rp::BrushParams p;
    p.tool = rp::Tool(tool_);
    for (int i = 0; i < 3; ++i) p.color[i] = color_[i];
    p.size = size_; p.hardness = hardness_; p.opacity = opacity_; p.flow = flow_;
    p.spacing = spacing_; p.pressureSize = pressureSize_; p.pressureFlow = pressureFlow_;
    p.blend = std::max(0, std::min(brushBlend_, int(rp::kBlendCount) - 1));
    p.smoothing = smoothing_;
    p.smudgeStrength = smudgeStrength_;
    p.smudgeRate = smudgeRate_;
    if (brush_ > 0 && size_t(brush_ - 1) < s.abr.presets.size()) {
      const rp::BrushPreset& pr = s.abr.presets[size_t(brush_ - 1)];
      if (pr.tip >= 0 && size_t(pr.tip) < s.abr.tips.size()) p.tip = &s.abr.tips[size_t(pr.tip)];
    }

    rp::BrushDynamics& d = p.dyn;
    d.angle = dynAngle_;
    d.roundness = dynRound_;
    d.flipX = dynFlipX_; d.flipY = dynFlipY_;
    d.size.jitter = sizeJitter_; d.size.minimum = sizeMin_;
    d.size.control = sizeControl_; d.size.fadeSteps = sizeFade_;
    d.angleJitter = angleJitter_;
    d.angleControl = angleControl_; d.angleFadeSteps = angleFade_;
    d.round.jitter = roundJitter_; d.round.minimum = roundMin_;
    d.round.control = roundControl_; d.round.fadeSteps = roundFade_;
    d.flipXJitter = flipXJitter_; d.flipYJitter = flipYJitter_;
    d.scatter = scatter_; d.scatterBoth = scatterBoth_;
    d.count = count_; d.countJitter = countJitter_;
    d.opacity.jitter = opacityJitter_; d.opacity.minimum = opacityMin_;
    d.opacity.control = opacityControl_; d.opacity.fadeSteps = opacityFade_;
    d.flow.jitter = flowJitter_; d.flow.minimum = flowMin_;
    d.flow.control = flowControl_; d.flow.fadeSteps = flowFade_;
    d.seed = unsigned(dynSeed_ > 0 ? dynSeed_ : 1);
    return p;
  }

  void beginStroke(float x, float y) {
    PaintStore& s = store();
    loadBrushes(false);
    Layer& L = s.activeLayer();
    if (L.locked) {
      warning("NPaintShop: layer '%s' is locked.", L.name.c_str());
      return;
    }
    if (!L.visible) {
      warning("NPaintShop: layer '%s' is hidden, so the stroke would not be visible.",
              L.name.c_str());
      return;
    }
    if (proxied()) {
      warning("NPaintShop: painting is disabled while the viewer is proxied. The paint "
              "is stored at full resolution, so clone and smudge would bake "
              "proxy-resolution pixels into it permanently. Existing paint still renders "
              "correctly at any proxy scale; switch to full res to paint.");
      return;
    }
    rp::BrushParams p = currentParams();
    rp::BgFetch bg;
    const bool needsSource = p.tool == rp::Tool::Clone || p.tool == rp::Tool::Smudge;
    if (needsSource) {
      float dx = cloneOffset_[0], dy = cloneOffset_[1];
      if (s.cloneSrcPending) {  // first stroke after Ctrl+click fixes the offset
        dx = s.cloneSrcX - x; dy = s.cloneSrcY - y;
        s.cloneSrcPending = false;
        if (Knob* k = knob("clone_offset")) { k->set_value(dx, 0); k->set_value(dy, 1); }
      }
      if (p.tool == rp::Tool::Clone) {
        p.cloneDx = int(std::lround(dx));
        p.cloneDy = int(std::lround(dy));
      }
      // The clone source is the input with every other visible layer composited on it,
      // frozen for the stroke. The engine puts this layer's own pre-stroke pixels on
      // top, which is what stops a clone stroke smearing its own output.
      cloneStack_ = s.stackFor(frameKey(), paintDir(), &L);
      resolveCloneSource();
      s.inputCache.budget = size_t(std::max(128, int(knobValue("clone_cache", cloneCacheMB_)))) << 20;
      s.inputCache.validate(cloneSourceHash());
      strokeSrc_.clear();
      planeFetches_ = rowFetches_ = failedFetches_ = cacheHits_ = abortedFetches_ = 0;
      emptyFetches_ = 0;
      fetchUs_ = compositeUs_ = 0;
      bg = [this](int tx, int ty) { return fetchSourceTile(tx, ty); };
    }
    dabUs_ = 0;
    moves_ = flushes_ = 0;
    strokeDirty_ = rp::IRect();
    strokeStart_ = std::chrono::steady_clock::now();
    applyMemoryLimits();
    {
      const auto t0 = std::chrono::steady_clock::now();
      const rp::IRect d = s.engine.beginStroke(s.canvasFor(L, frameKey(), paintDir()), p, x, y,
                                               pressure(), bg);
      dabUs_ += us(t0);
      ++moves_;
      touch(d);
    }
  }

  // The composited colour at one pixel: the input with every visible layer on top.
  // Used by the eyedropper, so it picks what you can actually see.
  bool samplePixel(int px, int py, float rgb[3]) {
    float acc[4] = {0, 0, 0, 0};
    if (input(0)) {
      Iop& in = input0();
      in.validate(true);
      in.request(px, py, px + 1, py + 1, Mask_RGBA, 1);
      Row row(px, px + 1);
      row.get(in, py, px, px + 1, Mask_RGBA);
      if (aborted()) return false;
      static const Channel ch[4] = {Chan_Red, Chan_Green, Chan_Blue, Chan_Alpha};
      for (int c = 0; c < 4; ++c) acc[c] = row[ch[c]][px];
    }
    for (const PaintStore::Shot& sh : store().stackFor(frameKey(), paintDir())) {
      std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
      const rp::Tile* t = sh.canvas->find(rp::tileOf(px), rp::tileOf(py));
      if (!t) continue;
      const float* p =
          t->px + (size_t(py & rp::TILE_MASK) * rp::TILE + (px & rp::TILE_MASK)) * 4;
      rp::blendPixel(sh.blend, sh.opacity, p, acc);
    }
    // Un-premultiply for the colour knob. A plate with no alpha channel reads alpha 0,
    // so in that case the RGB is already unassociated and must be taken as it is.
    for (int c = 0; c < 3; ++c) rgb[c] = acc[3] > 0.0f ? acc[c] / acc[3] : acc[c];
    return true;
  }

  void pickColorAt(float x, float y) {
    float rgb[3];
    if (!samplePixel(int(std::floor(x)), int(std::floor(y)), rgb)) return;
    if (Knob* k = knob("color"))
      for (int c = 0; c < 3; ++c) k->set_value(rgb[c], c);
    store().colorPicking = false;
  }

  // Runs on the Op instance the viewer is showing.
  bool handle(ViewerContext* ctx, Knob* glue) {
    PaintStore& s = store();
    const float x = ctx->x(), y = ctx->y();
    switch (ctx->event()) {
      case MOVE:
        s.cursorX = x; s.cursorY = y; s.haveCursor = true;
        // Widget pixels, so the overlay can label whatever is under the pointer.
        // hitTest's own origin flip happens inside the overlay, so pass it raw.
        s.overlayHoverX = ViewerContext::mouse_x();
        s.overlayHoverY = ViewerContext::mouse_y();
        s.overlayHover = true;
        glue->redraw();
        return false;
      case KEY:
        return handleKey(ViewerContext::key(), ViewerContext::state(), glue);
      case PUSH: {
        if (ViewerContext::button() != 1) return false;
        const unsigned st = ViewerContext::state();
        if (st & (ALT | META)) return false;  // leave viewer navigation alone
        // The overlay gets first refusal: a click on the toolbar must not paint.
        if (s.overlay.overUI(ViewerContext::mouse_x(), ViewerContext::mouse_y())) {
          int brush = -1;
          const rpui::Action a =
              s.overlay.hitTest(ViewerContext::mouse_x(), ViewerContext::mouse_y(), brush);
          if (a == rpui::kResizePicker) {
            s.drag = PaintStore::kGrid;
            s.dragMouseX = ViewerContext::mouse_x();
            s.dragMouseY = ViewerContext::mouse_y();
            s.dragCols = s.overlay.gridCols();
            s.dragRows = s.overlay.gridRows();
            return true;
          }
          applyAction(a, brush, glue);
          s.drag = PaintStore::kNone;
          return true;
        }
        s.cursorX = x; s.cursorY = y; s.haveCursor = true;
        if (s.colorPicking) {  // armed by the eyedropper button
          pickColorAt(x, y);
          s.drag = PaintStore::kNone;
          glue->redraw();
          return true;
        }
        if (st & CTRL) {
          s.cloneSrcX = x; s.cloneSrcY = y; s.cloneSrcPending = true;
          s.drag = PaintStore::kNone;
          glue->redraw();
        } else if (st & SHIFT) {
          s.drag = PaintStore::kSize;
          s.dragMouseX = ViewerContext::mouse_x();
          s.dragSize = size_;
        } else {
          s.drag = PaintStore::kStroke;
          beginStroke(x, y);
        }
        return true;
      }
      case DRAG:
        s.cursorX = x; s.cursorY = y;
        if (s.drag == PaintStore::kStroke) {
          {
            const auto t0 = std::chrono::steady_clock::now();
            const rp::IRect d = s.engine.strokeTo(x, y, pressure());
            dabUs_ += us(t0);
            ++moves_;
            touch(d);
          }
        } else if (s.drag == PaintStore::kGrid) {
          // The grip is the panel's bottom-right corner: right grows columns, down
          // grows rows, by whole cells.
          const float cell = s.overlay.cellSize();
          const int dc = int(std::lround((ViewerContext::mouse_x() - s.dragMouseX) / cell));
          const int dr = int(std::lround((ViewerContext::mouse_y() - s.dragMouseY) / cell));
          s.overlay.setGrid(s.dragCols + dc, s.dragRows + dr);
          glue->redraw();
        } else if (s.drag == PaintStore::kSize) {
          const float v = std::max(1.0f, s.dragSize + float(ViewerContext::mouse_x() - s.dragMouseX));
          if (Knob* k = knob("size")) k->set_value(v);
          glue->redraw();
        }
        return true;
      case RELEASE: {
        const bool painted = s.drag == PaintStore::kStroke && s.engine.endStroke();
        s.drag = PaintStore::kNone;
        if (painted) {
          reportStroke();
          commitHistory();
          // Forced: the end of a stroke always reaches the viewer, however recently
          // the last refresh went out.
          touchAll();
        }
        return true;
      }
      default:
        return false;
    }
  }

  // Carries out a toolbar click or a hotkey.
  void applyAction(rpui::Action a, int brush, Knob* glue) {
    PaintStore& s = store();
    switch (a) {
      case rpui::kToolPaint:
      case rpui::kToolErase:
      case rpui::kToolClone:
      case rpui::kToolSmudge:
        if (Knob* k = knob("tool")) k->set_value(double(a - rpui::kToolPaint));
        break;
      case rpui::kTogglePicker:
        s.pickerOpen = !s.pickerOpen;
        if (s.pickerOpen) loadBrushes(false);
        break;
      case rpui::kPickColor:
        s.colorPicking = !s.colorPicking;
        break;
      case rpui::kToggleLayers:
        s.layersOpen = !s.layersOpen;
        break;
      case rpui::kSelectLayer:
        if (brush >= 0) selectLayer(brush);
        break;
      case rpui::kToggleLayerVis:
        if (brush >= 0) {
          {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (brush < int(s.layers.size()))
              s.layers[size_t(brush)].visible = !s.layers[size_t(brush)].visible;
          }
          syncLayerKnobs();
          writeLayerData();
          touchAll();
        }
        break;
      case rpui::kUndo: if (s.engine.canUndo()) stepHistory(-1); break;
      case rpui::kRedo: if (s.engine.canRedo()) stepHistory(+1); break;
      case rpui::kPagePrev: s.overlay.pagePrev(); break;
      case rpui::kPageNext: s.overlay.pageNext(); break;
      case rpui::kPickBrush:
        if (brush >= 0) {
          if (Knob* k = knob("brush")) k->set_value(double(brush));
          applyBrushFileSettings();
        }
        break;
      default: break;
    }
    if (glue) glue->redraw();
  }

  // Every shortcut needs Alt. Nuke's 2D viewer already owns the bare letters this node
  // would otherwise want - p pauses the viewer, b shows the blue channel, l plays
  // forward, i marks an in point, and [ and ] toggle the toolbars - so taking them
  // would break the viewer. Alt is nearly free: Nuke uses only Alt+G, I, O, P, U, W
  // and Z, none of which are used here.
  bool handleKey(unsigned key, unsigned state, Knob* glue) {
    PaintStore& s = store();
    if (!(state & ALT)) return false;         // leave every unmodified key to Nuke
    if (state & (CTRL | META)) return false;  // and anything with a second modifier
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    switch (key) {
      case 'b': applyAction(rpui::kToolPaint, -1, glue); return true;   // Brush
      case 'e': applyAction(rpui::kToolErase, -1, glue); return true;   // Eraser
      case 's': applyAction(rpui::kToolClone, -1, glue); return true;   // clone Stamp
      case 'u': applyAction(rpui::kToolSmudge, -1, glue); return true;  // smUdge ('s' is taken)
      case 'm': applyAction(rpui::kTogglePicker, -1, glue); return true;  // brush Menu
      case 'l': applyAction(rpui::kToggleLayers, -1, glue); return true;  // Layer list
      case 'c':  // Colour: sample under the cursor, like Photoshop's eyedropper
        if (s.haveCursor) { pickColorAt(s.cursorX, s.cursorY); if (glue) glue->redraw(); }
        return true;
      case '[':
      case ']': {
        const float f = key == '[' ? 0.8f : 1.25f;
        if (Knob* k = knob("size")) k->set_value(std::max(1.0, k->get_value() * f));
        if (glue) glue->redraw();
        return true;
      }
      default:
        return false;
    }
  }

  void drawViewerUI(ViewerContext* ctx) {
    PaintStore& s = store();
    drawCursor();
    rpui::Overlay::State st;
    st.tool = tool_;
    st.brush = brush_;
    st.pickerOpen = s.pickerOpen;
    st.layersOpen = s.layersOpen;
    st.colorPicking = s.colorPicking;
    for (int c = 0; c < 3; ++c) st.color[c] = color_[c];
    st.canUndo = s.engine.canUndo();
    st.canRedo = s.engine.canRedo();
    st.tips.reserve(s.abr.presets.size());
    for (const rp::BrushPreset& pr : s.abr.presets)
      st.tips.push_back(pr.tip >= 0 && size_t(pr.tip) < s.abr.tips.size()
                            ? &s.abr.tips[size_t(pr.tip)]
                            : nullptr);
    st.params = currentParams();
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      for (const Layer& l : s.layers)
        st.layers.push_back(rpui::LayerRow{l.name, l.visible, l.locked});
      st.activeLayer = s.active;
    }
    s.overlay.setHover(s.overlayHover, s.overlayHoverX, s.overlayHoverY);
    s.overlay.draw(st);
    (void)ctx;
  }

  void drawCursor() {
    PaintStore& s = store();
    if (!s.haveCursor) return;
    // The real tip shape, ghosted, so the cursor shows the dab that will be stamped,
    // rotation and roundness included.
    const rp::BrushParams p = currentParams();
    rp::DabShape sh;
    sh.diameter = std::max(size_, 1.0f);
    sh.hardness = hardness_;
    sh.angle = p.dyn.angle * 3.14159265f / 180.0f;
    sh.roundness = p.dyn.roundness;
    sh.flipX = p.dyn.flipX; sh.flipY = p.dyn.flipY;
    s.overlay.drawCursorTip(s.cursorX, s.cursorY, sh, p.tip);
    auto circle = [](float cx, float cy, float r) {
      glBegin(GL_LINE_LOOP);
      for (int i = 0; i < 64; ++i) {
        const float a = float(i) * 6.2831853f / 64.0f;
        glVertex2f(cx + r * std::cos(a), cy + r * std::sin(a));
      }
      glEnd();
    };
    const float r = std::max(size_ * 0.5f, 0.5f);
    glColor3f(0.0f, 0.0f, 0.0f);
    circle(s.cursorX, s.cursorY, r + 1.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    circle(s.cursorX, s.cursorY, r);
    if (tool_ == 2) {  // clone: show where the source is
      const float sx = s.cloneSrcPending ? s.cloneSrcX : s.cursorX + cloneOffset_[0];
      const float sy = s.cloneSrcPending ? s.cloneSrcY : s.cursorY + cloneOffset_[1];
      glColor3f(1.0f, 0.8f, 0.2f);
      circle(sx, sy, r);
      glBegin(GL_LINES);
      glVertex2f(sx - 6, sy); glVertex2f(sx + 6, sy);
      glVertex2f(sx, sy - 6); glVertex2f(sx, sy + 6);
      glEnd();
    }
  }

  // The NDK routes viewer mouse events through a knob, so this one exists only to
  // receive them. It also piggy-backs on script saving to write the paint files.
  class GlueKnob : public Knob {
    NPaintShop* op_;

   public:
    GlueKnob(Knob_Closure* kc, NPaintShop* op, const char* name) : Knob(kc, name), op_(op) {}
    // Deliberately unchanged by the rename: this string is written into saved
    // scripts, and a new one would leave existing scripts unable to restore the knob.
    const char* Class() const override { return "RasterPaintHandle"; }

    static bool handleCb(ViewerContext* ctx, Knob* k, int) {
      GlueKnob* g = static_cast<GlueKnob*>(k);
      return g->op_->uiOp()->handle(ctx, k);
    }
    bool build_handle(ViewerContext* ctx) override { return ctx->transform_mode() == VIEWER_2D; }
    void draw_handle(ViewerContext* ctx) override {
      const int e = ctx->event();
      if (e == DRAW_OPAQUE || e == PUSH || e == DRAG || e == RELEASE || e == MOVE) {
        // Lowest-priority handle covering the whole viewer, with mouse-move events.
        begin_handle(Knob::ANYWHERE_MOUSEMOVES, ctx, handleCb, 0, 0, 0, 0);
        end_handle(ctx);
        // A second handle so the viewer sends us key presses for the tool hotkeys.
        begin_handle(Knob::ANYWHERE_KEY_PRESSED, ctx, handleCb, 1, 0, 0, 0);
        end_handle(ctx);
      }
      if (ctx->draw_lines()) op_->uiOp()->drawViewerUI(ctx);
    }

    // Saved with the script so Nuke calls to_script() on every save / autosave:
    // that is the moment the paint files are flushed.
    bool not_default() const override { return true; }
    void to_script(std::ostream& o, const OutputContext*, bool) const override {
      op_->store().saveDirty();
      o << "1";
    }
    bool from_script(const char*) override { return true; }
  };

  // ---------------------------------------------------------------- image
  void append(Hash& hash) override {
    RPT("append() enter");
    // paint_rev already changes the hash per edit; this nonce just keeps hashes from an
    // earlier session (disk cache) from matching different paint. Deliberately a plain
    // process-wide value rather than the node's store: hashing runs on render threads
    // and must not be the thing that first creates per-node state.
    static const uint64_t n = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    RPT("append() got nonce");
    hash.append(int(n & 0x7fffffff));
    hash.append(int((n >> 31) & 0x7fffffff));
    hash.append(frameKey());
  }

  void _validate(bool forReal) override {
    RPT("_validate enter forReal=%d this=%p", int(forReal), (void*)this);
    copy_info();
    RPT("_validate after copy_info  bbox=%d,%d..%d,%d", info_.x(), info_.y(), info_.r(), info_.t());
    PaintStore& s = store();
    RPT("_validate after store()");
    const int key = frameKey();
    RPT("_validate frameKey=%d", key);
    const std::string dir = paintDir();
    RPT("_validate paintDir=%s", dir.c_str());
    stack_ = s.stackFor(key, dir);
    RPT("_validate stack of %zu layers", stack_.size());
    rp::IRect b;
    for (const PaintStore::Shot& sh : stack_) {
      std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
      b.merge(sh.canvas->bounds());
    }
    RPT("_validate bounds=%d,%d..%d,%d empty=%d", b.x0, b.y0, b.x1, b.y1, int(b.empty()));
    if (output_ == 1) {
      // Paint only: RGBA and nothing else, whether or not there is any paint, because
      // "no paint" has to come out as transparent black rather than as the input.
      // The box is the paint's own, grown by a pixel so its edge is empty - Nuke
      // repeats the edge of the box outwards, and tiles are often painted right up to
      // theirs.
      info_.channels(Mask_RGBA);
      set_out_channels(Mask_All);
      if (b.empty()) {
        info_.set(info_.x(), info_.y(), info_.x() + 1, info_.y() + 1);
      } else {
        const double px = proxyX(), py = proxyY();
        info_.set(int(std::floor(b.x0 * px)) - 1, int(std::floor(b.y0 * py)) - 1,
                  int(std::ceil(b.x1 * px)) + 1, int(std::ceil(b.y1 * py)) + 1);
      }
      RPT("_validate done (paint only)");
      return;
    }
    if (b.empty()) {
      set_out_channels(Mask_None);
      RPT("_validate done (no paint)");
      return;
    }
    const double sx = proxyX(), sy = proxyY();
    const int bx0 = int(std::floor(b.x0 * sx)), by0 = int(std::floor(b.y0 * sy));
    const int bx1 = int(std::ceil(b.x1 * sx)), by1 = int(std::ceil(b.y1 * sy));
    info_.set(std::min(info_.x(), bx0), std::min(info_.y(), by0), std::max(info_.r(), bx1),
              std::max(info_.t(), by1));
    info_.turn_on(Mask_RGBA);
    set_out_channels(Mask_RGBA);
    RPT("_validate done (has paint)");
  }

  static bool wantsRGBA(ChannelMask channels) {
    return channels.contains(Chan_Red) || channels.contains(Chan_Green) ||
           channels.contains(Chan_Blue) || channels.contains(Chan_Alpha);
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override {
    // Blending a layer needs the backdrop alpha even when only RGB was asked for, so
    // always pull the whole of RGBA once any of it is wanted.
    ChannelSet need(channels);
    if (wantsRGBA(channels)) need += Mask_RGBA;
    input0().request(x, y, r, t, need, count);

    // Clone reads the input at an offset from wherever the brush is, which is almost
    // never inside the area the viewer asked for, and Nuke will not let a request made
    // later widen that. So declare that area here, while the clone tool is selected:
    // the visible box shifted by the clone offset and padded by the brush radius, which
    // is exactly what a stroke drawn in the visible area can reach. Declaring the whole
    // input instead would also be correct but makes every render carry a frame-sized
    // request. A tool change calls invalidate() so this is re-run.
    if (tool_ == int(rp::Tool::Clone) || tool_ == int(rp::Tool::Smudge)) {
      const int pad = int(std::ceil(size_ * 0.5)) + 2;
      const int dx = int(std::lround(cloneOffset_[0])), dy = int(std::lround(cloneOffset_[1]));
      for (int i = 0; i < 2; ++i) {
        Iop* in = dynamic_cast<Iop*>(input(i));
        if (!in) continue;
        in->validate(true);
        const Box& b = in->info();
        if (b.w() <= 0 || b.h() <= 0) continue;
        Box want(x + dx - pad, y + dy - pad, r + dx + pad, t + dy + pad);
        want.set(std::max(want.x(), b.x()), std::max(want.y(), b.y()), std::min(want.r(), b.r()),
                 std::min(want.t(), b.t()));
        if (want.w() > 0 && want.h() > 0)
          in->request(want.x(), want.y(), want.r(), want.t(), Mask_RGBA, count);
      }
    }
  }

  void engine(int y, int x, int r, ChannelMask channels, Row& row) override {
    static const Channel ch[4] = {Chan_Red, Chan_Green, Chan_Blue, Chan_Alpha};
    const double sx = proxyX(), sy = proxyY();
    bool hasPaint = false;
    if (wantsRGBA(channels)) {
      for (const PaintStore::Shot& sh : stack_) {
        std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
        if (sh.canvas->rowHasPaintScaled(y, x, r, sx, sy)) { hasPaint = true; break; }
      }
    }
    if (output_ == 1) {
      // Paint only. The input is never read: the stack is composited onto transparent
      // black, so a row with no paint is simply empty.
      row.erase(channels);
      if (!hasPaint) return;
      const int n = r - x;
      static thread_local std::vector<float> only;
      only.assign(size_t(n) * 4, 0.0f);
      float* acc[4] = {only.data(), only.data() + n, only.data() + 2 * n, only.data() + 3 * n};
      for (const PaintStore::Shot& sh : stack_) {
        std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
        sh.canvas->blendRowScaled(y, x, r, acc, sh.opacity, sh.blend, sx, sy);
      }
      for (int c = 0; c < 4; ++c)
        if (channels.contains(ch[c]))
          std::memcpy(row.writable(ch[c]) + x, acc[c], size_t(n) * sizeof(float));
      return;
    }
    if (!hasPaint) {  // rows no layer touches cost nothing: straight pass-through
      row.get(input0(), y, x, r, channels);
      return;
    }

    ChannelSet need(channels);
    need += Mask_RGBA;
    Row in(x, r);
    in.get(input0(), y, x, r, need);  // fetched before taking any canvas lock
    ChannelSet others(channels);
    others -= Mask_RGBA;
    if (others) row.copy(in, others, x, r);

    // The stack is composited into a scratch row, bottom to top, over the input.
    // engine() runs on many threads at once, so the scratch cannot be a member.
    const int n = r - x;
    static thread_local std::vector<float> scratch;
    scratch.resize(size_t(n) * 4);
    float* acc[4] = {scratch.data(), scratch.data() + n, scratch.data() + 2 * n,
                     scratch.data() + 3 * n};
    for (int c = 0; c < 4; ++c) std::memcpy(acc[c], in[ch[c]] + x, size_t(n) * sizeof(float));
    for (const PaintStore::Shot& sh : stack_) {
      std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
      sh.canvas->blendRowScaled(y, x, r, acc, sh.opacity, sh.blend, sx, sy);
    }
    for (int c = 0; c < 4; ++c) {
      if (!channels.contains(ch[c])) continue;
      // Channels the user excluded pass the input through untouched.
      const bool painted = paintChannels_ == 0 || (paintChannels_ == 1 ? c < 3 : c == 3);
      const float* src = painted ? acc[c] : in[ch[c]] + x;
      std::memcpy(row.writable(ch[c]) + x, src, size_t(n) * sizeof(float));
    }
  }
};

static Iop* build(Node* node) { return new NPaintShop(node); }
const Iop::Description NPaintShop::d(CLASS, "Draw/NPaintShop", build);

// The node was called RasterPaint until now. Registering the old name against the same
// builder keeps every script that already references it loading, rather than coming up
// with an unknown-node error and losing its paint.
const Iop::Description NPaintShop::dLegacy("RasterPaint", "Draw/NPaintShop", build);
