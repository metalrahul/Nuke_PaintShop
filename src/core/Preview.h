// NPaintShop core - preview renderers for the viewer UI.
// These run the real stroke engine, so what a preview shows is what the brush paints.
#pragma once
#include <vector>

#include "PaintEngine.h"

namespace rp {

// Renders a tapered sample stroke into a w x h premultiplied RGBA buffer (row-major,
// bottom-up like the canvas). Honours tip, hardness, opacity, flow, spacing and the
// pressure switches; the diameter is capped so the stroke always fits the swatch.
void renderStrokePreview(const BrushParams& p, int w, int h, std::vector<float>& rgba);

// Square coverage thumbnail of a brush tip, 0..1, row-major. Aspect is preserved and
// the tip is centred. An unsampled (round) tip renders as a disc of the given hardness.
void tipThumbnail(const BrushTip* tip, float hardness, int size, std::vector<float>& out);

}  // namespace rp
