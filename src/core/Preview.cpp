#include "Preview.h"

#include <cmath>
#include <memory>

namespace rp {

void renderStrokePreview(const BrushParams& p, int w, int h, std::vector<float>& rgba) {
  rgba.assign(size_t(std::max(w, 0)) * std::max(h, 0) * 4, 0.0f);
  if (w <= 0 || h <= 0) return;

  BrushParams q = p;
  q.tool = Tool::Paint;  // a swatch always shows what the brush lays down
  q.cloneDx = q.cloneDy = 0;
  // Keep the stroke inside the swatch whatever the real brush size is, but let small
  // brushes still read as small.
  const float maxDiam = float(h) * 0.72f;
  q.size = std::min(std::max(p.size, 1.0f), maxDiam);

  const float margin = q.size * 0.5f + 1.0f;
  const float x0 = margin, x1 = float(w) - margin;
  if (x1 <= x0) return;
  const float midY = float(h) * 0.5f;
  const float amp = std::max(0.0f, (float(h) - q.size) * 0.5f - 1.0f);

  auto canvas = std::make_shared<Canvas>();
  PaintEngine eng;
  const int steps = std::max(24, w / 3);
  auto at = [&](int i, float& x, float& y, float& pr) {
    const float t = float(i) / float(steps);
    x = x0 + (x1 - x0) * t;
    y = midY + std::sin(t * 6.2831853f) * amp * 0.5f;  // one gentle S
    pr = 0.15f + 0.85f * std::sin(t * 3.14159265f);    // taper in and out
  };

  float x, y, pr;
  at(0, x, y, pr);
  eng.beginStroke(canvas, q, x, y, pr);
  for (int i = 1; i <= steps; ++i) {
    at(i, x, y, pr);
    eng.strokeTo(x, y, pr);
  }
  eng.endStroke();

  for (int py = 0; py < h; ++py) {
    for (int px = 0; px < w; ++px) {
      const Tile* t = canvas->find(tileOf(px), tileOf(py));
      float* o = rgba.data() + (size_t(py) * w + px) * 4;
      if (!t) continue;
      const float* s = t->px + (size_t(py & TILE_MASK) * TILE + (px & TILE_MASK)) * 4;
      o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3];
    }
  }
}

void tipThumbnail(const BrushTip* tip, float hardness, int size, std::vector<float>& out) {
  out.assign(size_t(std::max(size, 0)) * std::max(size, 0), 0.0f);
  if (size <= 0) return;
  const float d = float(size) - 2.0f;  // one pixel of air on each side
  if (d <= 0.0f) return;
  const IRect box{0, 0, size, size};
  const float c = float(size) * 0.5f;
  DabShape sh;
  sh.diameter = d;
  sh.hardness = hardness;
  rasterizeDab(tip && tip->sampled() ? tip : nullptr, c, c, sh, box, out.data());
}

}  // namespace rp
