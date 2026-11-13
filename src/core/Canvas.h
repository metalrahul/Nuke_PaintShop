// NPaintShop core - sparse tiled raster canvas.
// No Nuke dependency: everything in src/core builds and is tested standalone.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Blend.h"

namespace rp {

constexpr int TILE_SHIFT = 7;
constexpr int TILE = 1 << TILE_SHIFT;  // 128 px
constexpr int TILE_MASK = TILE - 1;
constexpr int TILE_PIXELS = TILE * TILE;

// Half-open integer rectangle [x0,x1) x [y0,y1).
struct IRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  void merge(const IRect& o) {
    if (o.empty()) return;
    if (empty()) { *this = o; return; }
    x0 = std::min(x0, o.x0); y0 = std::min(y0, o.y0);
    x1 = std::max(x1, o.x1); y1 = std::max(y1, o.y1);
  }
};

// RGBA float, interleaved, premultiplied. 256 KB per tile.
struct Tile {
  float px[TILE_PIXELS * 4];
  Tile() { std::memset(px, 0, sizeof(px)); }
  bool allZero() const {
    for (int i = 0; i < TILE_PIXELS * 4; ++i)
      if (px[i] != 0.0f) return false;
    return true;
  }
};

using TileMap = std::unordered_map<uint64_t, std::unique_ptr<Tile>>;

inline uint64_t tileKey(int tx, int ty) {
  return (uint64_t(uint32_t(tx)) << 32) | uint64_t(uint32_t(ty));
}
inline int keyTx(uint64_t k) { return int(uint32_t(k >> 32)); }
inline int keyTy(uint64_t k) { return int(uint32_t(k & 0xffffffffu)); }
inline int tileOf(int v) { return v >> TILE_SHIFT; }  // floor division, works for negatives
inline IRect tileRect(int tx, int ty) {
  return IRect{tx * TILE, ty * TILE, (tx + 1) * TILE, (ty + 1) * TILE};
}

// One painted layer (one frame, or the single "all frames" layer).
// The paint lives in the pixels - strokes are never stored.
class Canvas {
 public:
  // Renderer threads take this shared, the painting thread takes it unique per dab batch.
  mutable std::shared_mutex mutex;
  TileMap tiles;
  bool dirty = false;  // has unsaved changes

  const Tile* find(int tx, int ty) const {
    auto it = tiles.find(tileKey(tx, ty));
    return it == tiles.end() ? nullptr : it->second.get();
  }
  Tile* getOrCreate(int tx, int ty) {
    auto& slot = tiles[tileKey(tx, ty)];
    if (!slot) slot.reset(new Tile);
    return slot.get();
  }
  bool empty() const { return tiles.empty(); }

  // Pixel bounds of all allocated tiles (tile granularity).
  IRect bounds() const;

  // True if any tile intersects row y within [x0,x1).
  bool rowHasPaint(int y, int x0, int x1) const;

  // out = paint OVER in, for row y and x in [x0,x1). Arrays are indexed from 0 (= x0).
  // Any of the four in/out pointers may be null (channel not requested / not present).
  // Caller must hold the mutex (shared is enough).
  void compositeRow(int y, int x0, int x1, const float* const in[4], float* const out[4]) const;

  // Blends this canvas's row onto acc in place, as a layer with the given opacity and
  // blend mode. acc is premultiplied RGBA, four separate channel arrays indexed from 0
  // (= x0); all four must be non-null. Caller holds the mutex (shared is enough).
  void blendRow(int y, int x0, int x1, float* const acc[4], float opacity, int mode) const;

  // Composites this canvas onto dst as a layer with the given opacity and mode.
  // Used by merge down and flatten. Caller holds both mutexes.
  void mergeInto(Canvas& dst, float opacity, int mode) const;

  // Bilinear sample of the premultiplied RGBA at a fractional position. Returns false
  // and leaves out untouched when nothing is allocated nearby.
  bool sampleBilinear(float x, float y, float out[4]) const;

  // As blendRow, but the canvas is in a different resolution to the output: a canvas
  // pixel maps to output position (x * sx, y * sy). Used when Nuke is proxied, where
  // the paint is kept at full resolution and scaled down on the way out.
  void blendRowScaled(int y, int x0, int x1, float* const acc[4], float opacity, int mode,
                      double sx, double sy) const;

  // True if any tile could contribute to output row y over [x0,x1) at the given scale.
  bool rowHasPaintScaled(int y, int x0, int x1, double sx, double sy) const;

  // Lossless sparse file IO. Returns false on failure. Caller holds the mutex.
  bool save(const std::string& path) const;
  bool load(const std::string& path);

  size_t memoryBytes() const { return tiles.size() * sizeof(Tile); }
};

// PackBits (the RLE used by PSD/ABR). Shared by the canvas file format and the ABR reader.
void packBitsEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out);
// Decodes until dstLen bytes are produced or src is exhausted. Returns bytes produced.
size_t packBitsDecode(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen);

}  // namespace rp
