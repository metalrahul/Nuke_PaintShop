#include "Brush.h"

#include "Parallel.h"

#include "Descriptor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace rp {

// ---------------------------------------------------------------- tips

void BrushTip::setImage(int w, int h, std::vector<float> coverage) {
  mips.clear();
  if (w <= 0 || h <= 0 || coverage.size() < size_t(w) * size_t(h)) return;
  Level l0;
  l0.w = w; l0.h = h; l0.a = std::move(coverage);
  mips.push_back(std::move(l0));
  while (mips.back().w > 1 || mips.back().h > 1) {
    const Level& s = mips.back();
    Level d;
    d.w = (s.w + 1) / 2; d.h = (s.h + 1) / 2;
    d.a.resize(size_t(d.w) * d.h);
    for (int y = 0; y < d.h; ++y) {
      const int y0 = std::min(y * 2, s.h - 1), y1 = std::min(y * 2 + 1, s.h - 1);
      for (int x = 0; x < d.w; ++x) {
        const int x0 = std::min(x * 2, s.w - 1), x1 = std::min(x * 2 + 1, s.w - 1);
        d.a[size_t(y) * d.w + x] = 0.25f * (s.a[size_t(y0) * s.w + x0] + s.a[size_t(y0) * s.w + x1] +
                                            s.a[size_t(y1) * s.w + x0] + s.a[size_t(y1) * s.w + x1]);
      }
    }
    mips.push_back(std::move(d));
  }
}

namespace {
// Semi-axes of a dab before rotation: half-width along the tip's x and y.
void semiAxes(const BrushTip* tip, const DabShape& s, float& a, float& b) {
  const float r = std::max(s.roundness, 0.0f);
  a = std::max(s.diameter, 0.5f) * 0.5f;
  b = a * r;
  if (tip && tip->sampled()) {
    const float m = float(std::max(tip->width(), tip->height()));
    a = std::max(s.diameter, 0.5f) * tip->width() / m * 0.5f;
    b = std::max(s.diameter, 0.5f) * tip->height() / m * 0.5f * r;
  }
}
}  // namespace

IRect dabBounds(const BrushTip* tip, float cx, float cy, const DabShape& s) {
  float a, b;
  semiAxes(tip, s, a, b);
  // Bounding box of the rotated tip rectangle; conservative for the round brush too.
  const float c = std::fabs(std::cos(s.angle)), sn = std::fabs(std::sin(s.angle));
  const float hw = a * c + b * sn, hh = a * sn + b * c;
  return IRect{int(std::floor(cx - hw - 0.5f)), int(std::floor(cy - hh - 0.5f)),
               int(std::ceil(cx + hw + 0.5f)) + 1, int(std::ceil(cy + hh + 0.5f)) + 1};
}

// Both rasterisers map each destination pixel back into the dab's own space with the
// inverse rotation. The mapping is affine in x, so each row steps incrementally and the
// span that can possibly be covered is solved up front - rotation and roundness then
// cost nothing beyond the setup, and empty pixels are never visited.

namespace {
// Clears [0,lo) and [hi,bw) of a row and returns false if nothing is left to draw.
inline bool clipRow(float* row, int bw, int& lo, int& hi) {
  lo = std::max(lo, 0);
  hi = std::min(hi, bw);
  if (hi <= lo) { std::memset(row, 0, size_t(bw) * sizeof(float)); return false; }
  if (lo > 0) std::memset(row, 0, size_t(lo) * sizeof(float));
  if (hi < bw) std::memset(row + hi, 0, size_t(bw - hi) * sizeof(float));
  return true;
}

// Range of i where |p0 + dp*i| <= 1, as a half-open [lo, hi).
inline void stripSpan(float p0, float dp, int bw, int& lo, int& hi) {
  if (std::fabs(dp) < 1e-9f) {
    if (std::fabs(p0) > 1.0f) { lo = 0; hi = 0; }
    return;
  }
  float t0 = (-1.0f - p0) / dp, t1 = (1.0f - p0) / dp;
  if (t0 > t1) std::swap(t0, t1);
  lo = std::max(lo, int(std::floor(t0)));
  hi = std::min(hi, int(std::ceil(t1)) + 1);
  (void)bw;
}
}  // namespace

static void rasterizeRound(float cx, float cy, const DabShape& s, float a, float b,
                           const IRect& box, float* cov) {
  const int bw = box.x1 - box.x0;
  const float hardness = std::min(std::max(s.hardness, 0.0f), 1.0f);
  const float soft = std::max(1.0f - hardness, 1e-4f);
  const float invA = 1.0f / std::max(a, 0.25f), invB = 1.0f / std::max(b, 0.25f);
  const float ca = std::cos(-s.angle), sa = std::sin(-s.angle);
  // One destination pixel is this wide in normalised units; gives ~1px of edge AA.
  const float aa = std::max(invA, invB);
  const float R = 1.0f + aa;  // outer radius including the anti-aliased fringe
  const float du = ca * invA, dv = sa * invB;
  const float qa = du * du + dv * dv;
  const float x0c = (float(box.x0) + 0.5f) - cx;
  // Rows are independent and give the same values either way, so a large tip's
  // coverage is rasterised across cores.
  parallelForRows(box.y0, box.y1, size_t(bw) * size_t(box.y1 - box.y0), [&](int y) {
    float* row = cov + size_t(y - box.y0) * bw;
    const float dy = (y + 0.5f) - cy;
    const float u0 = (x0c * ca - dy * sa) * invA;
    const float v0 = (x0c * sa + dy * ca) * invB;
    // Solve |(u0,v0) + i*(du,dv)| = R for the covered span of this row.
    const float qb = 2.0f * (u0 * du + v0 * dv);
    const float qc = u0 * u0 + v0 * v0 - R * R;
    const float disc = qb * qb - 4.0f * qa * qc;
    if (disc <= 0.0f) { std::memset(row, 0, size_t(bw) * sizeof(float)); return; }
    const float sq = std::sqrt(disc);
    int lo = int(std::floor((-qb - sq) / (2.0f * qa)));
    int hi = int(std::ceil((-qb + sq) / (2.0f * qa))) + 1;
    if (!clipRow(row, bw, lo, hi)) return;
    float u = u0 + du * float(lo), v = v0 + dv * float(lo);
    for (int i = lo; i < hi; ++i, u += du, v += dv) {
      const float d = std::sqrt(u * u + v * v);
      const float edge = std::min(std::max((1.0f - d) / aa + 0.5f, 0.0f), 1.0f);
      float t = std::min(std::max((1.0f - d) / soft, 0.0f), 1.0f);
      t = t * t * (3.0f - 2.0f * t);
      row[i] = hardness >= 0.999f ? edge : edge * t;
    }
  });
}

static void rasterizeSampled(const BrushTip& tip, float cx, float cy, const DabShape& s, float a,
                             float b, const IRect& box, float* cov) {
  const int bw = box.x1 - box.x0;
  // Pick the mip whose texels are about one destination pixel apart.
  const float scale = (2.0f * std::max(a, b)) / float(std::max(tip.width(), tip.height()));
  int L = 0;
  if (scale < 1.0f && scale > 0.0f)
    L = std::min(int(std::floor(std::log2(1.0f / scale))), int(tip.mips.size()) - 1);
  const BrushTip::Level& m = tip.mips[size_t(L)];
  const float ca = std::cos(-s.angle), sa = std::sin(-s.angle);
  const float invA = 1.0f / std::max(a, 0.25f), invB = 1.0f / std::max(b, 0.25f);
  const float fx = s.flipX ? -1.0f : 1.0f, fy = s.flipY ? -1.0f : 1.0f;
  const float* ar = m.a.data();
  const float dun = ca * invA * fx, dvn = sa * invB * fy;
  const float x0c = (float(box.x0) + 0.5f) - cx;
  const float su = 0.5f * float(m.w), sv = 0.5f * float(m.h);
  // As the round tip: rows are independent, so a large sampled tip - the slowest
  // brush there is - spreads its coverage across cores.
  parallelForRows(box.y0, box.y1, size_t(bw) * size_t(box.y1 - box.y0), [&](int y) {
    float* row = cov + size_t(y - box.y0) * bw;
    const float dy = (y + 0.5f) - cy;
    // Normalised dab space, -1..1 across the tip; affine in x, so step it.
    const float un0 = (x0c * ca - dy * sa) * invA * fx;
    const float vn0 = (x0c * sa + dy * ca) * invB * fy;
    int lo = 0, hi = bw;
    stripSpan(un0, dun, bw, lo, hi);
    stripSpan(vn0, dvn, bw, lo, hi);
    if (!clipRow(row, bw, lo, hi)) return;
    float un = un0 + dun * float(lo), vn = vn0 + dvn * float(lo);
    for (int i = lo; i < hi; ++i, un += dun, vn += dvn) {
      if (un < -1.0f || un > 1.0f || vn < -1.0f || vn > 1.0f) { row[i] = 0.0f; continue; }
      const float u = (un + 1.0f) * su - 0.5f;
      const float v = (vn + 1.0f) * sv - 0.5f;
      const int u0 = int(std::floor(u)), v0 = int(std::floor(v));
      const float fu = u - float(u0), fv = v - float(v0);
      const bool inner = u0 >= 0 && u0 + 1 < m.w && v0 >= 0 && v0 + 1 < m.h;
      if (inner) {  // the common case: four taps with no bounds checks
        const float* r0 = ar + size_t(v0) * m.w + u0;
        const float* r1 = r0 + m.w;
        const float top = r0[0] + (r0[1] - r0[0]) * fu;
        const float bot = r1[0] + (r1[1] - r1[0]) * fu;
        row[i] = top + (bot - top) * fv;
      } else {
        auto tap = [&](int ix, int iy) -> float {
          if (ix < 0 || ix >= m.w || iy < 0 || iy >= m.h) return 0.0f;
          return ar[size_t(iy) * m.w + ix];
        };
        const float top = tap(u0, v0) + (tap(u0 + 1, v0) - tap(u0, v0)) * fu;
        const float bot = tap(u0, v0 + 1) + (tap(u0 + 1, v0 + 1) - tap(u0, v0 + 1)) * fu;
        row[i] = top + (bot - top) * fv;
      }
    }
  });
}

void rasterizeDab(const BrushTip* tip, float cx, float cy, const DabShape& s, const IRect& box,
                  float* cov) {
  if (box.empty()) return;
  float a, b;
  semiAxes(tip, s, a, b);
  if (tip && tip->sampled()) rasterizeSampled(*tip, cx, cy, s, a, b, box, cov);
  else rasterizeRound(cx, cy, s, a, b, box, cov);
}

// ---------------------------------------------------------------- .abr reader

namespace {

struct Reader {
  const std::vector<uint8_t>& d;
  size_t p = 0;
  bool ok = true;
  explicit Reader(const std::vector<uint8_t>& data) : d(data) {}
  bool has(size_t n) const { return p + n <= d.size(); }
  uint8_t u8() { if (!has(1)) { ok = false; return 0; } return d[p++]; }
  int16_t i16() { if (!has(2)) { ok = false; return 0; } int16_t v = int16_t((d[p] << 8) | d[p + 1]); p += 2; return v; }
  int32_t i32() {
    if (!has(4)) { ok = false; return 0; }
    uint32_t v = (uint32_t(d[p]) << 24) | (uint32_t(d[p + 1]) << 16) | (uint32_t(d[p + 2]) << 8) | d[p + 3];
    p += 4; return int32_t(v);
  }
  void skip(size_t n) { if (!has(n)) { ok = false; p = d.size(); } else p += n; }
  void seek(size_t to) { if (to > d.size()) { ok = false; p = d.size(); } else p = to; }
};

const int kMaxTipSide = 16384;

// Reads the sample plane that follows the bounds/depth/compression fields.
bool readSamples(Reader& r, int w, int h, int depth, int compression, std::vector<float>& out) {
  if (w <= 0 || h <= 0 || w > kMaxTipSide || h > kMaxTipSide) return false;
  if (depth != 8 && depth != 16) return false;
  const size_t bps = size_t(depth / 8), rowBytes = size_t(w) * bps;
  std::vector<uint8_t> raw(rowBytes * size_t(h));
  if (!compression) {
    if (!r.has(raw.size())) return false;
    std::memcpy(raw.data(), r.d.data() + r.p, raw.size());
    r.p += raw.size();
  } else {
    // One big-endian int16 packed length per scanline, then the PackBits data.
    std::vector<size_t> lens(size_t(h), 0);
    for (int y = 0; y < h; ++y) lens[size_t(y)] = size_t(uint16_t(r.i16()));
    if (!r.ok) return false;
    for (int y = 0; y < h; ++y) {
      const size_t n = lens[size_t(y)];
      if (!r.has(n)) return false;
      packBitsDecode(r.d.data() + r.p, n, raw.data() + size_t(y) * rowBytes, rowBytes);
      r.p += n;
    }
  }
  out.resize(size_t(w) * size_t(h));
  if (bps == 1) {
    for (size_t i = 0; i < out.size(); ++i) out[i] = raw[i] * (1.0f / 255.0f);
  } else {
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = float((raw[i * 2] << 8) | raw[i * 2 + 1]) * (1.0f / 65535.0f);
  }
  return true;
}

// .abr rows run top to bottom; Nuke's y axis points up. Flip so brushes are not upside down.
void flipRows(int w, int h, std::vector<float>& a) {
  for (int y = 0; y < h / 2; ++y)
    std::swap_ranges(a.begin() + size_t(y) * w, a.begin() + size_t(y + 1) * w,
                     a.begin() + size_t(h - 1 - y) * w);
}

void addTip(std::vector<BrushTip>& tips, const std::string& base, const std::string& name, int w,
            int h, std::vector<float>& a, float spacing, const std::string& id = std::string()) {
  flipRows(w, h, a);
  BrushTip t;
  t.name = name.empty() ? base + " " + std::to_string(tips.size() + 1) : name;
  t.spacingHint = spacing;
  t.id = id;
  t.setImage(w, h, std::move(a));
  if (t.sampled()) tips.push_back(std::move(t));
}

// Versions 1 and 2: a flat list of brushes.
void loadV12(Reader& r, int version, const std::string& base, std::vector<BrushTip>& tips) {
  const int count = r.i16();
  for (int i = 0; i < count && r.ok; ++i) {
    const int type = r.i16();
    const int32_t size = r.i32();
    if (!r.ok || size < 0) break;
    const size_t next = r.p + size_t(size);
    if (type == 2) {
      r.i32();  // misc
      const int spacing = r.i16();
      std::string name;
      if (version == 2) {
        const int32_t chars = r.i32();
        for (int32_t c = 0; c < chars && r.ok; ++c) {
          const int ch = uint16_t(r.i16());
          if (ch > 0 && ch < 128) name.push_back(char(ch));
        }
      }
      r.u8();     // anti-aliasing
      r.skip(8);  // short bounds
      const int top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
      const int depth = r.i16();
      const int comp = r.u8();
      std::vector<float> a;
      if (r.ok && readSamples(r, right - left, bottom - top, depth, comp, a))
        addTip(tips, base, name, right - left, bottom - top, a, spacing / 100.0f);
    }
    r.seek(next);  // also skips computed brushes (type 1)
  }
}

// Versions 6, 7, 10: 8BIM blocks; the 'samp' block holds the sampled tips.
void loadV6(Reader& r, int subversion, const std::string& base, std::vector<BrushTip>& tips) {
  while (r.ok && r.has(12)) {
    if (std::memcmp(r.d.data() + r.p, "8BIM", 4) != 0) {
      // Some writers pad blocks to 4 bytes; resynchronise.
      const size_t aligned = (r.p + 3) & ~size_t(3);
      if (aligned == r.p || !r.has(aligned - r.p + 12) ||
          std::memcmp(r.d.data() + aligned, "8BIM", 4) != 0) return;
      r.p = aligned;
    }
    r.skip(4);
    const bool isSamp = std::memcmp(r.d.data() + r.p, "samp", 4) == 0;
    r.skip(4);
    const int32_t blockSize = r.i32();
    if (!r.ok || blockSize < 0) return;
    const size_t blockEnd = std::min(r.p + size_t(blockSize), r.d.size());
    if (!isSamp) { r.seek(blockEnd); continue; }
    while (r.ok && r.p + 4 <= blockEnd) {
      const int32_t brushSize = r.i32();
      if (!r.ok || brushSize <= 0) break;
      const size_t next = r.p + ((size_t(brushSize) + 3) & ~size_t(3));
      // A Pascal string holding the brush's id, which the descriptor block refers to,
      // then a fixed header whose size differs by sub-version. Measuring the id rather
      // than assuming 36 characters keeps the header skip correct either way.
      const size_t keyStart = r.p;
      std::string id;
      if (r.has(1)) {
        const size_t klen = r.u8();
        if (r.has(klen)) id.assign(reinterpret_cast<const char*>(r.d.data() + r.p), klen);
        r.skip(klen);
      }
      const size_t header = subversion == 1 ? 47 : 301;
      if (keyStart + header > r.p) r.seek(keyStart + header);
      const int top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
      const int depth = r.i16();
      const int comp = r.u8();
      std::vector<float> a;
      if (r.ok && readSamples(r, right - left, bottom - top, depth, comp, a))
        addTip(tips, base, std::string(), right - left, bottom - top, a, 0.0f, id);
      r.ok = true;  // one bad sample should not lose the rest
      r.seek(std::min(next, blockEnd));
    }
    r.seek(blockEnd);
  }
}

// ---- descriptor block -> brush presets

// Photoshop's "brush variance type": which input drives a dynamic.
int controlFromBVTy(int v, bool forAngle) {
  switch (v) {
    case 1: return 1;  // fade
    case 2: return 2;  // pen pressure
    case 5:            // initial direction
    case 6:            // direction
      return forAngle ? kAngleDirection : 0;
    default: return 0;  // off, or pen tilt / stylus wheel, which Nuke cannot report
  }
}

void readDyn(const DVal* d, DynControl& out, bool forAngle = false) {
  if (!d || d->type != DVal::kDesc) return;
  out.jitter = float(d->numOr("jitter", 0.0) / 100.0);
  out.minimum = float(d->numOr("Mnm ", 0.0) / 100.0);
  out.control = controlFromBVTy(int(d->numOr("bVTy", 0.0)), forAngle);
  out.fadeSteps = std::max(1, int(d->numOr("fStp", 25.0)));
}

float clamp01f(double v) { return float(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)); }

BrushPreset presetFromDescriptor(const DVal& b, const std::map<std::string, int>& tipById) {
  BrushPreset p;
  p.fromFile = true;
  p.name = b.textOr("Nm  ", std::string());

  if (const DVal* tip = b.find("Brsh")) {
    if (tip->type == DVal::kDesc) {
      p.diameter = float(std::max(1.0, tip->numOr("Dmtr", 50.0)));
      p.spacing = float(std::max(0.01, tip->numOr("Spcn", 25.0) / 100.0));
      p.hardness = clamp01f(tip->numOr("Hrdn", 100.0) / 100.0);
      p.dyn.angle = float(tip->numOr("Angl", 0.0));
      p.dyn.roundness = clamp01f(tip->numOr("Rndn", 100.0) / 100.0);
      p.dyn.flipX = tip->boolOr("flipX", false);
      p.dyn.flipY = tip->boolOr("flipY", false);
      const std::string id = tip->textOr("sampledData", std::string());
      if (!id.empty()) {
        auto it = tipById.find(id);
        if (it != tipById.end()) p.tip = it->second;
      }
      if (p.name.empty()) p.name = tip->textOr("Nm  ", std::string());
    }
  }

  // Shape dynamics. The minimums live at the top level in Photoshop's UI, not inside
  // the jitter sub-descriptors, so they are read separately and win.
  if (b.boolOr("useTipDynamics", false)) {
    readDyn(b.find("szVr"), p.dyn.size);
    readDyn(b.find("roundnessDynamics"), p.dyn.round);
    if (const DVal* ad = b.find("angleDynamics")) {
      if (ad->type == DVal::kDesc) {
        p.dyn.angleJitter = clamp01f(ad->numOr("jitter", 0.0) / 100.0);
        p.dyn.angleControl = controlFromBVTy(int(ad->numOr("bVTy", 0.0)), true);
        p.dyn.angleFadeSteps = std::max(1, int(ad->numOr("fStp", 25.0)));
      }
    }
    if (b.find("minimumDiameter"))
      p.dyn.size.minimum = clamp01f(b.numOr("minimumDiameter", 0.0) / 100.0);
    if (b.find("minimumRoundness"))
      p.dyn.round.minimum = clamp01f(b.numOr("minimumRoundness", 0.0) / 100.0);
    // The top-level flips are the jitters; the static ones are on the tip above.
    p.dyn.flipXJitter = b.boolOr("flipX", false);
    p.dyn.flipYJitter = b.boolOr("flipY", false);
  }

  if (b.boolOr("useScatter", false)) {
    p.dyn.scatter = float(std::max(0.0, b.numOr("Spcn", 0.0) / 100.0));
    p.dyn.count = std::max(1, int(b.numOr("Cnt ", 1.0)));
    p.dyn.scatterBoth = b.boolOr("bothAxes", false);
    if (const DVal* cd = b.find("countDynamics"))
      p.dyn.countJitter = clamp01f(cd->numOr("jitter", 0.0) / 100.0);
  }

  if (b.boolOr("usePaintDynamics", false)) {
    readDyn(b.find("opVr"), p.dyn.opacity);  // opacity jitter
    readDyn(b.find("prVr"), p.dyn.flow);     // flow jitter
  }
  return p;
}

// Walks the 8BIM blocks looking for one key. Returns its payload.
bool findBlock(const std::vector<uint8_t>& d, const char* want, size_t& pos, size_t& len) {
  size_t p = 4;  // past version + sub-version
  while (p + 12 <= d.size()) {
    if (std::memcmp(d.data() + p, "8BIM", 4) != 0) {
      const size_t aligned = (p + 3) & ~size_t(3);
      if (aligned == p || aligned + 12 > d.size() ||
          std::memcmp(d.data() + aligned, "8BIM", 4) != 0)
        return false;
      p = aligned;
    }
    const uint8_t* h = d.data() + p;
    const uint32_t size = (uint32_t(h[8]) << 24) | (uint32_t(h[9]) << 16) |
                          (uint32_t(h[10]) << 8) | h[11];
    if (std::memcmp(h + 4, want, 4) == 0) {
      pos = p + 12;
      len = std::min(size_t(size), d.size() - pos);
      return true;
    }
    if (size > d.size() - (p + 12)) return false;
    p += 12 + size;
  }
  return false;
}

}  // namespace

AbrFile loadABR(const std::string& path, std::string* err) {
  AbrFile out;
  auto fail = [&](const char* m) { if (err) *err = m; return AbrFile(); };
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return fail("cannot open file");
  std::vector<uint8_t> data;
  std::fseek(f, 0, SEEK_END);
  const long len = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (len > 0) {
    data.resize(size_t(len));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
  }
  std::fclose(f);
  if (data.size() < 4) return fail("file is empty or unreadable");

  std::string base = path;
  const size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base = base.substr(0, dot);

  Reader r(data);
  const int version = r.i16();
  bool haveDesc = false;
  DVal desc;
  if (version == 1 || version == 2) {
    loadV12(r, version, base, out.tips);
  } else if (version == 6 || version == 7 || version == 10) {
    const int sub = r.i16();
    if (sub != 1 && sub != 2) return fail("unsupported .abr sub-version");
    loadV6(r, sub, base, out.tips);
    size_t dpos = 0, dlen = 0;
    if (findBlock(data, "desc", dpos, dlen) && dlen > 4) {
      size_t p = dpos + 4;  // a version word precedes the descriptor
      haveDesc = parseDescriptor(data.data(), dpos + dlen, p, desc);
    }
  } else {
    return fail("unsupported .abr version");
  }

  std::map<std::string, int> tipById;
  for (size_t i = 0; i < out.tips.size(); ++i)
    if (!out.tips[i].id.empty()) tipById.emplace(out.tips[i].id, int(i));

  if (haveDesc) {
    const DVal* list = desc.find("Brsh");
    if (list && list->type == DVal::kList) {
      for (const DVal& b : list->items) {
        if (b.type != DVal::kDesc) continue;
        BrushPreset p = presetFromDescriptor(b, tipById);
        if (p.name.empty())
          p.name = base + " " + std::to_string(out.presets.size() + 1);
        out.presets.push_back(std::move(p));
      }
    }
  }

  // No descriptor, or one that named nothing: fall back to one preset per bitmap.
  if (out.presets.empty()) {
    for (size_t i = 0; i < out.tips.size(); ++i) {
      BrushPreset p;
      p.name = out.tips[i].name;
      p.tip = int(i);
      if (out.tips[i].spacingHint > 0.0f) {
        p.spacing = out.tips[i].spacingHint;
        p.fromFile = true;
      }
      p.diameter = float(std::max(out.tips[i].width(), out.tips[i].height()));
      out.presets.push_back(std::move(p));
    }
  }

  if (out.presets.empty() && err)
    *err = "no brushes found (computed-only files are not supported)";
  return out;
}

}  // namespace rp
