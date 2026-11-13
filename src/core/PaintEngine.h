// NPaintShop core - stroke engine. Strokes are baked into the canvas tiles as they
// are drawn; the only history kept is copy-on-write tile snapshots for undo.
#pragma once
#include <deque>
#include <functional>

#include "Brush.h"
#include "Canvas.h"

namespace rp {

enum class Tool { Paint = 0, Erase = 1, Clone = 2, Smudge = 3 };

struct BrushParams {
  Tool tool = Tool::Paint;
  float color[3] = {1.0f, 1.0f, 1.0f};
  float size = 50.0f;       // diameter in pixels
  float hardness = 0.8f;    // round brush only
  float opacity = 1.0f;     // ceiling for the whole stroke (Photoshop "opacity")
  float flow = 1.0f;        // per-dab build-up (Photoshop "flow")
  float spacing = 0.1f;     // fraction of the diameter between dabs
  bool pressureSize = true;
  bool pressureFlow = false;
  int blend = kNormal;  // how the stroke combines with what is already on the layer
  float smoothing = 0.0f;  // 0 follows the pointer exactly, 1 lags far behind it
  const BrushTip* tip = nullptr;  // nullptr = round brush; must outlive the stroke
  int cloneDx = 0, cloneDy = 0;   // clone source = destination + offset

  // Smudge. `smudgeStrength` is how much of the carried colour lands on each dab, and
  // `smudgeRate` how quickly that colour is replaced by what the brush passes over:
  // 0 keeps the colour picked up at the start of the stroke and drags it the whole way
  // (Photoshop's "finger painting" feel), 1 replaces it at every dab so almost nothing
  // travels. The default sits between the two, which is what reads as smearing.
  float smudgeStrength = 0.6f;
  float smudgeRate = 0.35f;

  BrushDynamics dyn;
};

// One tile of clone source: premultiplied RGBA, plus a per-pixel record of which
// pixels actually have data behind them.
struct SrcTile {
  Tile px;
  uint8_t ok[TILE_PIXELS];
  SrcTile() { std::memset(ok, 0, sizeof(ok)); }
};

// Supplies background (input image) pixels for the clone tool: returns tile (tx, ty) of
// the image being cloned from, or nullptr.
//
// Ownership is shared, so the supplier can cache tiles across strokes and evict them
// whenever it likes without pulling the rug from under a stroke in progress. It used to
// hand back a raw pointer it still owned; when the cache hit its budget mid-stroke and
// dropped everything, the engine was left reading freed memory, which showed up as
// black tiles on images large enough to fill the cache.
//
// nullptr means "I could not read this tile", which is NOT the same as "this tile is
// black". The clone tool leaves pixels it has no source for untouched rather than
// painting them; conflating absent with black is what made a refused fetch show up as
// an opaque block the size of a tile. A tile that is legitimately outside the image is
// a definite answer, so it comes back non-null with every ok[] byte clear.
using BgFetch = std::function<std::shared_ptr<const SrcTile>(int tx, int ty)>;

class PaintEngine {
 public:
  // All calls must come from one thread (the UI thread). Renderers only touch Canvas.
  IRect beginStroke(std::shared_ptr<Canvas> canvas, const BrushParams& params, float x, float y,
                    float pressure, BgFetch bg = nullptr);
  IRect strokeTo(float x, float y, float pressure);  // returns the pixels that changed
  bool endStroke();                                  // false if the stroke changed nothing
  bool stroking() const { return bool(canvas_); }

  // Undoable "erase everything" for one canvas.
  IRect clear(std::shared_ptr<Canvas> canvas);

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  bool undo(IRect* dirty = nullptr, std::shared_ptr<Canvas>* which = nullptr);
  bool redo(IRect* dirty = nullptr, std::shared_ptr<Canvas>* which = nullptr);
  void clearHistory() { undo_.clear(); redo_.clear(); }

  // Records a structural change - a layer added, deleted, reordered or merged - in the
  // same history as the strokes, so one Ctrl+Z walks back through both. `swap` must be
  // its own inverse: calling it undoes the change, calling it again redoes it.
  void pushStructural(std::function<void()> swap);
  void setUndoBudget(size_t bytes) { budget_ = bytes; trim(); }
  size_t historyBytes() const;

  // Clone-source tiles that were used / could not be read during the last stroke. Only
  // used for diagnostics - a non-zero miss count means the clone source could not be
  // read and those pixels were left alone. bgRetries counts attempts rather than
  // tiles, which is what shows a refusal turning into a retry storm.
  // Tiles allocated on the layer being painted. Shown in the stroke report, because a
  // stroke that feels slow on a huge plate is worth telling apart from one that has
  // simply covered a lot of ground.
  size_t canvasTiles() const { return canvasTiles_; }

  // Where a stroke's time went inside the engine, in microseconds, reset per stroke:
  // finding which tiles the dab covers, the serial bookkeeping that allocates them,
  // the pixel work itself, and rasterising the tip's coverage.
  long long scanUs() const { return scanUs_; }
  long long gatherUs() const { return gatherUs_; }
  long long pixelUs() const { return pixelUs_; }
  long long rasterUs() const { return rasterUs_; }

  int bgHits() const { return bgHit_; }
  int bgMisses() const { return int(missedEver_.size()); }
  int bgRetries() const { return bgMiss_; }

 private:
  // Per-pixel stroke state, one tile's worth.
  struct MaskTile {
    float a[TILE_PIXELS];
    MaskTile() { std::memset(a, 0, sizeof(a)); }
  };
  using MaskMap = std::unordered_map<uint64_t, std::unique_ptr<MaskTile>>;
  struct Entry {
    std::shared_ptr<Canvas> canvas;
    TileMap tiles;  // tile contents to swap in; a null value means "no tile here"
    std::function<void()> swapFn;  // set instead of tiles for a structural change
    size_t bytes() const;
  };

  // One tile of a dab, resolved serially (the map lookups and allocations are not
  // thread safe) so the pixel work itself can be handed out across cores.
  struct TileJob {
    IRect tr;
    const Tile* bef;
    Tile* cur;
    MaskTile* ms;
    MaskTile* as;
  };
  void dabTile(const TileJob& job, const IRect& box, int bw, float opacity, float flow);

  float rand01();
  float dynFactor(const DynControl& d, float pressure);
  // Applies the dynamics and lays down this step's dabs. `dir` is the stroke heading.
  IRect stamp(float cx, float cy, float pressure, float dir);
  IRect oneDab(float cx, float cy, const DabShape& shape, float opacity, float flow);
  // Clone source for one row. `valid[i]` is set to 0 for pixels with no readable
  // source, and those pixels must be left untouched by the caller.
  void fetchSourceRow(int sy, int sx, int n, float* out, uint8_t* valid);
  const SrcTile* bgTile(int tx, int ty);
  static IRect swap(Entry& e);
  void trim();

  std::shared_ptr<Canvas> canvas_;
  BrushParams p_;
  BgFetch bgFetch_;
  TileMap before_;  // pre-stroke state of every tile this stroke touched
  MaskMap mask_;   // coverage that flow builds up
  // Alpha actually achieved, so a later faint dab cannot pull an earlier solid one back
  // down. Only allocated when the opacity varies per dab - without it the achieved alpha
  // is just mask * opacity, and the common case pays nothing.
  MaskMap maskA_;
  bool useA_ = false;
  // clone: the supplier's tiles, held for the stroke so one map lookup replaces a
  // std::function call per row segment. Holding a reference rather than a bare pointer
  // is what makes it safe for the supplier to evict mid-stroke. Only successful
  // fetches are remembered, so a tile refused once can be asked for again.
  std::unordered_map<uint64_t, std::shared_ptr<const SrcTile>> bg_;
  // Tiles the supplier refused. miss_ is cleared on every mouse move, which is the
  // retry cadence: often enough that a tile blocked by one cancelled render is picked
  // up moments later, rare enough that a refusal cannot be re-attempted hundreds of
  // times inside a single dab.
  std::unordered_set<uint64_t> miss_;
  std::unordered_set<uint64_t> missedEver_;
  // Smudge carries one colour along the stroke rather than dragging pixels one by one.
  // Per-pixel dragging would need a snapshot of the dab's whole source region before
  // every dab - 64 MB for a 2000 px brush - which is exactly the kind of cost this
  // node has been fighting. A single carried colour is what MyPaint and GIMP do, it
  // reads as smearing, and it is free.
  float smudge_[4] = {0, 0, 0, 0};
  bool smudgeHas_ = false;

  // Coverage-weighted average of what the dab is sitting on, for smudge pickup.
  bool sampleUnder(const IRect& box, int bw, float out[4]);

  std::vector<TileJob> jobs_;  // the tiles of the dab being laid down
  std::vector<float> cov_, src_;
  std::vector<uint8_t> vld_;  // clone: which pixels of src_ have a readable source
  int bgHit_ = 0, bgMiss_ = 0;
  size_t canvasTiles_ = 0;
  long long scanUs_ = 0, gatherUs_ = 0, pixelUs_ = 0, rasterUs_ = 0;
  std::vector<uint8_t> covered_;  // which tiles of the dab's bbox the coverage reaches
  float lx_ = 0, ly_ = 0, lp_ = 1, acc_ = 0, dir_ = 0;
  float rawX_ = 0, rawY_ = 0;  // where the pointer actually is, before smoothing
  IRect strokeTowards(float x, float y, float pressure);
  unsigned rng_ = 1;
  int dabIndex_ = 0;  // dabs laid down so far, drives the fade ramps

  std::deque<Entry> undo_, redo_;
  size_t budget_ = size_t(1) << 30;
};

}  // namespace rp
