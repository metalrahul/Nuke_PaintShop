#include "Canvas.h"

#include <cstdio>

namespace rp {

IRect Canvas::bounds() const {
  IRect b;
  for (const auto& kv : tiles) b.merge(tileRect(keyTx(kv.first), keyTy(kv.first)));
  return b;
}

bool Canvas::rowHasPaint(int y, int x0, int x1) const {
  if (tiles.empty() || x1 <= x0) return false;
  const int ty = tileOf(y);
  for (int tx = tileOf(x0); tx <= tileOf(x1 - 1); ++tx)
    if (find(tx, ty)) return true;
  return false;
}

void Canvas::compositeRow(int y, int x0, int x1, const float* const in[4],
                          float* const out[4]) const {
  const int ty = tileOf(y);
  const int rowInTile = y & TILE_MASK;
  int x = x0;
  while (x < x1) {
    const int tx = tileOf(x);
    const int segEnd = std::min(x1, (tx + 1) * TILE);
    const int n = segEnd - x;
    const int o = x - x0;
    const Tile* t = find(tx, ty);
    if (!t) {
      for (int c = 0; c < 4; ++c) {
        if (!out[c]) continue;
        if (in[c]) std::memcpy(out[c] + o, in[c] + o, size_t(n) * sizeof(float));
        else std::memset(out[c] + o, 0, size_t(n) * sizeof(float));
      }
    } else {
      const float* p = t->px + (size_t(rowInTile) * TILE + (x & TILE_MASK)) * 4;
      for (int c = 0; c < 4; ++c) {
        float* d = out[c];
        if (!d) continue;
        d += o;
        const float* s = in[c] ? in[c] + o : nullptr;
        if (s) {
          for (int i = 0; i < n; ++i) d[i] = p[i * 4 + c] + s[i] * (1.0f - p[i * 4 + 3]);
        } else {
          for (int i = 0; i < n; ++i) d[i] = p[i * 4 + c];
        }
      }
    }
    x = segEnd;
  }
}

void Canvas::blendRow(int y, int x0, int x1, float* const acc[4], float opacity, int mode) const {
  if (opacity <= 0.0f || x1 <= x0) return;
  const int ty = tileOf(y);
  const int rowInTile = y & TILE_MASK;
  int x = x0;
  while (x < x1) {
    const int tx = tileOf(x);
    const int segEnd = std::min(x1, (tx + 1) * TILE);
    const int n = segEnd - x;
    const int o = x - x0;
    const Tile* t = find(tx, ty);
    if (!t) { x = segEnd; continue; }  // nothing on this layer here
    const float* p = t->px + (size_t(rowInTile) * TILE + (x & TILE_MASK)) * 4;
    float* ar = acc[0] + o;
    float* ag = acc[1] + o;
    float* ab_ = acc[2] + o;
    float* aa = acc[3] + o;
    if (isPassThrough(mode) && opacity >= 1.0f) {
      // Plain source-over, the overwhelmingly common case.
      for (int i = 0; i < n; ++i) {
        const float k = 1.0f - p[i * 4 + 3];
        ar[i] = p[i * 4 + 0] + ar[i] * k;
        ag[i] = p[i * 4 + 1] + ag[i] * k;
        ab_[i] = p[i * 4 + 2] + ab_[i] * k;
        aa[i] = p[i * 4 + 3] + aa[i] * k;
      }
    } else if (isPassThrough(mode)) {
      for (int i = 0; i < n; ++i) {
        const float sa = p[i * 4 + 3] * opacity;
        const float k = 1.0f - sa;
        ar[i] = p[i * 4 + 0] * opacity + ar[i] * k;
        ag[i] = p[i * 4 + 1] * opacity + ag[i] * k;
        ab_[i] = p[i * 4 + 2] * opacity + ab_[i] * k;
        aa[i] = sa + aa[i] * k;
      }
    } else {
      for (int i = 0; i < n; ++i) {
        const float sa = p[i * 4 + 3] * opacity;
        const float ba = aa[i];
        if (sa <= 0.0f) continue;
        const float invSa = p[i * 4 + 3] > 0.0f ? 1.0f / p[i * 4 + 3] : 0.0f;
        const float invBa = ba > 0.0f ? 1.0f / ba : 0.0f;
        float* dst[3] = {ar + i, ag + i, ab_ + i};
        for (int c = 0; c < 3; ++c) {
          const float Cs = p[i * 4 + c] * opacity;
          const float Cb = *dst[c];
          const float cs = p[i * 4 + c] * invSa;  // unpremultiplied
          const float cb = Cb * invBa;
          const float bl = blendChannel(mode, cb, cs);
          *dst[c] = (1.0f - ba) * Cs + (1.0f - sa) * Cb + sa * ba * bl;
        }
        aa[i] = sa + ba - sa * ba;
      }
    }
    x = segEnd;
  }
}

void Canvas::mergeInto(Canvas& dst, float opacity, int mode) const {
  if (opacity <= 0.0f) return;
  for (const auto& kv : tiles) {
    const int tx = keyTx(kv.first), ty = keyTy(kv.first);
    Tile* d = dst.getOrCreate(tx, ty);
    const float* s = kv.second->px;
    for (int i = 0; i < TILE_PIXELS; ++i) blendPixel(mode, opacity, s + i * 4, d->px + i * 4);
  }
  // A blend can leave a tile empty (minus against itself, say); keep the canvas sparse.
  for (auto it = dst.tiles.begin(); it != dst.tiles.end();)
    it = it->second->allZero() ? dst.tiles.erase(it) : std::next(it);
  dst.dirty = true;
}

bool Canvas::sampleBilinear(float x, float y, float out[4]) const {
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  float acc[4] = {0, 0, 0, 0};
  bool any = false;
  for (int j = 0; j < 2; ++j) {
    const int py = y0 + j;
    const float wy = j ? fy : 1.0f - fy;
    if (wy <= 0.0f) continue;
    for (int i = 0; i < 2; ++i) {
      const int px = x0 + i;
      const float w = wy * (i ? fx : 1.0f - fx);
      if (w <= 0.0f) continue;
      const Tile* t = find(tileOf(px), tileOf(py));
      if (!t) continue;
      const float* p = t->px + (size_t(py & TILE_MASK) * TILE + (px & TILE_MASK)) * 4;
      for (int c = 0; c < 4; ++c) acc[c] += p[c] * w;
      any = true;
    }
  }
  if (!any) return false;
  for (int c = 0; c < 4; ++c) out[c] = acc[c];
  return true;
}

bool Canvas::rowHasPaintScaled(int y, int x0, int x1, double sx, double sy) const {
  if (tiles.empty() || x1 <= x0) return false;
  if (sx == 1.0 && sy == 1.0) return rowHasPaint(y, x0, x1);
  if (sx <= 0.0 || sy <= 0.0) return false;
  // One output row covers a band of canvas rows; check the whole band.
  const int cy0 = int(std::floor((double(y) - 0.5) / sy)) - 1;
  const int cy1 = int(std::ceil((double(y) + 1.5) / sy)) + 1;
  const int cx0 = int(std::floor((double(x0) - 0.5) / sx)) - 1;
  const int cx1 = int(std::ceil((double(x1) + 0.5) / sx)) + 1;
  for (int ty = tileOf(cy0); ty <= tileOf(cy1); ++ty)
    for (int tx = tileOf(cx0); tx <= tileOf(cx1); ++tx)
      if (find(tx, ty)) return true;
  return false;
}

void Canvas::blendRowScaled(int y, int x0, int x1, float* const acc[4], float opacity, int mode,
                            double sx, double sy) const {
  if (sx == 1.0 && sy == 1.0) { blendRow(y, x0, x1, acc, opacity, mode); return; }
  if (opacity <= 0.0f || x1 <= x0 || sx <= 0.0 || sy <= 0.0) return;
  const float cy = float((double(y) + 0.5) / sy - 0.5);
  float src[4];
  for (int x = x0; x < x1; ++x) {
    const float cx = float((double(x) + 0.5) / sx - 0.5);
    if (!sampleBilinear(cx, cy, src)) continue;
    if (src[3] <= 0.0f && src[0] == 0.0f && src[1] == 0.0f && src[2] == 0.0f) continue;
    const int i = x - x0;
    float dst[4] = {acc[0][i], acc[1][i], acc[2][i], acc[3][i]};
    blendPixel(mode, opacity, src, dst);
    for (int c = 0; c < 4; ++c) acc[c][i] = dst[c];
  }
}

// ---------------------------------------------------------------- PackBits

void packBitsEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < n) {
    // Measure the run starting at i.
    size_t run = 1;
    while (i + run < n && run < 128 && src[i + run] == src[i]) ++run;
    if (run >= 3) {
      out.push_back(uint8_t(257 - run));
      out.push_back(src[i]);
      i += run;
      continue;
    }
    // Literal block: extend until a run of 3 starts or 128 bytes.
    size_t start = i, len = 0;
    while (i < n && len < 128) {
      if (i + 2 < n && src[i] == src[i + 1] && src[i] == src[i + 2]) break;
      ++i; ++len;
    }
    out.push_back(uint8_t(len - 1));
    out.insert(out.end(), src + start, src + start + len);
  }
}

size_t packBitsDecode(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
  size_t i = 0, o = 0;
  while (i < srcLen && o < dstLen) {
    const int8_t c = int8_t(src[i++]);
    if (c >= 0) {
      size_t len = size_t(c) + 1;
      len = std::min(len, std::min(srcLen - i, dstLen - o));
      std::memcpy(dst + o, src + i, len);
      i += len; o += len;
    } else if (c != -128) {
      if (i >= srcLen) break;
      size_t len = std::min(size_t(1 - int(c)), dstLen - o);
      std::memset(dst + o, src[i++], len);
      o += len;
    }
  }
  return o;
}

// ---------------------------------------------------------------- file IO
// Layout: "RPT1" | int32 tileSize | int32 tileCount | per tile: int32 tx, int32 ty,
// uint32 packedBytes, packed data. A tile is split into its 4 byte planes
// (byte k of every float) and PackBits'd, so flat/empty areas shrink to nothing
// while the data stays bit-exact. Written to a temp file and renamed.

namespace {
const size_t kRaw = sizeof(float) * TILE_PIXELS * 4;

template <class T> bool wr(FILE* f, const T& v) { return std::fwrite(&v, sizeof(T), 1, f) == 1; }
template <class T> bool rd(FILE* f, T& v) { return std::fread(&v, sizeof(T), 1, f) == 1; }
}  // namespace

bool Canvas::save(const std::string& path) const {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite("RPT1", 1, 4, f) == 4 && wr(f, int32_t(TILE)) && wr(f, int32_t(tiles.size()));
  std::vector<uint8_t> planes(kRaw), packed;
  for (const auto& kv : tiles) {
    if (!ok) break;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(kv.second->px);
    const size_t nf = kRaw / 4;
    for (size_t i = 0; i < nf; ++i)
      for (size_t b = 0; b < 4; ++b) planes[b * nf + i] = raw[i * 4 + b];
    packed.clear();
    packBitsEncode(planes.data(), kRaw, packed);
    ok = wr(f, int32_t(keyTx(kv.first))) && wr(f, int32_t(keyTy(kv.first))) &&
         wr(f, uint32_t(packed.size())) &&
         std::fwrite(packed.data(), 1, packed.size(), f) == packed.size();
  }
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) { std::remove(tmp.c_str()); return false; }
  std::remove(path.c_str());  // rename() does not overwrite on Windows
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool Canvas::load(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char magic[4];
  int32_t ts = 0, count = 0;
  bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "RPT1", 4) == 0 && rd(f, ts) &&
            rd(f, count) && ts == TILE && count >= 0;
  TileMap loaded;
  std::vector<uint8_t> planes(kRaw), packed;
  for (int32_t n = 0; ok && n < count; ++n) {
    int32_t tx, ty;
    uint32_t sz;
    ok = rd(f, tx) && rd(f, ty) && rd(f, sz) && sz <= kRaw * 2 + 1024;
    if (!ok) break;
    packed.resize(sz);
    ok = std::fread(packed.data(), 1, sz, f) == sz &&
         packBitsDecode(packed.data(), sz, planes.data(), kRaw) == kRaw;
    if (!ok) break;
    std::unique_ptr<Tile> t(new Tile);
    uint8_t* raw = reinterpret_cast<uint8_t*>(t->px);
    const size_t nf = kRaw / 4;
    for (size_t i = 0; i < nf; ++i)
      for (size_t b = 0; b < 4; ++b) raw[i * 4 + b] = planes[b * nf + i];
    loaded[tileKey(tx, ty)] = std::move(t);
  }
  std::fclose(f);
  if (!ok) return false;
  tiles = std::move(loaded);
  dirty = false;
  return true;
}

}  // namespace rp
