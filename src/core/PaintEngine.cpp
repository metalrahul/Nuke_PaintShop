#include "PaintEngine.h"

#include <chrono>

#include "Parallel.h"

#include <cmath>
#include <mutex>

namespace rp {

namespace {
inline long long elapsedUs(const std::chrono::steady_clock::time_point& t0) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}
}  // namespace



static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool BrushDynamics::any() const {
  return angle != 0.0f || roundness != 1.0f || flipX || flipY || flipXJitter || flipYJitter ||
         size.jitter > 0.0f || size.control != 0 || round.jitter > 0.0f || round.control != 0 ||
         angleJitter > 0.0f || angleControl != kAngleOff || scatter > 0.0f || count > 1 ||
         opacity.jitter > 0.0f || opacity.control != 0 || flow.jitter > 0.0f || flow.control != 0;
}

// xorshift32: cheap, and seeded per stroke so a stroke always plays out the same way.
float PaintEngine::rand01() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return float(rng_ & 0xffffffu) * (1.0f / 16777216.0f);
}

float PaintEngine::dynFactor(const DynControl& d, float pressure) {
  float v = 1.0f;
  if (d.jitter > 0.0f) v = 1.0f - clamp01(d.jitter) * rand01();
  switch (d.control) {
    case 1: {  // fade
      const float n = float(d.fadeSteps > 0 ? d.fadeSteps : 1);
      v *= std::max(0.0f, 1.0f - float(dabIndex_) / n);
      break;
    }
    case 2: v *= clamp01(pressure); break;
    default: break;
  }
  const float mn = clamp01(d.minimum);
  return mn + (1.0f - mn) * clamp01(v);
}

size_t PaintEngine::Entry::bytes() const {
  if (swapFn) return 256;  // the canvases it refers to are shared, not copied
  size_t n = 0;
  for (const auto& kv : tiles) n += kv.second ? sizeof(Tile) : 16;
  return n;
}

size_t PaintEngine::historyBytes() const {
  size_t n = 0;
  for (const auto& e : undo_) n += e.bytes();
  for (const auto& e : redo_) n += e.bytes();
  return n;
}

void PaintEngine::trim() {
  size_t total = historyBytes();
  while (total > budget_ && !redo_.empty()) { total -= redo_.front().bytes(); redo_.pop_front(); }
  while (total > budget_ && undo_.size() > 1) { total -= undo_.front().bytes(); undo_.pop_front(); }
}

// ---------------------------------------------------------------- stroke

IRect PaintEngine::beginStroke(std::shared_ptr<Canvas> canvas, const BrushParams& params, float x,
                               float y, float pressure, BgFetch bg) {
  if (canvas_) endStroke();
  canvas_ = std::move(canvas);
  p_ = params;
  p_.opacity = clamp01(p_.opacity);
  p_.flow = clamp01(p_.flow);
  p_.spacing = std::max(p_.spacing, 0.01f);
  bgFetch_ = std::move(bg);
  before_.clear(); mask_.clear(); maskA_.clear(); bg_.clear(); miss_.clear(); missedEver_.clear();
  bgHit_ = bgMiss_ = 0;
  smudgeHas_ = false;  // each stroke starts by picking up fresh colour
  scanUs_ = gatherUs_ = pixelUs_ = rasterUs_ = 0;
  useA_ = p_.dyn.opacity.jitter > 0.0f || p_.dyn.opacity.control != 0;
  lx_ = x; ly_ = y; lp_ = pressure; acc_ = 0.0f; dir_ = 0.0f;
  rawX_ = x; rawY_ = y;
  rng_ = p_.dyn.seed ? p_.dyn.seed : 1u;
  dabIndex_ = 0;
  if (!canvas_) return IRect();
  std::unique_lock<std::shared_mutex> lock(canvas_->mutex);
  return stamp(x, y, pressure, dir_);
}

// Stabilisation: the brush chases the pointer instead of following it exactly, which
// takes the wobble out of a freehand line. The pointer's true position is kept so the
// end of the stroke can catch up to it rather than stopping short.
IRect PaintEngine::strokeTo(float x, float y, float pressure) {
  if (!canvas_) return IRect();
  // A new mouse move is the retry cadence for clone tiles the supplier could not
  // read last time: conditions may have changed, and once per move is often enough
  // to fill them in without re-attempting inside every dab.
  miss_.clear();
  rawX_ = x;
  rawY_ = y;
  const float s = p_.smoothing < 0.0f ? 0.0f : (p_.smoothing > 1.0f ? 1.0f : p_.smoothing);
  if (s > 0.0f) {
    const float k = 1.0f - s * 0.92f;  // never below 0.08, or the brush would stall
    x = lx_ + (x - lx_) * k;
    y = ly_ + (y - ly_) * k;
  }
  return strokeTowards(x, y, pressure);
}

IRect PaintEngine::strokeTowards(float x, float y, float pressure) {
  IRect dirty;
  if (!canvas_) return dirty;
  const float dx = x - lx_, dy = y - ly_;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0.0f) { lp_ = pressure; return dirty; }
  std::unique_lock<std::shared_mutex> lock(canvas_->mutex);
  dir_ = std::atan2(dy, dx);
  float t = 0.0f;
  for (;;) {
    const float pr = lp_ + (pressure - lp_) * (t / len);
    const float diam = p_.size * (p_.pressureSize ? pr : 1.0f);
    const float step = std::max(p_.spacing * diam, 0.5f);
    const float need = step - acc_;
    if (t + need > len) { acc_ += len - t; break; }
    t += need;
    acc_ = 0.0f;
    const float f = t / len;
    dirty.merge(stamp(lx_ + dx * f, ly_ + dy * f, lp_ + (pressure - lp_) * f, dir_));
  }
  lx_ = x; ly_ = y; lp_ = pressure;
  return dirty;
}

const SrcTile* PaintEngine::bgTile(int tx, int ty) {
  if (!bgFetch_) return nullptr;
  const uint64_t key = tileKey(tx, ty);
  auto it = bg_.find(key);
  if (it != bg_.end()) return it->second.get();
  // Already refused during this mouse move, so don't ask again. fetchSourceRow calls
  // this once per tile segment per row per dab - about 900 times for a 300 px brush,
  // ~100k times across a stroke - and with no memo every one of those re-attempted a
  // real input read. Each costs only microseconds when the tree is aborted, but at
  // that count it added most of a second of lag to a single stroke.
  if (miss_.count(key)) return nullptr;
  std::shared_ptr<const SrcTile> t = bgFetch_(tx, ty);
  if (!t) {
    // Remembered only until the next mouse move. A refusal is transient - the render
    // it would have come from was cancelled - so a tile must get another chance, but
    // once per move is plenty. Remembering it for the whole stroke is what used to
    // leave a tile-shaped block behind.
    ++bgMiss_;
    miss_.insert(key);
    missedEver_.insert(key);
    return nullptr;
  }
  ++bgHit_;
  // The map keeps the tile alive for the stroke, so handing out a raw pointer from
  // here costs no refcounting on the hot path.
  return (bg_[key] = std::move(t)).get();
}

// Clone source for one row: (pre-stroke paint) OVER (input image). Sampling the
// pre-stroke state keeps a clone stroke from smearing its own output. Composited
// straight into the caller's buffer: resolving whole tiles up front and copying out
// of them was measurably slower, because both ways cost one pass over the dab and
// the tile version adds the copy.
void PaintEngine::fetchSourceRow(int sy, int sx, int n, float* out, uint8_t* valid) {
  const int ty = tileOf(sy), row = sy & TILE_MASK;
  int x = sx;
  const int end = sx + n;
  while (x < end) {
    const int tx = tileOf(x);
    const int segEnd = std::min(end, (tx + 1) * TILE);
    const int cnt = segEnd - x;
    const uint64_t key = tileKey(tx, ty);
    auto it = before_.find(key);
    const Tile* paint = it != before_.end() ? it->second.get() : canvas_->find(tx, ty);
    const SrcTile* bg = bgTile(tx, ty);
    const size_t off = (size_t(row) * TILE + (x & TILE_MASK)) * 4;
    float* o = out + size_t(x - sx) * 4;
    uint8_t* v = valid + (x - sx);
    if (bgFetch_ && !bg) {
      std::memset(o, 0, size_t(cnt) * 4 * sizeof(float));
      std::memset(v, 0, size_t(cnt));
      x = segEnd;
      continue;
    }
    const size_t vOff = size_t(row) * TILE + (x & TILE_MASK);
    if (bg) std::memcpy(v, bg->ok + vOff, size_t(cnt));
    else std::memset(v, 1, size_t(cnt));
    if (paint && bg) {
      const float* p = paint->px + off;
      const float* b = bg->px.px + off;
      for (int i = 0, j = 0; i < cnt * 4; i += 4, ++j) {
        const float pa = p[i + 3];
        if (!v[j]) {
          o[i] = p[i]; o[i + 1] = p[i + 1]; o[i + 2] = p[i + 2]; o[i + 3] = pa;
          if (pa >= 0.999f) v[j] = 1;
          continue;
        }
        const float k = 1.0f - pa;
        o[i] = p[i] + b[i] * k; o[i + 1] = p[i + 1] + b[i + 1] * k;
        o[i + 2] = p[i + 2] + b[i + 2] * k; o[i + 3] = pa + b[i + 3] * k;
      }
    } else if (bg) {
      std::memcpy(o, bg->px.px + off, size_t(cnt) * 4 * sizeof(float));
    } else if (paint) {
      std::memcpy(o, paint->px + off, size_t(cnt) * 4 * sizeof(float));
    } else {
      std::memset(o, 0, size_t(cnt) * 4 * sizeof(float));
    }
    x = segEnd;
  }
}

// One spacing step: applies the dynamics and lays down this step's dabs. Scatter and
// count make a single step produce several dabs at jittered positions and shapes.
IRect PaintEngine::stamp(float cx, float cy, float pressure, float dir) {
  pressure = clamp01(pressure);
  const BrushDynamics& d = p_.dyn;
  IRect dirty;

  int n = d.count > 1 ? d.count : 1;
  if (n > 1 && d.countJitter > 0.0f) {
    const int lo = std::max(1, int(std::lround(float(n) * (1.0f - clamp01(d.countJitter)))));
    n = lo + int(rand01() * float(n - lo + 1));
    n = std::max(1, std::min(n, d.count));
  }

  for (int i = 0; i < n; ++i) {
    DabShape sh;
    const float base = p_.size * (p_.pressureSize ? pressure : 1.0f);
    sh.diameter = base * dynFactor(d.size, pressure);
    // Once the dynamics shrink a dab below a pixel it leaves nothing but specks, so
    // stop stamping - that is what makes a size fade actually end. A brush the user
    // set this small in the first place still paints.
    if (sh.diameter < 0.8f && base >= 0.8f) continue;
    sh.diameter = std::max(sh.diameter, 0.5f);
    sh.hardness = p_.hardness;
    sh.roundness = clamp01(d.roundness) * dynFactor(d.round, pressure);

    float ang = d.angle * 3.14159265f / 180.0f;
    if (d.angleControl == kAngleDirection) {
      ang += dir;
    } else if (d.angleControl == kAngleFade) {
      const float steps = float(d.angleFadeSteps > 0 ? d.angleFadeSteps : 1);
      ang += 6.2831853f * std::min(1.0f, float(dabIndex_) / steps);
    } else if (d.angleControl == kAnglePressure) {
      ang += 6.2831853f * pressure;
    }
    if (d.angleJitter > 0.0f)
      ang += (rand01() - 0.5f) * 2.0f * clamp01(d.angleJitter) * 3.14159265f;
    sh.angle = ang;

    sh.flipX = d.flipX ^ (d.flipXJitter && rand01() < 0.5f);
    sh.flipY = d.flipY ^ (d.flipYJitter && rand01() < 0.5f);

    const float opacity = clamp01(p_.opacity * dynFactor(d.opacity, pressure));
    const float flow = clamp01(p_.flow * (p_.pressureFlow ? pressure : 1.0f) *
                               dynFactor(d.flow, pressure));

    float px = cx, py = cy;
    if (d.scatter > 0.0f) {
      // Across the stroke, and optionally along it too.
      const float s = d.scatter * sh.diameter;
      const float off = (rand01() - 0.5f) * 2.0f * s;
      px += -std::sin(dir) * off;
      py += std::cos(dir) * off;
      if (d.scatterBoth) {
        const float along = (rand01() - 0.5f) * 2.0f * s;
        px += std::cos(dir) * along;
        py += std::sin(dir) * along;
      }
    }
    dirty.merge(oneDab(px, py, sh, opacity, flow));
  }
  ++dabIndex_;
  return dirty;
}

// One brush dab. For every covered pixel:
//   m   = m + (1 - m) * coverage * flow        (stroke mask, builds up with flow)
//   a   = max(a, m * opacity)                  (opacity caps it; max keeps a monotonic
//                                               so a jittered faint dab cannot undo an
//                                               earlier solid one)
//   out = lerp(pixel before the stroke, tool result, a)
// Blending against the pre-stroke snapshot is what gives Photoshop-style opacity
// without ever storing the stroke itself.
IRect PaintEngine::oneDab(float cx, float cy, const DabShape& shape, float opacity, float flow) {
  if (flow <= 0.0f || opacity <= 0.0f) return IRect();
  const IRect box = dabBounds(p_.tip, cx, cy, shape);
  const int bw = box.x1 - box.x0, bh = box.y1 - box.y0;
  if (bw <= 0 || bh <= 0) return IRect();
  cov_.resize(size_t(bw) * bh);
  const auto tRaster = std::chrono::steady_clock::now();
  rasterizeDab(p_.tip, cx, cy, shape, box, cov_.data());
  rasterUs_ += elapsedUs(tRaster);
  if (p_.tool == Tool::Clone) { src_.resize(size_t(TILE) * 4); vld_.resize(size_t(TILE)); }

  const float cr = p_.color[0], cg = p_.color[1], cb = p_.color[2];
  const int blendMode = p_.blend;
  static const Tile kZero;
  IRect dirty;
  jobs_.clear();

  // Which tiles the dab's coverage actually reaches. A round dab misses about a fifth
  // of its own bounding box and the corners are not worth allocating tiles for, but
  // finding that out is a second full pass over the dab. It used to run on one thread
  // while the rest of the machine sat idle; now it runs across cores like everything
  // else, and the serial pass below only visits tiles that survived it.
  const int tx0 = tileOf(box.x0), tx1 = tileOf(box.x1 - 1);
  const int ty0 = tileOf(box.y0), ty1 = tileOf(box.y1 - 1);
  const int ntx = tx1 - tx0 + 1, nty = ty1 - ty0 + 1;
  const auto tScan = std::chrono::steady_clock::now();
  covered_.assign(size_t(ntx) * size_t(nty), 0);
  parallelFor(ntx * nty, size_t(bw) * size_t(bh), [&](int k) {
    const int tx = tx0 + k % ntx, ty = ty0 + k / ntx;
    const int rx0 = std::max(box.x0, tx * TILE), rx1 = std::min(box.x1, (tx + 1) * TILE);
    const int ry0 = std::max(box.y0, ty * TILE), ry1 = std::min(box.y1, (ty + 1) * TILE);
    for (int y = ry0; y < ry1; ++y) {
      const float* c = cov_.data() + size_t(y - box.y0) * bw + (rx0 - box.x0);
      for (int i = 0, m = rx1 - rx0; i < m; ++i)
        if (c[i] > 0.0f) { covered_[size_t(k)] = 1; return; }
    }
  });
  scanUs_ += elapsedUs(tScan);

  const auto tGather = std::chrono::steady_clock::now();
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      if (!covered_[size_t(ty - ty0) * size_t(ntx) + size_t(tx - tx0)]) continue;
      const IRect tr{std::max(box.x0, tx * TILE), std::max(box.y0, ty * TILE),
                     std::min(box.x1, (tx + 1) * TILE), std::min(box.y1, (ty + 1) * TILE)};

      const uint64_t key = tileKey(tx, ty);
      auto bit = before_.find(key);
      if (bit == before_.end()) {
        const Tile* existing = canvas_->find(tx, ty);
        bit = before_.emplace(key, existing ? std::unique_ptr<Tile>(new Tile(*existing)) : nullptr).first;
      }
      const Tile* bef = bit->second ? bit->second.get() : &kZero;
      Tile* cur = canvas_->getOrCreate(tx, ty);
      auto& ms = mask_[key];
      if (!ms) ms.reset(new MaskTile);
      MaskTile* as = nullptr;
      if (useA_) {
        auto& slot = maskA_[key];
        if (!slot) slot.reset(new MaskTile);
        as = slot.get();
      }

      jobs_.push_back(TileJob{tr, bef, cur, ms.get(), as});
      dirty.merge(tr);
    }
  }
  // Tiles of one dab touch disjoint pixels, so they run across cores once the dab is
  // big enough to pay for the threads. Clone stays serial: its source fetch reaches
  // back into the host's image tree, which is not thread safe.
  gatherUs_ += elapsedUs(tGather);

  // Smudge picks up colour before it puts any down, so this has to read the canvas
  // while the dab's own pixels are still untouched. The carried colour then moves
  // towards what the brush is passing over at `smudgeRate`, which is what makes a
  // smudge stroke fade from the colour it started on into the colour it ends on.
  if (p_.tool == Tool::Smudge) {
    float under[4];
    if (sampleUnder(box, bw, under)) {
      if (!smudgeHas_) {
        std::memcpy(smudge_, under, sizeof(smudge_));
        smudgeHas_ = true;
      } else {
        const float r = std::min(std::max(p_.smudgeRate, 0.0f), 1.0f);
        for (int k = 0; k < 4; ++k) smudge_[k] += (under[k] - smudge_[k]) * r;
      }
    }
  }

  const auto tPixels = std::chrono::steady_clock::now();
  const size_t pixels = p_.tool == Tool::Clone ? 0 : size_t(bw) * size_t(bh);
  parallelFor(int(jobs_.size()), pixels,
              [&](int j) { dabTile(jobs_[j], box, bw, opacity, flow); });
  pixelUs_ += elapsedUs(tPixels);

  if (!dirty.empty()) canvas_->dirty = true;
  canvasTiles_ = canvas_->tiles.size();
  return dirty;
}


// The pixel work for one tile of a dab. Writes only into that tile's own buffers, so
// tiles of the same dab are independent and can run on different cores.
// Coverage-weighted average of what the dab is sitting on: this layer's live pixels
// over whatever the supplier gives for the same tile, so smudging a plate through a
// still-empty paint layer picks up the plate rather than nothing.
bool PaintEngine::sampleUnder(const IRect& box, int bw, float out[4]) {
  struct Part {
    double c[4] = {0, 0, 0, 0};
    double w = 0;
  };
  std::vector<Part> parts(jobs_.size());
  parallelFor(int(jobs_.size()), size_t(box.x1 - box.x0) * size_t(box.y1 - box.y0), [&](int j) {
    const TileJob& job = jobs_[size_t(j)];
    const IRect& tr = job.tr;
    const int n = tr.x1 - tr.x0;
    const SrcTile* bg = bgTile(tileOf(tr.x0), tileOf(tr.y0));
    Part& q = parts[size_t(j)];
    for (int y = tr.y0; y < tr.y1; ++y) {
      const float* c = cov_.data() + size_t(y - box.y0) * bw + (tr.x0 - box.x0);
      const size_t off = size_t(y & TILE_MASK) * TILE + (tr.x0 & TILE_MASK);
      const float* s = job.cur->px + off * 4;
      const float* u = bg ? bg->px.px + off * 4 : nullptr;
      for (int i = 0; i < n; ++i) {
        const float w = c[i];
        if (w <= 0.0f) continue;
        const float* p = s + i * 4;
        // The layer over the supplier's pixels, premultiplied.
        const float k = u ? 1.0f - p[3] : 0.0f;
        for (int ch = 0; ch < 4; ++ch) q.c[ch] += double(p[ch] + (u ? u[i * 4 + ch] * k : 0.0f)) * w;
        q.w += w;
      }
    }
  });
  double acc[4] = {0, 0, 0, 0}, wsum = 0;
  for (const Part& q : parts) {
    for (int ch = 0; ch < 4; ++ch) acc[ch] += q.c[ch];
    wsum += q.w;
  }
  if (wsum <= 0.0) return false;
  for (int ch = 0; ch < 4; ++ch) out[ch] = float(acc[ch] / wsum);
  return true;
}

void PaintEngine::dabTile(const TileJob& job, const IRect& box, int bw, float opacity,
                          float flow) {
  const IRect& tr = job.tr;
  const int n = tr.x1 - tr.x0;
  const Tile* bef = job.bef;
  Tile* cur = job.cur;
  MaskTile* ms = job.ms;
  MaskTile* as = job.as;
  const float cr = p_.color[0], cg = p_.color[1], cb = p_.color[2];
  const int blendMode = p_.blend;
  for (int y = tr.y0; y < tr.y1; ++y) {
    const float* c = cov_.data() + size_t(y - box.y0) * bw + (tr.x0 - box.x0);
    const size_t off = size_t(y & TILE_MASK) * TILE + (tr.x0 & TILE_MASK);
    float* M = ms->a + off;
    float* A = as ? as->a + off : nullptr;
    float* o = cur->px + off * 4;
    const float* b = bef->px + off * 4;
    switch (p_.tool) {
      case Tool::Paint:
        if (blendMode == kNormal) {
          for (int i = 0; i < n; ++i) {
            const float cv = c[i] * flow;
            if (cv <= 0.0f) continue;
            const float m = M[i] + (1.0f - M[i]) * cv;
            M[i] = m;
            float a = m * opacity;
            if (A) { if (a > A[i]) A[i] = a; a = A[i]; }
            const float ia = 1.0f - a;
            o[i * 4] = b[i * 4] * ia + cr * a;
            o[i * 4 + 1] = b[i * 4 + 1] * ia + cg * a;
            o[i * 4 + 2] = b[i * 4 + 2] * ia + cb * a;
            o[i * 4 + 3] = b[i * 4 + 3] * ia + a;
          }
        } else {
          // The whole stroke is one blend onto the pre-stroke pixels, which is why
          // the source alpha is the accumulated stroke alpha rather than this dab's.
          for (int i = 0; i < n; ++i) {
            const float cv = c[i] * flow;
            if (cv <= 0.0f) continue;
            const float m = M[i] + (1.0f - M[i]) * cv;
            M[i] = m;
            float a = m * opacity;
            if (A) { if (a > A[i]) A[i] = a; a = A[i]; }
            const float src[4] = {cr * a, cg * a, cb * a, a};
            float dst[4] = {b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]};
            blendPixel(blendMode, 1.0f, src, dst);
            for (int ch = 0; ch < 4; ++ch) o[i * 4 + ch] = dst[ch];
          }
        }
        break;
      case Tool::Erase:
        for (int i = 0; i < n; ++i) {
          const float cv = c[i] * flow;
          if (cv <= 0.0f) continue;
          const float m = M[i] + (1.0f - M[i]) * cv;
          M[i] = m;
          float a = m * opacity;
          if (A) { if (a > A[i]) A[i] = a; a = A[i]; }
          const float ia = 1.0f - a;
          o[i * 4] = b[i * 4] * ia; o[i * 4 + 1] = b[i * 4 + 1] * ia;
          o[i * 4 + 2] = b[i * 4 + 2] * ia; o[i * 4 + 3] = b[i * 4 + 3] * ia;
        }
        break;
      case Tool::Smudge: {
        // Smudge is the one tool that builds on its own output: it writes into the
        // live canvas rather than compositing onto the pre-stroke snapshot, because
        // each dab has to pick up where the last one left off. Undo is unaffected -
        // before_ still holds the pre-stroke copy of every tile the stroke touches.
        const float* sm = smudge_;
        const float str = p_.smudgeStrength;
        for (int i = 0; i < n; ++i) {
          const float cv = c[i] * flow * opacity * str;
          if (cv <= 0.0f) continue;
          float* d = o + i * 4;
          for (int ch = 0; ch < 4; ++ch) d[ch] += (sm[ch] - d[ch]) * cv;
        }
        break;
      }
      case Tool::Clone: {
        fetchSourceRow(y + p_.cloneDy, tr.x0 + p_.cloneDx, n, src_.data(), vld_.data());
        const float* s = src_.data();
        const uint8_t* v = vld_.data();
        for (int i = 0; i < n; ++i) {
          // No readable source: leave the pixel exactly as it was. Nothing is
          // written and the coverage masks are not advanced, so a later dab over
          // the same spot still lays down the paint once the source can be read.
          if (!v[i]) continue;
          const float cv = c[i] * flow;
          if (cv <= 0.0f) continue;
          const float m = M[i] + (1.0f - M[i]) * cv;
          M[i] = m;
          float a = m * opacity;
          if (A) { if (a > A[i]) A[i] = a; a = A[i]; }
          if (blendMode != kNormal) {
            const float src4[4] = {s[i * 4] * a, s[i * 4 + 1] * a, s[i * 4 + 2] * a,
                                   s[i * 4 + 3] * a};
            float dst[4] = {b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]};
            blendPixel(blendMode, 1.0f, src4, dst);
            for (int ch = 0; ch < 4; ++ch) o[i * 4 + ch] = dst[ch];
            continue;
          }
          const float ia = 1.0f - a;
          o[i * 4] = b[i * 4] * ia + s[i * 4] * a;
          o[i * 4 + 1] = b[i * 4 + 1] * ia + s[i * 4 + 1] * a;
          o[i * 4 + 2] = b[i * 4 + 2] * ia + s[i * 4 + 2] * a;
          // Alpha comes from the source, not from the dab. Taking it from the dab
          // meant a source with no coverage still claimed to be fully opaque, so
          // empty source landed as solid black. For an ordinary opaque plate
          // s[3] is 1 and this is exactly what it was before.
          o[i * 4 + 3] = b[i * 4 + 3] * ia + s[i * 4 + 3] * a;
        }
        break;
      }
    }
  }
}


bool PaintEngine::endStroke() {
  if (!canvas_) return false;
  // Let a smoothed stroke finish where the pointer actually was, so lifting the pen
  // does not leave the last stretch unpainted.
  if (p_.smoothing > 0.0f) {
    for (int i = 0; i < 24; ++i) {
      const float dx = rawX_ - lx_, dy = rawY_ - ly_;
      if (dx * dx + dy * dy < 0.25f) break;
      strokeTowards(lx_ + dx * 0.5f, ly_ + dy * 0.5f, lp_);
    }
    strokeTowards(rawX_, rawY_, lp_);
  }
  bool changed = !before_.empty();
  if (changed) {
    {
      // Drop tiles the stroke emptied (erasing) so the canvas stays sparse.
      std::unique_lock<std::shared_mutex> lock(canvas_->mutex);
      for (const auto& kv : before_) {
        auto it = canvas_->tiles.find(kv.first);
        if (it != canvas_->tiles.end() && it->second->allZero()) canvas_->tiles.erase(it);
      }
    }
    Entry e;
    e.canvas = canvas_;
    e.tiles = std::move(before_);
    undo_.push_back(std::move(e));
    redo_.clear();
    trim();
  }
  before_.clear(); mask_.clear(); maskA_.clear(); bg_.clear(); miss_.clear();
  // missedEver_ and the hit/miss counts deliberately survive: the caller reports on the
  // stroke after endStroke returns, and clearing them here would always report zero.
  canvas_.reset();
  bgFetch_ = nullptr;
  return changed;
}

// ---------------------------------------------------------------- history

IRect PaintEngine::clear(std::shared_ptr<Canvas> canvas) {
  IRect dirty;
  if (!canvas) return dirty;
  if (canvas_) endStroke();
  Entry e;
  e.canvas = canvas;
  {
    std::unique_lock<std::shared_mutex> lock(canvas->mutex);
    if (canvas->tiles.empty()) return dirty;
    dirty = canvas->bounds();
    e.tiles = std::move(canvas->tiles);
    canvas->tiles.clear();
    canvas->dirty = true;
  }
  undo_.push_back(std::move(e));
  redo_.clear();
  trim();
  return dirty;
}

void PaintEngine::pushStructural(std::function<void()> swap) {
  if (!swap) return;
  if (canvas_) endStroke();
  Entry e;
  e.swapFn = std::move(swap);
  undo_.push_back(std::move(e));
  redo_.clear();
  trim();
}

// Exchanges the entry's tiles with the canvas's, so the same entry undoes and redoes.
// A structural entry just calls its own inverse and reports the whole image dirty.
IRect PaintEngine::swap(Entry& e) {
  IRect dirty;
  if (e.swapFn) { e.swapFn(); return dirty; }
  std::unique_lock<std::shared_mutex> lock(e.canvas->mutex);
  for (auto& kv : e.tiles) {
    auto it = e.canvas->tiles.find(kv.first);
    std::unique_ptr<Tile> current;
    if (it != e.canvas->tiles.end()) current = std::move(it->second);
    if (kv.second) {
      if (it != e.canvas->tiles.end()) it->second = std::move(kv.second);
      else e.canvas->tiles[kv.first] = std::move(kv.second);
    } else if (it != e.canvas->tiles.end()) {
      e.canvas->tiles.erase(it);
    }
    kv.second = std::move(current);
    dirty.merge(tileRect(keyTx(kv.first), keyTy(kv.first)));
  }
  e.canvas->dirty = true;
  return dirty;
}

bool PaintEngine::undo(IRect* dirty, std::shared_ptr<Canvas>* which) {
  if (canvas_) endStroke();
  if (undo_.empty()) return false;
  Entry e = std::move(undo_.back());
  undo_.pop_back();
  const IRect d = swap(e);
  if (dirty) *dirty = d;
  if (which) *which = e.canvas;
  redo_.push_back(std::move(e));
  return true;
}

bool PaintEngine::redo(IRect* dirty, std::shared_ptr<Canvas>* which) {
  if (canvas_) endStroke();
  if (redo_.empty()) return false;
  Entry e = std::move(redo_.back());
  redo_.pop_back();
  const IRect d = swap(e);
  if (dirty) *dirty = d;
  if (which) *which = e.canvas;
  undo_.push_back(std::move(e));
  return true;
}

}  // namespace rp
