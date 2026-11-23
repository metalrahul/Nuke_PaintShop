// NPaintShop core - blend modes.
//
// Everything in the canvas is premultiplied RGBA, so the compositing follows the
// standard separable-blend formula:
//
//   Co = (1 - ab) * Cs + (1 - as) * Cb + as * ab * B(cb, cs)
//   ao = as + ab - as * ab
//
// where lower case is the unpremultiplied colour. With B = cs that reduces exactly to
// "source over", so Normal costs nothing extra.
//
// Nuke works in scene-linear float and values above 1 are normal. Normal, multiply,
// screen, add, subtract, difference, darken and lighten behave sensibly there. The
// rest (overlay, the lights, dodge and burn) are defined against a 0..1 range and will
// do odd things to over-range values - that is inherent to the formulas, not a bug.
#pragma once
#include <algorithm>
#include <cmath>

namespace rp {

enum BlendMode {
  kNormal = 0, kMultiply, kScreen, kOverlay, kDarken, kLighten,
  kColorDodge, kColorBurn, kHardLight, kSoftLight, kDifference, kExclusion,
  kAdd, kSubtract, kBlendCount
};

// Names in the same order, for the node's menu. Null-terminated.
inline const char* const* blendNames() {
  static const char* const names[] = {
      "normal", "multiply", "screen", "overlay", "darken", "lighten",
      "color dodge", "color burn", "hard light", "soft light", "difference", "exclusion",
      "plus", "minus", nullptr};
  return names;
}

// B(cb, cs) on unpremultiplied channel values.
inline float blendChannel(int mode, float cb, float cs) {
  switch (mode) {
    case kMultiply: return cb * cs;
    case kScreen: return cb + cs - cb * cs;
    case kOverlay: return cb <= 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case kDarken: return std::min(cb, cs);
    case kLighten: return std::max(cb, cs);
    case kColorDodge:
      if (cb <= 0.0f) return 0.0f;
      if (cs >= 1.0f) return 1.0f;
      return std::min(1.0f, cb / (1.0f - cs));
    case kColorBurn:
      if (cb >= 1.0f) return 1.0f;
      if (cs <= 0.0f) return 0.0f;
      return 1.0f - std::min(1.0f, (1.0f - cb) / cs);
    case kHardLight:
      return cs <= 0.5f ? 2.0f * cs * cb : 1.0f - 2.0f * (1.0f - cs) * (1.0f - cb);
    case kSoftLight: {  // the W3C / PDF definition
      if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
      const float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(std::max(cb, 0.0f));
      return cb + (2.0f * cs - 1.0f) * (d - cb);
    }
    case kDifference: return std::fabs(cb - cs);
    case kExclusion: return cb + cs - 2.0f * cb * cs;
    case kAdd: return cb + cs;
    case kSubtract: return cb - cs;
    default: return cs;  // kNormal
  }
}

// One premultiplied RGBA pixel of `src` composited onto `dst` in place, with the
// layer opacity folded in.
inline void blendPixel(int mode, float opacity, const float* src, float* dst) {
  const float sa = src[3] * opacity;
  if (sa <= 0.0f) return;  // with as = 0 every separable mode leaves the backdrop alone
  const float ba = dst[3];
  if (mode == kNormal) {
    const float k = 1.0f - sa;
    for (int c = 0; c < 3; ++c) dst[c] = src[c] * opacity + dst[c] * k;
    dst[3] = sa + ba * k;
    return;
  }
  const float invSa = src[3] > 0.0f ? 1.0f / src[3] : 0.0f;
  const float invBa = ba > 0.0f ? 1.0f / ba : 0.0f;
  for (int c = 0; c < 3; ++c) {
    const float Cs = src[c] * opacity, Cb = dst[c];
    const float bl = blendChannel(mode, Cb * invBa, src[c] * invSa);
    dst[c] = (1.0f - ba) * Cs + (1.0f - sa) * Cb + sa * ba * bl;
  }
  dst[3] = sa + ba - sa * ba;
}

// blendPixel with the mode fixed at compile time. The per-pixel switch folds away, and
// over an empty backdrop (alpha 0) every separable mode is plain source-over, which
// skips the divisions entirely. Gives exactly the same results as blendPixel.
template <int M>
inline void blendPixelT(float opacity, const float* src, float* dst) {
  const float sa = src[3] * opacity;
  if (sa <= 0.0f) return;
  const float ba = dst[3];
  if (M == kNormal || ba <= 0.0f) {
    const float k = 1.0f - sa;
    for (int c = 0; c < 3; ++c) dst[c] = src[c] * opacity + dst[c] * k;
    dst[3] = sa + ba * k;
    return;
  }
  const float invSa = src[3] > 0.0f ? 1.0f / src[3] : 0.0f;
  const float invBa = 1.0f / ba;
  for (int c = 0; c < 3; ++c) {
    const float Cs = src[c] * opacity, Cb = dst[c];
    const float bl = blendChannel(M, Cb * invBa, src[c] * invSa);
    dst[c] = (1.0f - ba) * Cs + (1.0f - sa) * Cb + sa * ba * bl;
  }
  dst[3] = sa + ba - sa * ba;
}

template <int M> struct BlendTag { static constexpr int value = M; };

// Calls f(BlendTag<mode>{}) with the mode as a compile-time constant, so a loop
// written once runs as a separate, switch-free loop for each mode.
template <class F>
inline void withBlend(int mode, F&& f) {
  switch (mode) {
    case kMultiply: f(BlendTag<kMultiply>{}); break;
    case kScreen: f(BlendTag<kScreen>{}); break;
    case kOverlay: f(BlendTag<kOverlay>{}); break;
    case kDarken: f(BlendTag<kDarken>{}); break;
    case kLighten: f(BlendTag<kLighten>{}); break;
    case kColorDodge: f(BlendTag<kColorDodge>{}); break;
    case kColorBurn: f(BlendTag<kColorBurn>{}); break;
    case kHardLight: f(BlendTag<kHardLight>{}); break;
    case kSoftLight: f(BlendTag<kSoftLight>{}); break;
    case kDifference: f(BlendTag<kDifference>{}); break;
    case kExclusion: f(BlendTag<kExclusion>{}); break;
    case kAdd: f(BlendTag<kAdd>{}); break;
    case kSubtract: f(BlendTag<kSubtract>{}); break;
    default: f(BlendTag<kNormal>{}); break;
  }
}

// True for the modes that reduce to plain source-over, which can skip the
// unpremultiply/blend/premultiply round trip entirely.
inline bool isPassThrough(int mode) { return mode == kNormal; }

}  // namespace rp
