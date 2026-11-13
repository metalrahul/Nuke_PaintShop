// NPaintShop core - brush tips, dab rasterisation and the Photoshop .abr reader.
#pragma once
#include <string>
#include <vector>

#include "Canvas.h"

namespace rp {

// How a property varies along a stroke, following Photoshop's jitter / control /
// minimum model. The jitter picks a random factor in [1-jitter, 1], the control
// scales it by pressure or by the fade ramp, and minimum is the floor of the result.
struct DynControl {
  float jitter = 0.0f;
  float minimum = 0.0f;
  int control = 0;     // 0 off, 1 fade, 2 pen pressure
  int fadeSteps = 25;  // dabs to fade over when control == fade
};

// Angle control has two extra modes that follow the stroke rather than the pen.
enum AngleControl { kAngleOff = 0, kAngleFade, kAnglePressure, kAngleDirection };

struct BrushDynamics {
  // brush tip shape
  float angle = 0.0f;      // degrees, counter-clockwise
  float roundness = 1.0f;  // 1 = circular, squashes the short axis
  bool flipX = false, flipY = false;

  // shape dynamics
  DynControl size, round;
  float angleJitter = 0.0f;  // 1 = a full turn either way
  int angleControl = kAngleOff;
  int angleFadeSteps = 25;
  bool flipXJitter = false, flipYJitter = false;

  // scattering
  float scatter = 0.0f;      // multiples of the dab diameter, across the stroke
  bool scatterBoth = false;  // scatter along the stroke as well
  int count = 1;             // dabs per spacing step
  float countJitter = 0.0f;

  // transfer
  DynControl opacity, flow;

  unsigned seed = 1;  // per-stroke RNG seed, so a stroke is reproducible
  bool any() const;   // false when every setting is at its default
};

// A brush tip. With no mip levels it is the procedural round brush (size + hardness);
// otherwise it is a sampled grey-scale tip (e.g. from an .abr) with a mip pyramid so
// that any dab size is one bilinear lookup per pixel.
struct BrushTip {
  struct Level {
    int w = 0, h = 0;
    std::vector<float> a;  // coverage 0..1, row-major
  };
  std::string name = "round";
  std::string id;  // the brush file's id for this bitmap, used to match presets to tips
  std::vector<Level> mips;
  float spacingHint = 0.0f;  // fraction of diameter, 0 = unknown

  bool sampled() const { return !mips.empty(); }
  int width() const { return mips.empty() ? 0 : mips[0].w; }
  int height() const { return mips.empty() ? 0 : mips[0].h; }
  void setImage(int w, int h, std::vector<float> coverage);  // builds the pyramid
};

// The shape of one dab. `diameter` is the long axis in pixels; `roundness` squashes
// the short axis; `angle` rotates counter-clockwise, in radians, about the dab centre.
struct DabShape {
  float diameter = 1.0f;
  float hardness = 0.8f;    // round brush only
  float angle = 0.0f;       // radians
  float roundness = 1.0f;   // 1 = circular, 0 = flat
  bool flipX = false, flipY = false;
};

// Pixel box touched by a dab centred at (cx, cy).
IRect dabBounds(const BrushTip* tip, float cx, float cy, const DabShape& s);

// Writes coverage (0..1) for every pixel of `box` into cov (row-major, box-sized).
// tip == nullptr or an unsampled tip gives the round brush.
void rasterizeDab(const BrushTip* tip, float cx, float cy, const DabShape& s, const IRect& box,
                  float* cov);

// One entry from a brush file's preset list: a name, which tip it uses, and the
// settings Photoshop stored with it.
struct BrushPreset {
  std::string name;
  int tip = -1;            // index into AbrFile::tips, or -1 for the round brush
  float diameter = 50.0f;  // pixels
  float spacing = 0.25f;   // fraction of the diameter
  float hardness = 1.0f;   // round brush only
  BrushDynamics dyn;       // angle, roundness, flips and every jitter
  bool fromFile = false;   // false if the settings are just defaults
};

// A loaded Photoshop brush file. `tips` holds the bitmaps, `presets` the named brushes
// that use them; several presets commonly share one tip with different settings.
struct AbrFile {
  std::vector<BrushTip> tips;
  std::vector<BrushPreset> presets;
  bool empty() const { return presets.empty(); }
};

// Loads a Photoshop .abr (versions 1, 2, 6, 7, 10). Versions 6+ also carry a descriptor
// block with the real brush names and their settings, which is read when present; for
// the older formats a preset is synthesised per tip. Computed (parametric) brushes
// become presets on the round tip. Returns an empty file and fills *err on failure.
AbrFile loadABR(const std::string& path, std::string* err = nullptr);

}  // namespace rp
