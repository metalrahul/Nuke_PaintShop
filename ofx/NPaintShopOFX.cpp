// NPaintShop - OpenFX version.
//
// The same raster paint engine as the Nuke (NDK) node, wrapped as an OpenFX plugin so it
// loads where NDK plugins cannot - Nuke Indie in particular. All pixel work is in
// src/core and shared with the NDK node; this file is the OFX glue: parameters, the
// viewer overlay that takes the pen, rendering and saving.
//
// Differences from the NDK node, all down to what OFX offers:
//   - Pen pressure is read from the tablet driver directly (PenPressure.h), because
//     Nuke passes 1.0 to OFX overlays.
//   - Paint is saved when the host syncs the plugin before writing the script
//     (kOfxActionSyncPrivateData), either to sidecar files or inside the script.
//   - One layer for now. Clone and smudge need the input image during a stroke, which
//     OFX only hands out to renders; they come in a later pass.
#ifdef _WIN32
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // the pen pointer API needs Windows 8 or later
#endif
#include <windows.h>
#endif
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <chrono>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>
#include <sstream>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsInteract.h"
#include "ofxKeySyms.h"
#include "BrushLibrary.h"
#include "Overlay.h"
#include "PaintEngine.h"
#include "PenPressure.h"

namespace fs = std::filesystem;

namespace {

const char* const kPluginId = "com.rkvfx.NPaintShop";
constexpr int kAllFrames = INT_MIN;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------
// Parameters that describe the brush. One table drives the parameter definitions,
// "Reset brush", presets and the brush file's own settings, so they cannot drift apart.
// The names match the NDK node's knobs, so presets saved in either version load in both.

enum PType { kDouble, kInt, kBool, kChoice };
enum Page { kMain, kDyn };

const char* const kControl[] = {"off", "fade", "pen pressure", nullptr};
const char* const kAngleCtl[] = {"off", "fade", "pen pressure", "direction", nullptr};

struct PDef {
  Page page;
  const char* group;  // dynamics group, or nullptr
  const char* name;
  const char* label;
  PType type;
  double def, lo, hi;
  const char* const* choices;
  const char* hint;
};

const char* const kFadeHint =
    "How many dabs the fade takes to reach the minimum below. Only used when control is set to fade.";
const char* const kCtlHint =
    "What drives this along the stroke. 'fade' ramps it down over the next few dabs; "
    "'pen pressure' takes it from the tablet, and does nothing with a mouse.";

const PDef kBrush[] = {
    {kMain, nullptr, "size", "size", kDouble, 50, 1, 5000, nullptr,
     "Brush diameter in pixels. Ctrl+Shift+drag in the viewer to change it."},
    {kMain, nullptr, "hardness", "hardness", kDouble, 0.8, 0, 1, nullptr, "Edge hardness of the round brush."},
    {kMain, nullptr, "opacity", "opacity", kDouble, 1, 0, 1, nullptr,
     "Maximum opacity of one stroke, however much it overlaps itself."},
    {kMain, nullptr, "flow", "flow", kDouble, 1, 0, 1, nullptr, "How fast each dab builds up towards the stroke opacity."},
    {kMain, nullptr, "spacing", "spacing", kDouble, 0.1, 0.01, 1, nullptr,
     "Distance between dabs as a fraction of the brush size. Larger is faster and dottier."},
    {kMain, nullptr, "brush_blend", "brush blend", kChoice, 0, 0, 0, rp::blendNames(),
     "How the stroke combines with the paint already there. Erase ignores it."},
    {kMain, nullptr, "smoothing", "smoothing", kDouble, 0, 0, 1, nullptr,
     "Makes the brush chase the pointer instead of following it exactly, which takes the "
     "wobble out of a freehand line. The stroke still ends where you lifted off."},
    {kMain, nullptr, "pressure_size", "pressure > size", kBool, 1, 0, 1, nullptr, "Pen pressure controls the brush size."},
    {kMain, nullptr, "pressure_flow", "pressure > flow", kBool, 0, 0, 1, nullptr, "Pen pressure controls the flow."},

    {kDyn, "brush tip shape", "dyn_angle", "angle", kDouble, 0, -180, 180, nullptr,
     "Rotates the tip. Only visible on a non-round tip or with roundness below 1."},
    {kDyn, "brush tip shape", "dyn_roundness", "roundness", kDouble, 1, 0, 1, nullptr, "Squashes the tip's short axis. 1 is circular."},
    {kDyn, "brush tip shape", "dyn_flip_x", "flip x", kBool, 0, 0, 1, nullptr, "Mirrors the tip left to right."},
    {kDyn, "brush tip shape", "dyn_flip_y", "flip y", kBool, 0, 0, 1, nullptr, "Mirrors the tip top to bottom."},

    {kDyn, "shape dynamics", "size_jitter", "size jitter", kDouble, 0, 0, 1, nullptr, "Randomly shrinks each dab by up to this fraction."},
    {kDyn, "shape dynamics", "size_control", "size control", kChoice, 0, 0, 0, kControl, kCtlHint},
    {kDyn, "shape dynamics", "size_fade", "size fade steps", kInt, 25, 1, 200, nullptr, kFadeHint},
    {kDyn, "shape dynamics", "size_minimum", "size minimum", kDouble, 0, 0, 1, nullptr,
     "Floor for the size factor, so a fade or jitter cannot shrink past this."},
    {kDyn, "shape dynamics", "angle_jitter", "angle jitter", kDouble, 0, 0, 1, nullptr, "1 randomises the tip angle over a full turn."},
    {kDyn, "shape dynamics", "angle_control", "angle control", kChoice, 0, 0, 0, kAngleCtl,
     "'direction' turns the tip to follow the stroke, which makes a flat tip behave like a calligraphic nib."},
    {kDyn, "shape dynamics", "angle_fade", "angle fade steps", kInt, 25, 1, 200, nullptr, kFadeHint},
    {kDyn, "shape dynamics", "round_jitter", "roundness jitter", kDouble, 0, 0, 1, nullptr,
     "Randomly squashes each dab by up to this fraction."},
    {kDyn, "shape dynamics", "round_control", "roundness control", kChoice, 0, 0, 0, kControl, kCtlHint},
    {kDyn, "shape dynamics", "round_fade", "roundness fade steps", kInt, 25, 1, 200, nullptr, kFadeHint},
    {kDyn, "shape dynamics", "round_minimum", "roundness minimum", kDouble, 0, 0, 1, nullptr, "Floor for roundness."},
    {kDyn, "shape dynamics", "flip_x_jitter", "flip x jitter", kBool, 0, 0, 1, nullptr, "Mirrors each dab left to right at random."},
    {kDyn, "shape dynamics", "flip_y_jitter", "flip y jitter", kBool, 0, 0, 1, nullptr, "Mirrors each dab top to bottom at random."},

    {kDyn, "scattering", "scatter", "scatter", kDouble, 0, 0, 5, nullptr,
     "Throws each dab off the stroke by up to this many brush diameters."},
    {kDyn, "scattering", "scatter_both", "both axes", kBool, 0, 0, 1, nullptr, "Scatter along the stroke as well as across it."},
    {kDyn, "scattering", "scatter_count", "count", kInt, 1, 1, 16, nullptr, "Dabs laid down at every spacing step."},
    {kDyn, "scattering", "count_jitter", "count jitter", kDouble, 0, 0, 1, nullptr, "Randomly drops some of the scattered copies."},

    {kDyn, "transfer", "opacity_jitter", "opacity jitter", kDouble, 0, 0, 1, nullptr, "Randomly fades each dab by up to this fraction."},
    {kDyn, "transfer", "opacity_control", "opacity control", kChoice, 0, 0, 0, kControl, kCtlHint},
    {kDyn, "transfer", "opacity_fade", "opacity fade steps", kInt, 25, 1, 200, nullptr, kFadeHint},
    {kDyn, "transfer", "opacity_minimum", "opacity minimum", kDouble, 0, 0, 1, nullptr, "Floor for opacity."},
    {kDyn, "transfer", "flow_jitter", "flow jitter", kDouble, 0, 0, 1, nullptr,
     "Randomly reduces how much paint each dab puts down; reads as a grainier stroke."},
    {kDyn, "transfer", "flow_control", "flow control", kChoice, 0, 0, 0, kControl, kCtlHint},
    {kDyn, "transfer", "flow_fade", "flow fade steps", kInt, 25, 1, 200, nullptr, kFadeHint},
    {kDyn, "transfer", "flow_minimum", "flow minimum", kDouble, 0, 0, 1, nullptr, "Floor for flow."},

    {kDyn, "noise and wet edges", "noise", "noise", kBool, 0, 0, 1, nullptr,
     "Adds grain to the soft part of the tip, fixed to the image like paper texture."},
    {kDyn, "noise and wet edges", "wet_edges", "wet edges", kBool, 0, 0, 1, nullptr,
     "Paint collects at the edges of the stroke and thins out in the middle, like watercolour."},

    {kDyn, "colour dynamics", "fgbg_jitter", "fg/bg jitter", kDouble, 0, 0, 1, nullptr,
     "Mixes each dab a random amount of the way from the colour towards the background colour."},
    {kDyn, "colour dynamics", "fgbg_control", "fg/bg control", kChoice, 0, 0, 0, kControl,
     "'fade' moves to the background colour over the next few dabs. 'pen pressure' gives the "
     "foreground at full pressure and the background at the lightest touch."},
    {kDyn, "colour dynamics", "fgbg_fade", "fg/bg fade steps", kInt, 25, 1, 200, nullptr, kFadeHint},
    {kDyn, "colour dynamics", "fgbg_minimum", "fg/bg minimum", kDouble, 0, 0, 1, nullptr,
     "How much foreground colour is always kept."},
    {kDyn, "colour dynamics", "hue_jitter", "hue jitter", kDouble, 0, 0, 1, nullptr, "Shifts each dab's hue at random."},
    {kDyn, "colour dynamics", "sat_jitter", "saturation jitter", kDouble, 0, 0, 1, nullptr, "Varies each dab's saturation at random."},
    {kDyn, "colour dynamics", "bri_jitter", "brightness jitter", kDouble, 0, 0, 1, nullptr,
     "Varies each dab's brightness at random. Scales rather than clips, so HDR values behave."},
    {kDyn, "colour dynamics", "purity", "purity", kDouble, 0, -1, 1, nullptr,
     "Shifts the saturation of every dab: -1 greys it out, 1 saturates it fully."},
    {kDyn, "colour dynamics", "color_per_tip", "apply per tip", kBool, 1, 0, 1, nullptr,
     "On: every dab gets its own colour. Off: one colour per stroke."},

    {kDyn, nullptr, "dyn_seed", "random seed", kInt, 1, 1, 9999, nullptr,
     "Every stroke replays the same random sequence from this seed, so a stroke you like is reproducible."},
};

const PDef* findDef(const std::string& name) {
  for (const PDef& d : kBrush)
    if (name == d.name) return &d;
  return nullptr;
}

const char* const kLifetime[] = {"all frames", "single frame", nullptr};
const char* const kPaintChannels[] = {"rgba", "rgb", "alpha", nullptr};
const char* const kOutput[] = {"paint over input", "paint only", nullptr};
const char* const kStorage[] = {"sidecar files", "inside the script", nullptr};
const char* const kPenSource[] = {"automatic", "Windows Ink", "Wintab", "off (mouse)", nullptr};
const char* const kNoneMenu[] = {"(none)", nullptr};
const char* const kRoundMenu[] = {"round", nullptr};
const char* const kPaintMenu[] = {"Paint 1", nullptr};
const char* const kCloneInputs[] = {"A (this input)", "B (second input)", nullptr};
const char* const kPreviewColour[] = {"sRGB", "Rec.709", "gamma 2.2", "linear (raw)", nullptr};

// ---------------------------------------------------------------------------------
// Small helpers

std::string homeDir() {
  const char* h = std::getenv("USERPROFILE");
  if (!h || !*h) h = std::getenv("HOME");
  if (h && *h) return h;
  std::error_code ec;
  return fs::temp_directory_path(ec).generic_string();
}

// A memory limit in megabytes, from the environment when it is set there, as in the
// NDK node: limits describe the machine rather than the shot.
int envMB(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const long n = std::strtol(v, &end, 10);
  if (end == v || n < 0) return fallback;
  return int(std::min(n, 65536L));
}

// Where sidecar paint goes when 'paint directory' is left empty.
std::string defaultPaintRoot() {
  if (const char* e = std::getenv("NPAINTSHOP_PAINT_DIR"))
    if (*e) return e;
  return (fs::path(homeDir()) / ".nuke" / "NPaintShop" / "paint").generic_string();
}

std::string newId() {
  std::random_device rd;
  std::mt19937_64 g((uint64_t(rd()) << 32) ^ rd() ^
                    uint64_t(Clock::now().time_since_epoch().count()));
  char b[24];
  std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)g());
  return b;
}

std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    const unsigned char u = (unsigned char)c;
    if (u > 32 && u < 127 && c != '%' && c != '|' && c != ';' && c != ',') o += c;
    else { char b[8]; std::snprintf(b, sizeof(b), "%%%02X", u); o += b; }
  }
  return o;
}
std::string unesc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else o += s[i];
  }
  return o;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64encode(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = uint8_t(in[i]) << 16;
    if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
    out += kB64[v >> 18]; out += kB64[(v >> 12) & 63];
    out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}
std::string b64decode(const std::string& in) {
  static int8_t t[256];
  static bool init = false;
  if (!init) {
    std::memset(t, -1, sizeof(t));
    for (int i = 0; i < 64; ++i) t[uint8_t(kB64[i])] = int8_t(i);
    init = true;
  }
  std::string out;
  out.reserve(in.size() / 4 * 3);
  uint32_t v = 0;
  int bits = 0;
  for (char c : in) {
    const int8_t d = t[uint8_t(c)];
    if (d < 0) continue;  // padding, whitespace
    v = (v << 6) | uint32_t(d);
    bits += 6;
    if (bits >= 8) { bits -= 8; out += char((v >> bits) & 0xff); }
  }
  return out;
}

// A canvas to and from bytes, through the canvas's own file format. The file is the
// tested path, so a temporary file is used rather than a second encoder.
std::string tempPath() {
  std::error_code ec;
  return (fs::temp_directory_path(ec) / ("npaintshop_" + newId() + ".rpt")).generic_string();
}
bool canvasToBytes(const rp::Canvas& c, std::string& out) {
  const std::string p = tempPath();
  if (!c.save(p)) return false;
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  f.close();
  std::error_code ec;
  fs::remove(p, ec);
  return !out.empty();
}
bool canvasFromBytes(rp::Canvas& c, const std::string& bytes) {
  const std::string p = tempPath();
  {
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), std::streamsize(bytes.size()));
    if (!f) return false;
  }
  const bool ok = c.load(p);
  std::error_code ec;
  fs::remove(p, ec);
  return ok;
}

long long us(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
}

std::string sizeText(size_t bytes) {
  char b[32];
  if (bytes >= (size_t(1) << 30)) std::snprintf(b, sizeof(b), "%.1f GB", bytes / 1073741824.0);
  else if (bytes >= (size_t(1) << 20)) std::snprintf(b, sizeof(b), "%.0f MB", bytes / 1048576.0);
  else std::snprintf(b, sizeof(b), "%.0f KB", bytes / 1024.0);
  return b;
}

// Brush presets: the same file and format as the NDK node, so both share them.
struct Preset { std::string name, line; };
struct BrushPresets {
  static std::string path() {
    const fs::path dir = fs::path(homeDir()) / ".nuke";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "NPaintShop.presets").generic_string();
  }
  static std::vector<Preset> read() {
    std::vector<Preset> out;
    std::ifstream f(path());
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const size_t tab = line.find('\t');
      if (tab == std::string::npos || tab == 0) continue;
      out.push_back(Preset{unesc(line.substr(0, tab)), line.substr(tab + 1)});
    }
    return out;
  }
  static bool write(const std::vector<Preset>& ps) {
    const std::string p = path();
    {
      std::ofstream f(p + ".tmp");
      if (!f) return false;
      for (const Preset& e : ps) f << esc(e.name) << '\t' << e.line << '\n';
      if (!f) return false;
    }
    std::error_code ec;
    fs::remove(p, ec);
    fs::rename(p + ".tmp", p, ec);
    return !ec;
  }
};

// Modifier keys. Windows is asked directly, which is always right; elsewhere the
// overlay's own key events are tracked.
struct Mods {
  bool shift = false, ctrl = false;
};
Mods& trackedMods() { static Mods m; return m; }
Mods currentMods() {
#ifdef _WIN32
  Mods m;
  m.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  m.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  return m;
#else
  return trackedMods();
#endif
}

// The support library returns choice and string values through out-parameters.
int cv(OFX::ChoiceParam* p) { int v = 0; p->getValue(v); return v; }
int cv(OFX::ChoiceParam* p, double t) { int v = 0; p->getValueAtTime(t, v); return v; }
std::string sv(OFX::StringParam* p) { std::string v; p->getValue(v); return v; }

class PaintEffect;

// Live instances by paint id, so a copied node (which arrives carrying the original's
// id) can be told apart from the original and given its own copy of the paint.
std::mutex& registryMutex() { static std::mutex m; return m; }
std::map<std::string, PaintEffect*>& registry() { static std::map<std::string, PaintEffect*> r; return r; }

// ---------------------------------------------------------------------------------

// One paint layer. The pixels live in a canvas per frame key, so a layer works the same
// in "all frames" and "single frame" mode. `id` is stable for the life of the layer and
// names its files, so reordering never renames anything.
struct Layer {
  int id = 1;
  std::string name = "Paint 1";
  float opacity = 1.0f;
  int blend = rp::kNormal;
  bool visible = true, locked = false;
  std::map<int, std::shared_ptr<rp::Canvas>> canvases;
  bool contributes() const { return visible && opacity > 0.0f; }
};

// One layer's canvas for one frame, as a render sees it.
struct Shot {
  std::shared_ptr<rp::Canvas> canvas;
  float opacity = 1.0f;
  int blend = rp::kNormal;
};

// Frame and layer in one map key.
inline uint64_t ck(int id, int key) { return (uint64_t(uint32_t(id)) << 32) | uint64_t(uint32_t(key)); }
inline int ckId(uint64_t k) { return int(uint32_t(k >> 32)); }
inline int ckKey(uint64_t k) { return int(uint32_t(k & 0xffffffffu)); }

// Every instance's renders, for the stroke readout: if this node's renders stay at zero
// while these climb, the host is rendering through another instance of the node.
std::atomic<long>& allRenders() { static std::atomic<long> n{0}; return n; }
std::atomic<int>& liveInstances() { static std::atomic<int> n{0}; return n; }

const char* const kTools[] = {"paint", "erase", "clone", "smudge", nullptr};

class PaintEffect : public OFX::ImageEffect {
 public:
  explicit PaintEffect(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    ++liveInstances();
    dst_ = fetchClip(kOfxImageEffectOutputClipName);
    src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    tool_ = fetchChoiceParam("tool");
    color_ = fetchRGBParam("color");
    bgColor_ = fetchRGBParam("bg_color");
    brushSet_ = fetchChoiceParam("brush_set");
    brush_ = fetchChoiceParam("brush");
    useFileSettings_ = fetchBooleanParam("use_file_settings");
    preset_ = fetchChoiceParam("preset");
    lifetime_ = fetchChoiceParam("lifetime");
    channels_ = fetchChoiceParam("paint_channels");
    output_ = fetchChoiceParam("output");
    history_ = fetchIntParam("history");
    rev_ = fetchIntParam("paint_rev");
    penSource_ = fetchChoiceParam("pen_pressure");
    abrFile_ = fetchStringParam("abr_file");
    setName_ = fetchStringParam("brush_set_name");
    presetName_ = fetchStringParam("preset_name");
    storage_ = fetchChoiceParam("storage");
    paintDir_ = fetchStringParam("paint_dir");
    paintId_ = fetchStringParam("paint_id");
    paintData_ = fetchStringParam("paint_data");
    layerData_ = fetchStringParam("layer_data");
    undoMB_ = fetchIntParam("undo_memory");
    refreshMs_ = fetchIntParam("refresh_ms");
    penStatus_ = fetchStringParam("pen_status");
    storageStatus_ = fetchStringParam("storage_status");
    memStatus_ = fetchStringParam("memory_status");
    strokeStatus_ = fetchStringParam("stroke_status");
    layer_ = fetchChoiceParam("layer");
    layerName_ = fetchStringParam("layer_name");
    layerOpacity_ = fetchDoubleParam("layer_opacity");
    layerBlend_ = fetchChoiceParam("layer_blend");
    layerVisible_ = fetchBooleanParam("layer_visible");
    layerLocked_ = fetchBooleanParam("layer_locked");
    cloneOffset_ = fetchDouble2DParam("clone_offset");
    cloneTime_ = fetchDoubleParam("clone_time");
    smudgeStrength_ = fetchDoubleParam("smudge_strength");
    smudgeRate_ = fetchDoubleParam("smudge_rate");
    previewColourParam_ = fetchChoiceParam("preview_colour");
    cloneInput_ = fetchChoiceParam("clone_input");
    cloneCacheMB_ = fetchIntParam("clone_cache");
    frameMB_ = fetchIntParam("frame_memory");
    strokeDetails_ = fetchStringParam("stroke_details");
    cloneStatus_ = fetchStringParam("clone_status");
    if (getContext() == OFX::eContextGeneral) srcB_ = fetchClip("B");
    applyMemoryLimits();

    readStorageSettings();
    {
      std::lock_guard<std::mutex> g(mutex_);
      deserializeLocked(sv(layerData_));
      if (layers_.empty()) addLayerLocked("Paint 1");
    }
    try {
      claimId();
      refreshBrushSets();
      loadBrushes(false);
      refreshPresetMenu(-1);
      syncLayerParams();
    } catch (...) {
      // A host that refuses parameter changes while creating the instance still gets a
      // working node; the menus fill in on the first edit.
    }
    engine_.setUndoBudget(size_t(std::max(64, undoMB_->getValue())) << 20);
  }

  ~PaintEffect() override {
    --liveInstances();
    std::lock_guard<std::mutex> g(registryMutex());
    auto it = registry().find(id_);
    if (it != registry().end() && it->second == this) registry().erase(it);
  }

  // ------------------------------------------------------------------ overlay calls

  double brushSize() { return std::max(1.0, double(uiParams().size)); }
  double pixelAspect() const {
    const double par = src_ && src_->isConnected() ? src_->getPixelAspectRatio() : 1.0;
    return par > 1e-6 ? par : 1.0;
  }
  bool stroking() const { return engine_.stroking(); }
  void noteOverlayDraw() { if (engine_.stroking()) ++overlayDraws_; }
  void notePreviewDrawn(size_t tiles) {
    if (!engine_.stroking()) return;
    ++previewDraws_;
    previewTilesMax_ = std::max(previewTilesMax_, long(tiles));
  }

  // ---- live preview
  // Nuke does not render an OFX node while the pen is held down in its viewer, so
  // without help nothing appears until the pen lifts. The overlay therefore draws the
  // stroke itself while it is being painted: for each tile the stroke has touched, the
  // difference the stroke made, as an image to lay over what the viewer already shows.
  // Once the host's own render arrives after the stroke, the preview is dropped.
  struct PreviewTile {
    uint64_t key;
    std::vector<unsigned char> rgba;  // TILE x TILE, premultiplied, display-encoded
  };
  // Tiles changed since the last call, ready to upload. `serial` changes with each
  // stroke, so the overlay knows when to throw the previous stroke's tiles away.
  void takePreview(std::vector<PreviewTile>& out, int& serial) {
    serial = previewSerial_;
    if (engine_.stroking()) buildPreview();
    out = std::move(previewReady_);
    previewReady_.clear();
  }
  // True while the preview should still be drawn: during the stroke, and afterwards
  // until the host has rendered the result (or two seconds have gone by).
  bool previewVisible() const {
    if (previewSerial_ == 0) return false;
    if (engine_.stroking()) return true;
    const auto now = Clock::now();
    if (now - strokeEnd_ > std::chrono::seconds(3)) return false;
    if (renders_.load() <= rendersAtEnd_) return true;  // the host has not started yet
    // The host renders a frame in several pieces and shows it as they land. Dropping
    // the preview at the first piece showed the old image for a moment - a blink at
    // the end of every stroke - so it stays until the pieces have stopped arriving.
    const long long last = lastRenderNs_.load();
    const long long nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    return nowNs - last < 300000000LL;
  }

  // The brush as the parameters describe it, cached between parameter changes. The
  // overlay redraws on every pen move, and reading some sixty parameters through the
  // host each time is wasted work.
  const rp::BrushParams& uiParams() {
    if (paramsDirty_) {
      loadBrushes(false);
      cached_ = currentParams();
      refreshMsCached_ = std::max(10, refreshMs_->getValue());
      paramsDirty_ = false;
    }
    return cached_;
  }

  // Starts a stroke at canvas pixel (x, y). False if nothing was started.
  bool beginStroke(double x, double y, double pressure, double time) {
    if (engine_.stroking()) endStroke();
    claimId();
    rp::BrushParams p = uiParams();
    const int key = frameKey(time);
    std::shared_ptr<rp::Canvas> c;
    int skipId = -1;
    std::string refusal;
    {
      std::lock_guard<std::mutex> g(mutex_);
      Layer& L = activeLocked();
      if (L.locked || !L.visible) {
        refusal = "NPaintShop: layer '" + L.name + "' " +
                  (L.locked ? "is locked." : "is hidden, so the stroke would not be visible.");
      } else {
        c = canvasLocked(L, key, true);
        skipId = L.id;
      }
    }
    if (!refusal.empty()) {
      sendMessage(OFX::Message::eMessageWarning, "", refusal);
      return false;
    }

    rp::BgFetch bg;
    if (p.tool == rp::Tool::Clone || p.tool == rp::Tool::Smudge) {
      double dx = 0, dy = 0;
      cloneOffset_->getValue(dx, dy);
      if (cloneSrcPending_) {  // the first stroke after Ctrl+click fixes the offset
        dx = cloneSrcX_ - x;
        dy = cloneSrcY_ - y;
        cloneSrcPending_ = false;
        cloneOffset_->setValue(dx, dy);
      }
      if (p.tool == rp::Tool::Clone) {
        p.cloneDx = int(std::lround(dx));
        p.cloneDy = int(std::lround(dy));
      }
      bg = sourceFetch(time, skipId, p.tool == rp::Tool::Clone);
    }
    cloneStroke_ = p.tool == rp::Tool::Clone || p.tool == rp::Tool::Smudge;

    pressMin_ = pressMax_ = pressure;
    strokeStart_ = lastPaintRefresh_ = Clock::now();
    moves_ = 0;
    overlayDraws_ = 0;
    previewDraws_ = previewTilesMax_ = previewBuilt_ = 0;
    paintUs_ = 0;
    renders_ = 0;
    renderUs_ = 0;
    refreshes_ = 0;
    identities_ = 0;
    allAtStart_ = allRenders().load();
    rendersAtRefresh_ = 0;
    firstRenderUs_ = -1;
    engine_.setUndoBudget(size_t(std::max(64, undoMB_->getValue())) << 20);
    ++previewSerial_;
    previewDirty_.clear();
    previewReady_.clear();
    strokeKey_ = key;
    strokeTime_ = time;
    previewColour_ = cv(previewColourParam_);
    previewOutput_ = cv(output_);
    previewChannels_ = cv(channels_);
    {
      std::lock_guard<std::mutex> g(mutex_);
      previewOpacity_ = activeLocked().opacity;
    }
    strokeCanvas_ = c;
    const auto t0 = Clock::now();
    markPreview(engine_.beginStroke(c, p, float(x), float(y), float(pressure), bg));
    paintUs_ += us(t0);
    refresh();
    return true;
  }
  void strokeTo(double x, double y, double pressure) {
    if (!engine_.stroking()) return;
    pressMin_ = std::min(pressMin_, pressure);
    pressMax_ = std::max(pressMax_, pressure);
    const auto t0 = Clock::now();
    markPreview(engine_.strokeTo(float(x), float(y), float(pressure)));
    paintUs_ += us(t0);
    ++moves_;
    // Ask for a viewer update at most every 'refresh every' ms, and not again until the
    // last one has been answered. Asking again while a render is still on its way makes
    // the host cancel it and start over, and with a fast tablet that can go on for the
    // whole stroke: nothing appears until the pen lifts. The 400 ms cap re-asks in case
    // a request was dropped.
    const auto now = Clock::now();
    const auto since = now - lastPaintRefresh_;
    const bool answered = renders_.load() > rendersAtRefresh_;
    if (since > std::chrono::milliseconds(refreshMsCached_) &&
        (answered || since > std::chrono::milliseconds(400))) {
      lastPaintRefresh_ = now;
      rendersAtRefresh_ = renders_.load();
      refresh();
    }
  }
  void endStroke() {
    if (!engine_.stroking()) return;
    buildPreview();  // the last moves, while the engine still knows what was there before
    strokeEnd_ = Clock::now();
    rendersAtEnd_ = renders_.load();
    const bool painted = engine_.endStroke();
    if (painted) {
      if (strokeCanvas_) strokeCanvas_->dirty = true;
      commitHistory("Paint stroke");
    }
    strokeCanvas_.reset();
    srcImage_.reset();
    srcStack_.clear();
    reportPen();
    reportStroke();
    refresh();
    reportMemory();
  }
  int penMode() const { return cv(penSource_); }
  void setSize(double s) {
    OFX::DoubleParam* p = fetchDoubleParam("size");
    p->setValue(std::max(1.0, std::min(5000.0, s)));
  }

  // ---- toolbar
  int tool() { return cv(tool_); }
  void setTool(int t) { if (cv(tool_) != t) tool_->setValue(t); }
  int brushIndex() { return cv(brush_); }
  // Picking a brush on the toolbar does what picking it in the menu does, settings
  // from the brush file included.
  void pickBrush(int i) {
    if (i < 0 || i > int(abr_.presets.size())) return;
    if (cv(brush_) != i) brush_->setValue(i);
    applyBrushFileSettings();
  }
  std::vector<const rp::BrushTip*> tips() {
    uiParams();  // makes sure the brush file is loaded
    std::vector<const rp::BrushTip*> out;
    for (const rp::BrushPreset& pr : abr_.presets)
      out.push_back(pr.tip >= 0 && size_t(pr.tip) < abr_.tips.size() ? &abr_.tips[size_t(pr.tip)] : nullptr);
    return out;
  }
  bool canUndo() const { return engine_.canUndo(); }
  bool canRedo() const { return engine_.canRedo(); }
  // Undo / redo from the toolbar. Recorded as one host undo step, so Ctrl+Z after
  // pressing the toolbar's undo puts the stroke back, as it would in Nuke's own nodes.
  void stepHistory(int delta) {
    if (engine_.stroking()) return;
    if ((delta < 0 && !engine_.canUndo()) || (delta > 0 && !engine_.canRedo())) return;
    syncHistory(strokeIndex_ + delta);
    beginEditBlock(delta < 0 ? "Undo paint" : "Redo paint");
    history_->setValue(strokeIndex_);
    endEditBlock();
  }

  // Alt+C: the colour under the pointer, as you see it - the input with the paint on
  // top - becomes the brush colour. Held down, it follows the pointer. The input comes
  // from what the host last rendered; failing that the host is asked for the image once
  // per pick, which renders everything upstream and so is slow.
  void pickColor(double x, double y, double time) {
    const int px = int(std::floor(x)), py = int(std::floor(y));
    float acc[4] = {0, 0, 0, 0};
    bool have = false;
    {
      std::lock_guard<std::mutex> g(inputMutex_);
      if (inputTime_ == time) {
        auto it = inputTiles_.find(rp::tileKey(rp::tileOf(px), rp::tileOf(py)));
        const size_t i = size_t(py & rp::TILE_MASK) * rp::TILE + size_t(px & rp::TILE_MASK);
        if (it != inputTiles_.end() && it->second->ok[i]) {
          for (int c = 0; c < 4; ++c) acc[c] = it->second->px.px[i * 4 + c];
          have = true;
        }
      }
    }
    if (!have && src_ && src_->isConnected()) {
      try {
        if (!pickImage_) pickImage_.reset(src_->fetchImage(time));
        if (pickImage_ && pickImage_->getPixelDepth() == OFX::eBitDepthFloat &&
            pickImage_->getPixelComponents() == OFX::ePixelComponentRGBA) {
          const float* p = static_cast<const float*>(pickImage_->getPixelAddress(px, py));
          if (p) for (int c = 0; c < 4; ++c) acc[c] = p[c];
        }
      } catch (...) {
      }
    }
    for (const Shot& sh : stackFor(frameKey(time))) {
      std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
      if (const rp::Tile* t = sh.canvas->find(rp::tileOf(px), rp::tileOf(py))) {
        const float* p = t->px + (size_t(py & rp::TILE_MASK) * rp::TILE + (px & rp::TILE_MASK)) * 4;
        rp::blendPixel(sh.blend, sh.opacity, p, acc);
      }
    }
    double rgb[3];
    for (int c = 0; c < 3; ++c) rgb[c] = acc[3] > 0.0f ? acc[c] / acc[3] : acc[c];
    color_->setValue(rgb[0], rgb[1], rgb[2]);
    // The host can apply the new value a moment later, so reading the parameter back
    // here may still give the old colour. The brush and the swatch take the picked
    // colour directly instead of waiting for it.
    uiParams();
    for (int c = 0; c < 3; ++c) cached_.color[c] = float(rgb[c]);
  }
  void endPick() { pickImage_.reset(); }

  // ---- clone source
  // Ctrl+click: the next clone stroke copies from here.
  void setCloneSource(double x, double y) {
    cloneSrcX_ = x;
    cloneSrcY_ = y;
    cloneSrcPending_ = true;
  }
  bool cloneSourcePending(double& x, double& y) const {
    x = cloneSrcX_;
    y = cloneSrcY_;
    return cloneSrcPending_;
  }
  void cloneOffset(double& dx, double& dy) { cloneOffset_->getValue(dx, dy); }

  // ---- layers, for the toolbar's layer list
  std::vector<rpui::LayerRow> layerRows() {
    std::lock_guard<std::mutex> g(mutex_);
    std::vector<rpui::LayerRow> out;
    for (const Layer& l : layers_) {
      rpui::LayerRow r;
      r.name = l.name;
      r.visible = l.visible;
      r.locked = l.locked;
      out.push_back(r);
    }
    return out;
  }
  int activeLayer() {
    std::lock_guard<std::mutex> g(mutex_);
    return active_;
  }
  void selectLayer(int idx) {
    {
      std::lock_guard<std::mutex> g(mutex_);
      if (layers_.empty()) return;
      active_ = std::max(0, std::min(idx, int(layers_.size()) - 1));
    }
    syncLayerParams();
    writeLayerData();
  }
  void toggleLayerVisible(int idx) {
    {
      std::lock_guard<std::mutex> g(mutex_);
      if (idx < 0 || idx >= int(layers_.size())) return;
      layers_[size_t(idx)].visible = !layers_[size_t(idx)].visible;
      absent_.clear();
    }
    syncLayerParams();
    writeLayerData();
    refresh();
  }

  // ------------------------------------------------------------------ OFX actions

  bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& clip, double& time) override {
    if (cv(output_, args.time) != 0 || !src_ || !src_->isConnected()) return false;
    if (!stackFor(frameKey(args.time)).empty()) return false;
    ++identities_;
    clip = src_;
    time = args.time;
    return true;
  }

  void render(const OFX::RenderArguments& args) override {
    const auto t0 = Clock::now();
    struct Timer {
      PaintEffect* fx;
      Clock::time_point t0;
      ~Timer() {
        if (fx->engine_.stroking() && fx->firstRenderUs_ < 0)
          fx->firstRenderUs_ = std::chrono::duration_cast<std::chrono::microseconds>(t0 - fx->strokeStart_).count();
        fx->renders_ += 1;
        fx->renderUs_ += us(t0);
        fx->lastRenderNs_ =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        allRenders() += 1;
      }
    } timer{this, t0};
    std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
    if (!dst) return;
    if (dst->getPixelDepth() != OFX::eBitDepthFloat || dst->getPixelComponents() != OFX::ePixelComponentRGBA)
      OFX::throwSuiteStatusException(kOfxStatErrFormat);
    const int outMode = cv(output_, args.time);
    const int chans = cv(channels_, args.time);
    std::unique_ptr<OFX::Image> src;
    if (outMode == 0 && src_ && src_->isConnected()) src.reset(src_->fetchImage(args.time));
    OfxRectI sb{0, 0, 0, 0};
    if (src) sb = src->getBounds();

    const OfxRectI w = args.renderWindow;
    const int n = w.x2 - w.x1;
    if (n <= 0) return;
    const double sx = args.renderScale.x, sy = args.renderScale.y;
    const std::vector<Shot> stack = stackFor(frameKey(args.time));
    const bool keepInput = src && sx == 1.0 && sy == 1.0;

    std::vector<float> plane(size_t(n) * 4), in(size_t(n) * 4);
    float* acc[4] = {plane.data(), plane.data() + n, plane.data() + 2 * n, plane.data() + 3 * n};
    for (int y = w.y1; y < w.y2; ++y) {
      if (abort()) return;
      // The input row, interleaved, zero outside the input's bounds.
      std::fill(in.begin(), in.end(), 0.0f);
      if (src && y >= sb.y1 && y < sb.y2) {
        const int a = std::max(w.x1, sb.x1), b = std::min(w.x2, sb.x2);
        if (b > a) {
          const float* s = static_cast<const float*>(src->getPixelAddress(a, y));
          if (s) std::memcpy(&in[size_t(a - w.x1) * 4], s, size_t(b - a) * 4 * sizeof(float));
        }
      }
      if (keepInput) keepInputRow(args.time, y, w.x1, w.x2, in.data());
      float* d = static_cast<float*>(dst->getPixelAddress(w.x1, y));
      if (!d) continue;
      bool painted = false;
      for (const Shot& sh : stack) {
        // Locked per row rather than for the whole render, so a stroke in progress is
        // never held up for longer than one row of a render.
        std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
        if (!sh.canvas->rowHasPaintScaled(y, w.x1, w.x2, sx, sy)) continue;
        if (!painted) {
          for (int i = 0; i < n; ++i)
            for (int c = 0; c < 4; ++c) acc[c][i] = in[size_t(i) * 4 + c];
          painted = true;
        }
        sh.canvas->blendRowScaled(y, w.x1, w.x2, acc, sh.opacity, sh.blend, sx, sy);
      }
      if (!painted) {  // rows no layer touches are a straight copy
        std::memcpy(d, in.data(), in.size() * sizeof(float));
        continue;
      }
      for (int i = 0; i < n; ++i)
        for (int c = 0; c < 4; ++c) {
          // Channels the user excluded pass the input through untouched. Paint only is
          // always full RGBA, as in the NDK node.
          const bool use = outMode == 1 || chans == 0 || (chans == 1 ? c < 3 : c == 3);
          d[size_t(i) * 4 + c] = use ? acc[c][i] : in[size_t(i) * 4 + c];
        }
    }
  }

  // The host is about to save the script: write the paint.
  void syncPrivateData() override { savePaint(false); }

  void changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) override {
    if (name != "paint_rev" && name != "history") paramsDirty_ = true;
    const bool byPlugin = args.reason == OFX::eChangePluginEdit;
    if (name == "history") {
      if (!byPlugin) syncHistory(history_->getValue());
      return;
    }
    if (byPlugin) return;  // our own updates to menus, statuses and settings
    if (name == "brush_set") {
      const int sel = cv(brushSet_);
      setName_->setValue(sel > 0 && size_t(sel - 1) < sets_.size() ? sets_[size_t(sel - 1)] : "");
      loadBrushes(true);
      brush_->setValue(0);
    } else if (name == "abr_file") {
      // A file picked directly takes over from an installed set.
      setName_->setValue("");
      brushSet_->setValue(0);
      loadBrushes(true);
      brush_->setValue(0);
    } else if (name == "brush") {
      applyBrushFileSettings();
    } else if (name == "brush_install") {
      std::string err;
      const std::string n = npaint::BrushLibrary::install(sv(abrFile_), &err);
      if (n.empty()) { sendMessage(OFX::Message::eMessageError, "", "NPaintShop: " + err); return; }
      refreshBrushSets();
      setName_->setValue(n);
      selectSet(n);
      loadBrushes(true);
    } else if (name == "brush_remove") {
      std::string err;
      const std::string n = sv(setName_);
      if (n.empty()) { sendMessage(OFX::Message::eMessageError, "", "NPaintShop: pick a brush set to remove first."); return; }
      if (!npaint::BrushLibrary::remove(n, &err)) { sendMessage(OFX::Message::eMessageError, "", "NPaintShop: " + err); return; }
      setName_->setValue("");
      refreshBrushSets();
      loadBrushes(true);
    } else if (name == "preset") {
      applyPreset(cv(preset_));
    } else if (name == "preset_save") {
      savePreset();
    } else if (name == "preset_delete") {
      deletePreset();
    } else if (name == "brush_reset") {
      for (const PDef& d : kBrush) setNum(d.name, d.def);
      preset_->setValue(0);
    } else if (name == "clear_paint") {
      clearFrame(args.time);
    } else if (name == "save_paint") {
      savePaint(true);
    } else if (name == "storage" || name == "paint_dir") {
      storageChanged();
    } else if (name == "paint_data") {
      readStorageSettings();  // an embedded script pasted over this one
      reloadAll();
    } else if (name == "layer_data") {
      {
        std::lock_guard<std::mutex> g(mutex_);
        deserializeLocked(sv(layerData_));
      }
      syncLayerParams();
      refresh();
    } else if (name == "paint_id") {
      claimId();
    } else if (name == "undo_memory" || name == "clone_cache" || name == "frame_memory") {
      applyMemoryLimits();
      reportMemory();
    } else if (name == "undo_paint") {
      stepHistory(-1);
    } else if (name == "redo_paint") {
      stepHistory(+1);
    } else if (name == "reload_paint") {
      reloadAll();
      refresh();
      reportMemory();
    } else if (name == "free_memory") {
      freeMemory(args.time);
    } else if (name == "lifetime" || name == "output" || name == "paint_channels") {
      refresh();
    } else if (name == "layer") {
      selectLayer(cv(layer_));
    } else if (name == "layer_name" || name == "layer_opacity" || name == "layer_blend" ||
               name == "layer_visible" || name == "layer_locked") {
      pullLayerParams();
    } else if (name.rfind("layer_", 0) == 0) {
      layerOp(name);
    }
  }

 private:
  // ------------------------------------------------------------------ parameters


  double getNum(const char* name) {
    const PDef* d = findDef(name);
    if (!d) return 0;
    switch (d->type) {
      case kDouble: return fetchDoubleParam(name)->getValue();
      case kInt: return fetchIntParam(name)->getValue();
      case kBool: return fetchBooleanParam(name)->getValue() ? 1 : 0;
      case kChoice: return cv(fetchChoiceParam(name));
    }
    return 0;
  }
  void setNum(const char* name, double v) {
    const PDef* d = findDef(name);
    if (!d) return;
    switch (d->type) {
      case kDouble: fetchDoubleParam(name)->setValue(v); break;
      case kInt: fetchIntParam(name)->setValue(int(std::lround(v))); break;
      case kBool: fetchBooleanParam(name)->setValue(v != 0); break;
      case kChoice: fetchChoiceParam(name)->setValue(int(std::lround(v))); break;
    }
  }

  rp::BrushParams currentParams() {
    rp::BrushParams p;
    p.tool = rp::Tool(std::max(0, std::min(cv(tool_), 3)));
    p.smudgeStrength = float(smudgeStrength_->getValue());
    p.smudgeRate = float(smudgeRate_->getValue());
    double r, g, b;
    color_->getValue(r, g, b);
    p.color[0] = float(r); p.color[1] = float(g); p.color[2] = float(b);
    bgColor_->getValue(r, g, b);
    p.bgColor[0] = float(r); p.bgColor[1] = float(g); p.bgColor[2] = float(b);
    auto f = [&](const char* n) { return float(getNum(n)); };
    auto i = [&](const char* n) { return int(std::lround(getNum(n))); };
    auto on = [&](const char* n) { return getNum(n) != 0; };
    p.size = f("size"); p.hardness = f("hardness"); p.opacity = f("opacity"); p.flow = f("flow");
    p.spacing = std::max(0.01f, f("spacing"));
    p.blend = std::max(0, std::min(i("brush_blend"), int(rp::kBlendCount) - 1));
    p.smoothing = f("smoothing");
    p.pressureSize = on("pressure_size");
    p.pressureFlow = on("pressure_flow");
    const int sel = cv(brush_);
    if (sel > 0 && size_t(sel - 1) < abr_.presets.size()) {
      const rp::BrushPreset& pr = abr_.presets[size_t(sel - 1)];
      if (pr.tip >= 0 && size_t(pr.tip) < abr_.tips.size()) p.tip = &abr_.tips[size_t(pr.tip)];
    }
    rp::BrushDynamics& d = p.dyn;
    auto ctl = [&](rp::DynControl& c, const char* j, const char* k, const char* fd, const char* mn) {
      c.jitter = f(j); c.control = i(k); c.fadeSteps = std::max(1, i(fd));
      if (mn) c.minimum = f(mn);
    };
    d.angle = f("dyn_angle"); d.roundness = f("dyn_roundness");
    d.flipX = on("dyn_flip_x"); d.flipY = on("dyn_flip_y");
    ctl(d.size, "size_jitter", "size_control", "size_fade", "size_minimum");
    d.angleJitter = f("angle_jitter"); d.angleControl = i("angle_control");
    d.angleFadeSteps = std::max(1, i("angle_fade"));
    ctl(d.round, "round_jitter", "round_control", "round_fade", "round_minimum");
    d.flipXJitter = on("flip_x_jitter"); d.flipYJitter = on("flip_y_jitter");
    d.scatter = f("scatter"); d.scatterBoth = on("scatter_both");
    d.count = std::max(1, i("scatter_count")); d.countJitter = f("count_jitter");
    ctl(d.opacity, "opacity_jitter", "opacity_control", "opacity_fade", "opacity_minimum");
    ctl(d.flow, "flow_jitter", "flow_control", "flow_fade", "flow_minimum");
    d.noise = on("noise"); d.wetEdges = on("wet_edges");
    ctl(d.fgbg, "fgbg_jitter", "fgbg_control", "fgbg_fade", "fgbg_minimum");
    d.hueJitter = f("hue_jitter"); d.satJitter = f("sat_jitter"); d.briJitter = f("bri_jitter");
    d.purity = f("purity");
    d.colorPerTip = on("color_per_tip");
    const int seed = i("dyn_seed");
    d.seed = unsigned(seed > 0 ? seed : 1);
    return p;
  }


  void markPreview(const rp::IRect& d) {
    if (d.empty()) return;
    for (int ty = rp::tileOf(d.y0); ty <= rp::tileOf(d.y1 - 1); ++ty)
      for (int tx = rp::tileOf(d.x0); tx <= rp::tileOf(d.x1 - 1); ++tx) previewDirty_.insert(rp::tileKey(tx, ty));
  }

  // Linear to display, in a table: the overlay is drawn after the viewer's own colour
  // transform, so it applies one itself. 'preview colour' picks which, to match the
  // viewer; viewer gain and gamma are not followed.
  static unsigned char toDisplay(float v, int mode) {
    static unsigned char table[4][4097];
    static bool init = false;
    if (!init) {
      for (int i = 0; i <= 4096; ++i) {
        const double l = i / 4096.0;
        const double srgb = l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
        const double r709 = l < 0.018 ? 4.5 * l : 1.099 * std::pow(l, 0.45) - 0.099;
        const double g22 = std::pow(l, 1.0 / 2.2);
        const double vals[4] = {srgb, r709, g22, l};
        for (int m = 0; m < 4; ++m)
          table[m][i] = (unsigned char)std::lround(std::min(1.0, std::max(0.0, vals[m])) * 255.0);
      }
      init = true;
    }
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return table[mode & 3][int(v * 4096.0f)];
  }

  // Builds the preview for the tiles the stroke has changed since the last draw.
  //
  // Where the input under a tile is known (kept from the host's last render), the
  // preview is the finished picture - input, every visible layer, the channel rule -
  // drawn opaque over the stroke's pixels. It then looks the same whatever the viewer
  // currently shows, so handing over to the host's render cannot flash. Elsewhere it
  // falls back to the difference the stroke made, laid over the old image.
  void buildPreview() {
    if (!strokeCanvas_ || previewDirty_.empty()) return;
    const std::vector<Shot> stack = stackFor(strokeKey_);
    std::vector<std::shared_lock<std::shared_mutex>> locks;
    bool strokeInStack = false;
    for (const Shot& sh : stack) {
      locks.emplace_back(sh.canvas->mutex);
      strokeInStack = strokeInStack || sh.canvas == strokeCanvas_;
    }
    if (!strokeInStack) locks.emplace_back(strokeCanvas_->mutex);
    const int mode = previewColour_;
    const int outMode = previewOutput_, chans = previewChannels_;
    for (uint64_t key : previewDirty_) {
      const rp::Tile* before = nullptr;
      if (!engine_.strokeBefore(key, &before)) continue;
      const int tx = rp::keyTx(key), ty = rp::keyTy(key);
      const rp::Tile* cur = strokeCanvas_->find(tx, ty);
      std::shared_ptr<const rp::SrcTile> in = outMode == 0 ? inputTile(tx, ty) : nullptr;
      const bool full = in || outMode == 1 || !src_->isConnected();
      std::vector<const rp::Tile*> layerTiles;
      if (full)
        for (const Shot& sh : stack) layerTiles.push_back(sh.canvas->find(tx, ty));
      PreviewTile t;
      t.key = key;
      t.rgba.assign(size_t(rp::TILE_PIXELS) * 4, 0);
      static const float zero[4] = {0, 0, 0, 0};
      for (int i = 0; i < rp::TILE_PIXELS; ++i) {
        const float* B = before ? before->px + size_t(i) * 4 : zero;
        const float* C = cur ? cur->px + size_t(i) * 4 : zero;
        if (B[0] == C[0] && B[1] == C[1] && B[2] == C[2] && B[3] == C[3]) continue;  // untouched
        unsigned char* o = &t.rgba[size_t(i) * 4];
        // Per pixel: an input tile can be partly rendered (the edge of the viewer).
        if (full && (!in || in->ok[i])) {
          float base[4] = {0, 0, 0, 0};
          if (in) for (int c = 0; c < 4; ++c) base[c] = in->px.px[size_t(i) * 4 + c];
          float acc[4] = {base[0], base[1], base[2], base[3]};
          for (size_t k = 0; k < stack.size(); ++k)
            if (layerTiles[k]) rp::blendPixel(stack[k].blend, stack[k].opacity, layerTiles[k]->px + size_t(i) * 4, acc);
          for (int c = 0; c < 3; ++c) {
            const bool use = outMode == 1 || chans == 0 || chans == 1;
            o[c] = toDisplay(use ? acc[c] : base[c], mode);
          }
          o[3] = 255;
          continue;
        }
        // X such that X over B = C: what to lay over the old image to get the new one.
        // Only paint that adds coverage can be shown this way.
        float x[4];
        if (B[3] >= 0.999f) {
          for (int c = 0; c < 4; ++c) x[c] = C[c];
        } else {
          const float xa = std::min(1.0f, (C[3] - B[3]) / (1.0f - B[3]));
          if (xa <= 0.0f) continue;
          for (int c = 0; c < 3; ++c) x[c] = std::max(0.0f, C[c] - (1.0f - xa) * B[c]);
          x[3] = xa;
        }
        const float a = std::min(1.0f, x[3] * previewOpacity_);
        if (a <= 0.0f) continue;
        for (int c = 0; c < 3; ++c) o[c] = (unsigned char)std::lround(toDisplay(x[c] / x[3], mode) * a);
        o[3] = (unsigned char)std::lround(a * 255.0f);
      }
      previewReady_.push_back(std::move(t));
      ++previewBuilt_;
    }
    previewDirty_.clear();
  }

  // ---- the input, as the host last rendered it
  // Kept so the preview and the eyedropper can show the plate without asking the host
  // for the whole image again, which means rendering everything upstream. Filled by
  // render() at full resolution only, for one frame at a time, within a memory budget.
  void keepInputRow(double time, int y, int x0, int x1, const float* rgba) {
    std::lock_guard<std::mutex> g(inputMutex_);
    if (time != inputTime_) {
      inputTiles_.clear();
      inputTime_ = time;
    }
    const int ty = rp::tileOf(y);
    for (int tx = rp::tileOf(x0); tx <= rp::tileOf(x1 - 1); ++tx) {
      auto& slot = inputTiles_[rp::tileKey(tx, ty)];
      if (!slot) {
        if ((inputTiles_.size()) * sizeof(rp::SrcTile) > inputBudget_.load()) {
          inputTiles_.erase(rp::tileKey(tx, ty));
          return;  // over budget: the preview falls back for the rest
        }
        slot = std::make_shared<rp::SrcTile>();
      }
      const int a = std::max(x0, tx * rp::TILE), b = std::min(x1, (tx + 1) * rp::TILE);
      const size_t row = size_t(y & rp::TILE_MASK) * rp::TILE;
      std::memcpy(slot->px.px + (row + size_t(a & rp::TILE_MASK)) * 4, rgba + size_t(a - x0) * 4,
                  size_t(b - a) * 4 * sizeof(float));
      std::memset(slot->ok + row + size_t(a & rp::TILE_MASK), 1, size_t(b - a));
    }
  }
  // A tile of kept input, or null. Only trusted when every pixel of it was rendered.
  std::shared_ptr<const rp::SrcTile> inputTile(int tx, int ty) {
    std::lock_guard<std::mutex> g(inputMutex_);
    if (inputTime_ != strokeTime_) return nullptr;
    auto it = inputTiles_.find(rp::tileKey(tx, ty));
    if (it == inputTiles_.end()) return nullptr;
    return it->second;
  }

  void applyMemoryLimits() {
    engine_.setUndoBudget(size_t(std::max(64, undoMB_->getValue())) << 20);
    inputBudget_ = size_t(std::max(128, cloneCacheMB_->getValue())) << 20;
    std::lock_guard<std::mutex> g(mutex_);
    frameBudget_ = size_t(std::max(0, frameMB_->getValue())) << 20;
  }

  // Empties the clone cache and the undo history, saves everything, and lets go of
  // every frame but the current one. Nothing painted is lost.
  void freeMemory(double time) {
    if (engine_.stroking()) return;
    {
      std::lock_guard<std::mutex> g(inputMutex_);
      inputTiles_.clear();
    }
    srcStack_.clear();
    engine_.clearHistory();
    savePaint(false);
    {
      std::lock_guard<std::mutex> g(mutex_);
      const int keep = frameKey(time);
      for (Layer& l : layers_)
        for (auto it = l.canvases.begin(); it != l.canvases.end();) {
          if (it->first != keep && it->second.use_count() == 1 && !it->second->dirty) it = l.canvases.erase(it);
          else ++it;
        }
    }
    reportMemory();
  }

  int frameKey(double time) {
    return cv(lifetime_, time) == 0 ? kAllFrames : int(std::lround(time));
  }

  // Tells the host the image changed. OFX has no "this region is dirty" call, so a
  // hidden counter that cannot be undone is moved on, which invalidates the cache.
  void refresh() {
    ++refreshes_;
    rev_->setValue(rev_->getValue() + 1);
  }

  void setStatus(OFX::StringParam* p, const std::string& text) {
    if (sv(p) != text) p->setValue(text);
  }

  // ------------------------------------------------------------------ clone source

  // What clone and smudge read from: the input with every other visible layer on top,
  // frozen for the stroke. The engine adds this layer's own pre-stroke pixels, which is
  // what stops a clone stroke smearing its own output. OFX hands out the input image
  // whole, so it is fetched once per stroke and cut into tiles as the brush needs them.
  rp::BgFetch sourceFetch(double time, int skipId, bool clone) {
    srcImage_.reset();
    // Clone can read from B; smudge always works on what is under the brush (A).
    OFX::Clip* from = src_;
    cloneFromB_ = false;
    if (clone && cv(cloneInput_) == 1 && srcB_ && srcB_->isConnected()) {
      from = srcB_;
      cloneFromB_ = true;
    }
    try {
      if (from && from->isConnected()) {
        const double t = time + (clone ? cloneTime_->getValue() : 0.0);
        srcImage_.reset(from->fetchImage(t));
        if (srcImage_ && (srcImage_->getPixelDepth() != OFX::eBitDepthFloat ||
                          srcImage_->getPixelComponents() != OFX::ePixelComponentRGBA))
          srcImage_.reset();
      }
    } catch (...) {
      srcImage_.reset();
    }
    cloneHadPlate_ = bool(srcImage_);
    if (!srcImage_ && from && from->isConnected() && !warnedNoSource_) {
      warnedNoSource_ = true;
      sendMessage(OFX::Message::eMessageWarning, "",
                  "NPaintShop: the host did not hand over the input image while painting, so "
                  "clone and smudge can only pick up paint, not the plate.");
    }
    srcStack_.clear();
    if (!cloneFromB_) srcStack_ = stackFor(frameKey(time), skipId);  // B is cloned as it is
    OFX::Image* img = srcImage_.get();
    std::vector<Shot>* stack = &srcStack_;
    return [img, stack](int tx, int ty) -> std::shared_ptr<const rp::SrcTile> {
      auto t = std::make_shared<rp::SrcTile>();
      const int x0 = tx * rp::TILE, y0 = ty * rp::TILE;
      OfxRectI b{0, 0, 0, 0};
      if (img) b = img->getBounds();
      for (int j = 0; j < rp::TILE; ++j) {
        const int y = y0 + j;
        for (int i = 0; i < rp::TILE; ++i) {
          const int x = x0 + i;
          float* o = t->px.px + (size_t(j) * rp::TILE + i) * 4;
          if (img) {
            if (x < b.x1 || x >= b.x2 || y < b.y1 || y >= b.y2) continue;  // outside: no source
            const float* p = static_cast<const float*>(img->getPixelAddress(x, y));
            if (!p) continue;
            for (int c = 0; c < 4; ++c) o[c] = p[c];
          }
          t->ok[size_t(j) * rp::TILE + i] = 1;
        }
      }
      for (const Shot& sh : *stack) {
        std::shared_lock<std::shared_mutex> lock(sh.canvas->mutex);
        const rp::Tile* lt = sh.canvas->find(tx, ty);
        if (!lt) continue;
        for (int k = 0; k < rp::TILE_PIXELS; ++k)
          if (t->ok[k]) rp::blendPixel(sh.blend, sh.opacity, lt->px + size_t(k) * 4, t->px.px + size_t(k) * 4);
      }
      return t;
    };
  }

  // ------------------------------------------------------------------ undo

  // One undoable step per stroke, the NDK way: a counter parameter moves on by one as a
  // single host undo step. Ctrl+Z makes the host put the counter back, and syncHistory
  // steps the paint engine's own history to match.
  void commitHistory(const char* what) {
    ++strokeIndex_;
    beginEditBlock(what);
    history_->setValue(strokeIndex_);
    endEditBlock();
  }
  void syncHistory(int target) {
    if (target == strokeIndex_ || engine_.stroking()) return;
    std::shared_ptr<rp::Canvas> which;
    while (strokeIndex_ > target && engine_.undo(nullptr, &which)) {
      --strokeIndex_;
      if (which) which->dirty = true;
    }
    while (strokeIndex_ < target && engine_.redo(nullptr, &which)) {
      ++strokeIndex_;
      if (which) which->dirty = true;
    }
    strokeIndex_ = target;
    syncLayerParams();
    writeLayerData();
    refresh();
    reportMemory();
  }

  void clearFrame(double time) {
    if (engine_.stroking()) return;
    std::shared_ptr<rp::Canvas> c;
    {
      std::lock_guard<std::mutex> g(mutex_);
      c = canvasLocked(activeLocked(), frameKey(time), true);
    }
    engine_.clear(c);
    c->dirty = true;
    commitHistory("Clear paint");
    refresh();
    reportMemory();
  }

  // ------------------------------------------------------------------ layers

  Layer& activeLocked() {
    if (layers_.empty()) addLayerLocked("Paint 1");
    if (active_ < 0 || active_ >= int(layers_.size())) active_ = int(layers_.size()) - 1;
    return layers_[size_t(active_)];
  }
  Layer& addLayerLocked(const std::string& name) {
    Layer l;
    l.id = nextId_++;
    l.name = name.empty() ? "Paint " + std::to_string(l.id) : name;
    layers_.push_back(std::move(l));
    return layers_.back();
  }

  // Layer metadata travels in the script (layer_data); the pixels go wherever 'store
  // paint' says. Same format as the NDK node.
  std::string serializeLocked() const {
    std::string o = "v1;" + std::to_string(nextId_) + ";" + std::to_string(active_);
    for (const Layer& l : layers_) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "|%d,%.6g,%d,%d,%d,", l.id, l.opacity, l.blend, l.visible ? 1 : 0,
                    l.locked ? 1 : 0);
      o += buf;
      o += esc(l.name);
    }
    return o;
  }
  void deserializeLocked(const std::string& s) {
    if (s.rfind("v1;", 0) != 0) return;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= s.size()) {
      const size_t q = s.find('|', p);
      parts.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
      if (q == std::string::npos) break;
      p = q + 1;
    }
    int n = 1, a = 0;
    if (parts.empty() || std::sscanf(parts[0].c_str(), "v1;%d;%d", &n, &a) != 2) return;
    std::vector<Layer> loaded;
    for (size_t i = 1; i < parts.size(); ++i) {
      Layer l;
      int vis = 1, lk = 0;
      char nameBuf[512] = {0};
      if (std::sscanf(parts[i].c_str(), "%d,%f,%d,%d,%d,%511[^\n]", &l.id, &l.opacity, &l.blend, &vis, &lk,
                      nameBuf) < 5)
        continue;
      l.visible = vis != 0;
      l.locked = lk != 0;
      l.blend = std::max(0, std::min(l.blend, int(rp::kBlendCount) - 1));
      l.name = unesc(nameBuf);
      if (l.name.empty()) l.name = "Paint " + std::to_string(l.id);
      for (Layer& old : layers_)  // keep pixels already loaded for a layer with this id
        if (old.id == l.id) { l.canvases = std::move(old.canvases); break; }
      loaded.push_back(std::move(l));
    }
    if (loaded.empty()) return;
    layers_ = std::move(loaded);
    nextId_ = std::max(n, 1);
    for (const Layer& l : layers_) nextId_ = std::max(nextId_, l.id + 1);
    active_ = std::max(0, std::min(a, int(layers_.size()) - 1));
    absent_.clear();
  }
  void writeLayerData() {
    std::string data;
    {
      std::lock_guard<std::mutex> g(mutex_);
      data = serializeLocked();
    }
    if (sv(layerData_) != data) layerData_->setValue(data);
  }

  // Pushes the stack into the parameters: the layer menu and the active layer's fields.
  void syncLayerParams() {
    std::vector<std::string> names;
    int active;
    Layer cur;
    {
      std::lock_guard<std::mutex> g(mutex_);
      Layer& a = activeLocked();
      active = active_;
      cur.name = a.name; cur.opacity = a.opacity; cur.blend = a.blend;
      cur.visible = a.visible; cur.locked = a.locked;
      for (const Layer& l : layers_) {
        std::string n = l.name;
        if (!l.visible) n += "  (hidden)";
        else if (l.locked) n += "  (locked)";
        names.push_back(n);
      }
    }
    if (names != layerMenu_) {
      layer_->resetOptions();
      for (const std::string& n : names) layer_->appendOption(n);
      layerMenu_ = names;
    }
    if (cv(layer_) != active) layer_->setValue(active);
    if (sv(layerName_) != cur.name) layerName_->setValue(cur.name);
    if (std::fabs(layerOpacity_->getValue() - cur.opacity) > 1e-6) layerOpacity_->setValue(cur.opacity);
    if (cv(layerBlend_) != cur.blend) layerBlend_->setValue(cur.blend);
    if (layerVisible_->getValue() != cur.visible) layerVisible_->setValue(cur.visible);
    if (layerLocked_->getValue() != cur.locked) layerLocked_->setValue(cur.locked);
  }

  // The user edited the active layer's fields.
  void pullLayerParams() {
    {
      std::lock_guard<std::mutex> g(mutex_);
      Layer& l = activeLocked();
      const std::string n = sv(layerName_);
      if (!n.empty()) l.name = n;
      l.opacity = float(layerOpacity_->getValue());
      l.blend = std::max(0, std::min(cv(layerBlend_), int(rp::kBlendCount) - 1));
      l.visible = layerVisible_->getValue();
      l.locked = layerLocked_->getValue();
      absent_.clear();
    }
    syncLayerParams();
    writeLayerData();
    refresh();
  }

  static std::shared_ptr<rp::Canvas> copyCanvas(const rp::Canvas& src) {
    auto c = std::make_shared<rp::Canvas>();
    std::shared_lock<std::shared_mutex> lock(src.mutex);
    for (const auto& kv : src.tiles) c->tiles[kv.first].reset(new rp::Tile(*kv.second));
    c->dirty = true;
    return c;
  }

  // Applies a change to the layer stack and records it, so Ctrl+Z walks back through it
  // alongside the strokes. Copying the stack is cheap: the canvases are shared pointers.
  void structuralChange(const char* what, const std::function<void()>& apply) {
    if (engine_.stroking()) return;
    auto before = std::make_shared<std::vector<Layer>>();
    auto after = std::make_shared<std::vector<Layer>>();
    int beforeActive, beforeNext, afterActive, afterNext;
    {
      std::lock_guard<std::mutex> g(mutex_);
      *before = layers_; beforeActive = active_; beforeNext = nextId_;
      apply();
      for (Layer& l : layers_)
        for (auto& kv : l.canvases) kv.second->dirty = true;
      *after = layers_; afterActive = active_; afterNext = nextId_;
      absent_.clear();
    }
    auto atAfter = std::make_shared<bool>(true);
    PaintEffect* self = this;
    engine_.pushStructural([self, before, after, atAfter, beforeActive, beforeNext, afterActive, afterNext]() {
      std::lock_guard<std::mutex> g(self->mutex_);
      const bool goBack = *atAfter;
      // Canvases held in memory now are the newest; keep them when swapping back.
      self->layers_ = goBack ? *before : *after;
      self->active_ = goBack ? beforeActive : afterActive;
      self->nextId_ = goBack ? beforeNext : afterNext;
      for (Layer& l : self->layers_)
        for (auto& kv : l.canvases) kv.second->dirty = true;
      self->absent_.clear();
      *atAfter = !goBack;
    });
    commitHistory(what);
    syncLayerParams();
    writeLayerData();
    refresh();
  }

  void layerOp(const std::string& name) {
    int count, active;
    {
      std::lock_guard<std::mutex> g(mutex_);
      count = int(layers_.size());
      active = active_;
    }
    if (name == "layer_add") {
      structuralChange("Add layer", [this]() {
        Layer l;
        l.id = nextId_++;
        l.name = "Paint " + std::to_string(l.id);
        layers_.insert(layers_.begin() + active_ + 1, std::move(l));
        ++active_;
      });
    } else if (name == "layer_duplicate") {
      structuralChange("Duplicate layer", [this]() {
        loadAllFramesLocked(layers_[size_t(active_)]);
        Layer l = layers_[size_t(active_)];
        l.id = nextId_++;
        l.name += " copy";
        for (auto& kv : l.canvases) kv.second = copyCanvas(*kv.second);
        layers_.insert(layers_.begin() + active_ + 1, std::move(l));
        ++active_;
      });
    } else if (name == "layer_delete") {
      if (count <= 1) {
        sendMessage(OFX::Message::eMessageWarning, "",
                    "NPaintShop: a node always keeps at least one layer. Use 'Clear frame' to empty it.");
        return;
      }
      structuralChange("Delete layer", [this]() {
        loadAllFramesLocked(layers_[size_t(active_)]);  // so undo can bring it all back
        layers_.erase(layers_.begin() + active_);
        if (active_ >= int(layers_.size())) active_ = int(layers_.size()) - 1;
      });
    } else if (name == "layer_up" || name == "layer_down") {
      const int to = active + (name == "layer_up" ? 1 : -1);
      if (to < 0 || to >= count) return;
      structuralChange("Move layer", [this, to]() {
        std::swap(layers_[size_t(active_)], layers_[size_t(to)]);
        active_ = to;
      });
    } else if (name == "layer_merge") {
      if (active <= 0) {
        sendMessage(OFX::Message::eMessageWarning, "", "NPaintShop: the bottom layer has nothing to merge down into.");
        return;
      }
      mergeLayers(active, active - 1, false);
    } else if (name == "layer_flatten") {
      mergeLayers(-1, 0, true);
    }
  }

  // Merges `from` into `into` across every frame they hold paint on. With all set,
  // every visible layer is merged down into the bottom one.
  void mergeLayers(int from, int into, bool all) {
    structuralChange(all ? "Flatten layers" : "Merge layer down", [this, from, into, all]() {
      struct Job { int src, dst; };
      std::vector<Job> jobs;
      if (all) {
        for (int i = int(layers_.size()) - 1; i > 0; --i)
          if (layers_[size_t(i)].contributes()) jobs.push_back(Job{i, 0});
      } else if (from > 0 && into >= 0) {
        jobs.push_back(Job{from, into});
      }
      for (const Job& j : jobs) {
        Layer& src = layers_[size_t(j.src)];
        Layer& dst = layers_[size_t(j.dst)];
        loadAllFramesLocked(src);
        loadAllFramesLocked(dst);
        for (auto& kv : src.canvases) {
          if (kv.second->empty()) continue;
          auto it = dst.canvases.find(kv.first);
          // The destination is replaced rather than edited in place, so the undo
          // snapshot keeps pointing at the original pixels.
          auto merged = it != dst.canvases.end() ? copyCanvas(*it->second) : std::make_shared<rp::Canvas>();
          std::shared_lock<std::shared_mutex> sl(kv.second->mutex);
          kv.second->mergeInto(*merged, src.opacity, src.blend);
          merged->dirty = true;
          dst.canvases[kv.first] = merged;
        }
      }
      std::vector<int> drop;
      for (const Job& j : jobs) drop.push_back(j.src);
      std::sort(drop.begin(), drop.end(), std::greater<int>());
      for (int i : drop) layers_.erase(layers_.begin() + i);
      active_ = std::max(0, std::min(all ? 0 : into, int(layers_.size()) - 1));
    });
  }

  // ------------------------------------------------------------------ paint storage

  // Settings the render threads need, copied out of the parameters on the UI thread.
  void readStorageSettings() {
    std::lock_guard<std::mutex> g(mutex_);
    embedded_ = cv(storage_) == 1;
    userDir_ = sv(paintDir_);
    blobs_.clear();
    absent_.clear();
    parseBlobs(sv(paintData_), blobs_, layers_.empty() ? 1 : layers_.front().id);
  }

  // paint_data: "L<layer>:<frame>:<base64>;..." where frame is a number or "all".
  // The first OFX beta wrote "<frame>:<base64>" with no layer; that lands in the bottom layer.
  static void parseBlobs(const std::string& s, std::map<uint64_t, std::string>& out, int firstId) {
    size_t p = 0;
    while (p < s.size()) {
      size_t q = s.find(';', p);
      if (q == std::string::npos) q = s.size();
      const std::string item = s.substr(p, q - p);
      p = q + 1;
      int id = firstId;
      size_t start = 0;
      if (!item.empty() && item[0] == 'L') {
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        id = std::atoi(item.substr(1, c1 - 1).c_str());
        start = c1 + 1;
      }
      const size_t colon = item.find(':', start);
      if (colon == std::string::npos) continue;
      const std::string k = item.substr(start, colon - start);
      const int key = k == "all" ? kAllFrames : std::atoi(k.c_str());
      out[ck(id, key)] = item.substr(colon + 1);
    }
  }
  static std::string joinBlobs(const std::map<uint64_t, std::string>& in) {
    std::string o;
    for (const auto& kv : in) {
      if (!o.empty()) o += ';';
      const int key = ckKey(kv.first);
      o += "L" + std::to_string(ckId(kv.first)) + ":";
      o += key == kAllFrames ? std::string("all") : std::to_string(key);
      o += ':';
      o += kv.second;
    }
    return o;
  }

  // Called with mutex_ held.
  std::string dirLocked() const {
    const std::string root = userDir_.empty() ? defaultPaintRoot() : userDir_;
    return (fs::path(root) / id_).generic_string();
  }
  // Same names as the NDK node's files.
  static std::string fileFor(const std::string& dir, int id, int key) {
    char b[64];
    if (key == kAllFrames) std::snprintf(b, sizeof(b), "paint.L%02d.rpt", id);
    else std::snprintf(b, sizeof(b), "paint.L%02d.%04d.rpt", id, key);
    return (fs::path(dir) / b).generic_string();
  }
  static std::string legacyFileFor(const std::string& dir, int key) {
    char b[48];
    if (key == kAllFrames) std::snprintf(b, sizeof(b), "paint.rpt");
    else std::snprintf(b, sizeof(b), "paint.%04d.rpt", key);
    return (fs::path(dir) / b).generic_string();
  }

  // Loads one layer's frame from wherever this node keeps its paint. mutex_ held.
  void loadLocked(const Layer& l, int key, rp::Canvas& c) {
    if (embedded_) {
      auto it = blobs_.find(ck(l.id, key));
      if (it != blobs_.end()) canvasFromBytes(c, b64decode(it->second));
    } else if (!id_.empty()) {
      const std::string dir = dirLocked();
      if (!c.load(fileFor(dir, l.id, key)) && !layers_.empty() && &l == &layers_.front())
        c.load(legacyFileFor(dir, key));
    }
    c.dirty = false;
  }

  // One layer's canvas for one frame. With `create` false, returns nullptr when the
  // frame has no paint anywhere, so renders of unpainted frames allocate nothing.
  std::shared_ptr<rp::Canvas> canvasLocked(Layer& l, int key, bool create) {
    auto it = l.canvases.find(key);
    if (it != l.canvases.end()) return it->second;
    const uint64_t k = ck(l.id, key);
    if (!create && absent_.count(k)) return nullptr;
    auto c = std::make_shared<rp::Canvas>();
    loadLocked(l, key, *c);
    if (c->empty() && !create) {
      absent_.insert(k);
      return nullptr;
    }
    absent_.erase(k);
    l.canvases[key] = c;
    evictLocked(key, create);  // `create` comes from the interface thread only
    return c;
  }

  // Every visible layer's paint for one frame, bottom to top.
  std::vector<Shot> stackFor(int key, int skipId = -1) {
    std::vector<Shot> out;
    std::lock_guard<std::mutex> g(mutex_);
    for (Layer& l : layers_) {
      if (!l.contributes() || l.id == skipId) continue;
      auto c = canvasLocked(l, key, false);
      if (c && !c->empty()) out.push_back(Shot{c, l.opacity, l.blend});
    }
    return out;
  }

  // Keeps memory bounded in single-frame mode: frames that are saved, idle and not the
  // one being worked on are let go and reloaded when next needed.
  void evictLocked(int keep, bool mayWrite = false) {
    size_t total = 0;
    for (const Layer& l : layers_) total += l.canvases.size();
    if (total > 24)
      for (Layer& l : layers_)
        for (auto it = l.canvases.begin(); it != l.canvases.end() && total > 16;) {
          if (it->first != keep && it->second.use_count() == 1 && !it->second->dirty) {
            it = l.canvases.erase(it);
            --total;
          } else ++it;
        }
    // By size, as the NDK node does: frames furthest from the current one go first.
    // An unsaved frame is written to its sidecar file first, and only from the
    // interface thread; one that cannot be written stays in memory.
    if (frameBudget_ == 0) return;
    struct Cand { Layer* layer; int key; size_t bytes; long long away; };
    std::vector<Cand> cands;
    size_t held = 0;
    for (Layer& l : layers_)
      for (auto& kv : l.canvases) {
        if (kv.first == keep || kv.second.use_count() != 1) continue;
        const size_t b = kv.second->memoryBytes();
        const long long d = (long long)kv.first - (long long)keep;
        held += b;
        cands.push_back(Cand{&l, kv.first, b, d < 0 ? -d : d});
      }
    if (held <= frameBudget_) return;
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.away > y.away; });
    for (const Cand& c : cands) {
      if (held <= frameBudget_) break;
      auto it = c.layer->canvases.find(c.key);
      if (it == c.layer->canvases.end()) continue;
      rp::Canvas& cv = *it->second;
      if (cv.dirty) {
        if (!mayWrite || embedded_ || id_.empty()) continue;
        std::error_code ec;
        const std::string dir = dirLocked();
        fs::create_directories(dir, ec);
        if (cv.empty()) fs::remove(fileFor(dir, c.layer->id, c.key), ec);
        else if (!cv.save(fileFor(dir, c.layer->id, c.key))) continue;
        cv.dirty = false;
      }
      c.layer->canvases.erase(it);
      held -= c.bytes;
    }
  }

  // Every frame one layer has paint for, loaded into memory. mutex_ held.
  void loadAllFramesLocked(Layer& l) {
    std::vector<int> keys;
    if (embedded_) {
      for (const auto& kv : blobs_)
        if (ckId(kv.first) == l.id) keys.push_back(ckKey(kv.first));
    } else if (!id_.empty()) {
      std::error_code ec;
      const std::string dir = dirLocked();
      char prefix[32];
      std::snprintf(prefix, sizeof(prefix), "paint.L%02d.", l.id);
      const bool first = !layers_.empty() && &l == &layers_.front();
      if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir, ec)) {
          const std::string n = e.path().filename().generic_string();
          int f = 0;
          if (n.rfind(prefix, 0) == 0) {
            const std::string rest = n.substr(std::strlen(prefix));
            if (rest == "rpt") keys.push_back(kAllFrames);
            else if (std::sscanf(rest.c_str(), "%d", &f) == 1) keys.push_back(f);
          } else if (first) {
            if (n == "paint.rpt") keys.push_back(kAllFrames);
            else if (std::sscanf(n.c_str(), "paint.%d.rpt", &f) == 1 && n.find(".L") == std::string::npos)
              keys.push_back(f);
          }
        }
    }
    for (int k : keys) {
      if (l.canvases.count(k)) continue;
      auto c = std::make_shared<rp::Canvas>();
      loadLocked(l, k, *c);
      l.canvases[k] = c;
    }
  }

  // Writes everything unsaved. `loud` is the button: says what happened.
  void savePaint(bool loud) {
    if (engine_.stroking()) return;
    claimId();
    std::string err;
    int frames = 0;
    std::string newData;
    bool dataChanged = false;
    {
      std::lock_guard<std::mutex> g(mutex_);
      if (embedded_) {
        // Paint of deleted layers goes with them.
        for (auto it = blobs_.begin(); it != blobs_.end();) {
          bool keep = false;
          for (const Layer& l : layers_) keep = keep || l.id == ckId(it->first);
          if (keep) ++it;
          else { it = blobs_.erase(it); dataChanged = true; }
        }
        for (Layer& l : layers_)
          for (auto& kv : l.canvases) {
            rp::Canvas& c = *kv.second;
            std::shared_lock<std::shared_mutex> cl(c.mutex);
            if (!c.dirty) continue;
            if (c.empty()) blobs_.erase(ck(l.id, kv.first));
            else {
              std::string bytes;
              if (!canvasToBytes(c, bytes)) { err = "could not encode the paint"; continue; }
              blobs_[ck(l.id, kv.first)] = b64encode(bytes);
            }
            c.dirty = false;
            ++frames;
            dataChanged = true;
          }
        if (dataChanged) newData = joinBlobs(blobs_);
      } else {
        const std::string dir = dirLocked();
        std::error_code ec;
        for (Layer& l : layers_)
          for (auto& kv : l.canvases) {
            rp::Canvas& c = *kv.second;
            std::shared_lock<std::shared_mutex> cl(c.mutex);
            if (!c.dirty) continue;
            const std::string f = fileFor(dir, l.id, kv.first);
            if (c.empty()) fs::remove(f, ec);
            else {
              fs::create_directories(dir, ec);
              if (!c.save(f)) { err = "could not write " + f; continue; }
            }
            // Paint from the first beta now lives under the layer's own name.
            if (&l == &layers_.front()) fs::remove(legacyFileFor(dir, kv.first), ec);
            c.dirty = false;
            ++frames;
          }
      }
    }
    if (dataChanged) paintData_->setValue(newData);
    writeLayerData();
    savedAt_ = std::chrono::system_clock::now();
    haveSaved_ = true;
    reportStorage();
    if (!err.empty()) sendMessage(OFX::Message::eMessageError, "", "NPaintShop: " + err);
    else if (loud)
      sendMessage(OFX::Message::eMessageMessage, "",
                  frames ? "NPaintShop: saved " + std::to_string(frames) + " frame(s)."
                         : std::string("NPaintShop: nothing unsaved."));
  }

  // 'store paint' or 'paint directory' changed: bring every frame across, so switching
  // never loses paint. It lands in the new place at the next save.
  void storageChanged() {
    if (engine_.stroking()) return;
    {
      std::lock_guard<std::mutex> g(mutex_);
      for (Layer& l : layers_) loadAllFramesLocked(l);  // from the old place
      for (Layer& l : layers_)
        for (auto& kv : l.canvases) kv.second->dirty = true;
      embedded_ = cv(storage_) == 1;
      userDir_ = sv(paintDir_);
      absent_.clear();
      if (!embedded_) blobs_.clear();
    }
    if (cv(storage_) == 0 && !sv(paintData_).empty()) paintData_->setValue("");
    reportStorage();
  }

  void reloadAll() {
    std::lock_guard<std::mutex> g(mutex_);
    for (Layer& l : layers_) l.canvases.clear();
    absent_.clear();
    engine_.clearHistory();
  }

  // Gives this node an id of its own. A copied node arrives with the original's id;
  // the copy gets a new one and a copy of the original's paint, so painting on one
  // never changes the other.
  void claimId() {
    std::string want = sv(paintId_);
    PaintEffect* original = nullptr;
    {
      std::lock_guard<std::mutex> g(registryMutex());
      if (!want.empty() && want == id_) return;
      auto it = registry().find(want);
      if (!want.empty() && it != registry().end() && it->second != this) original = it->second;
      if (want.empty() || original) want = newId();
      auto mine = registry().find(id_);
      if (mine != registry().end() && mine->second == this) registry().erase(mine);
      registry()[want] = this;
    }
    if (original) copyPaintFrom(*original);
    {
      std::lock_guard<std::mutex> g(mutex_);
      id_ = want;
    }
    if (sv(paintId_) != want) paintId_->setValue(want);
    reportStorage();
  }

  void copyPaintFrom(PaintEffect& o) {
    std::vector<Layer> copies;
    {
      std::lock_guard<std::mutex> g(o.mutex_);
      for (Layer& l : o.layers_) o.loadAllFramesLocked(l);
      copies = o.layers_;
    }
    for (Layer& l : copies)
      for (auto& kv : l.canvases) kv.second = copyCanvas(*kv.second);
    std::lock_guard<std::mutex> g(mutex_);
    layers_ = std::move(copies);
    absent_.clear();
  }

  // ------------------------------------------------------------------ brushes

  void refreshBrushSets() {
    sets_ = npaint::BrushLibrary::list();
    brushSet_->resetOptions();
    brushSet_->appendOption("(none)");
    for (const std::string& s : sets_) brushSet_->appendOption(s);
    selectSet(sv(setName_));
  }
  void selectSet(const std::string& name) {
    int idx = 0;
    for (size_t i = 0; i < sets_.size(); ++i)
      if (sets_[i] == name) idx = int(i) + 1;
    if (cv(brushSet_) != idx) brushSet_->setValue(idx);
  }

  void loadBrushes(bool force) {
    std::string path;
    const std::string set = sv(setName_);
    if (!set.empty()) {
      path = npaint::BrushLibrary::pathFor(set);
      if (path.empty() && abrLoaded_ != set) {
        abrLoaded_ = set;
        abr_ = rp::AbrFile();
        setBrushMenu();
        sendMessage(OFX::Message::eMessageWarning, "",
                    "NPaintShop: brush set '" + set + "' is not installed on this machine.");
        return;
      }
    } else {
      path = sv(abrFile_);
    }
    if (!force && path == abrLoaded_ && (path.empty() || !abr_.empty())) return;
    abrLoaded_ = path;
    abr_ = rp::AbrFile();
    if (!path.empty()) {
      std::string err;
      abr_ = rp::loadABR(path, &err);
      if (abr_.empty()) sendMessage(OFX::Message::eMessageError, "", "NPaintShop: " + path + ": " + err);
    }
    setBrushMenu();
  }
  void setBrushMenu() {
    const int keep = cv(brush_);
    brush_->resetOptions();
    brush_->appendOption("round");
    for (const rp::BrushPreset& pr : abr_.presets)
      brush_->appendOption(pr.tip < 0 ? pr.name + "  (round)" : pr.name);
    if (keep > int(abr_.presets.size())) brush_->setValue(0);
  }

  // Copies a brush file's own settings into the parameters when a brush is picked.
  void applyBrushFileSettings() {
    const int sel = cv(brush_);
    if (!useFileSettings_->getValue() || sel <= 0 || size_t(sel - 1) >= abr_.presets.size()) return;
    const rp::BrushPreset& pr = abr_.presets[size_t(sel - 1)];
    if (!pr.fromFile) return;
    const rp::BrushDynamics& d = pr.dyn;
    auto ctl = [&](const rp::DynControl& c, const char* j, const char* k, const char* fd, const char* mn) {
      setNum(j, c.jitter); setNum(k, c.control); setNum(fd, c.fadeSteps);
      if (mn) setNum(mn, c.minimum);
    };
    setNum("size", pr.diameter);
    setNum("spacing", pr.spacing);
    setNum("hardness", pr.hardness);
    setNum("dyn_angle", d.angle); setNum("dyn_roundness", d.roundness);
    setNum("dyn_flip_x", d.flipX); setNum("dyn_flip_y", d.flipY);
    ctl(d.size, "size_jitter", "size_control", "size_fade", "size_minimum");
    setNum("angle_jitter", d.angleJitter); setNum("angle_control", d.angleControl);
    setNum("angle_fade", d.angleFadeSteps);
    ctl(d.round, "round_jitter", "round_control", "round_fade", "round_minimum");
    setNum("flip_x_jitter", d.flipXJitter); setNum("flip_y_jitter", d.flipYJitter);
    setNum("scatter", d.scatter); setNum("scatter_both", d.scatterBoth);
    setNum("scatter_count", d.count); setNum("count_jitter", d.countJitter);
    ctl(d.opacity, "opacity_jitter", "opacity_control", "opacity_fade", "opacity_minimum");
    ctl(d.flow, "flow_jitter", "flow_control", "flow_fade", "flow_minimum");
    setNum("noise", d.noise); setNum("wet_edges", d.wetEdges);
    ctl(d.fgbg, "fgbg_jitter", "fgbg_control", "fgbg_fade", "fgbg_minimum");
    setNum("hue_jitter", d.hueJitter); setNum("sat_jitter", d.satJitter);
    setNum("bri_jitter", d.briJitter); setNum("purity", d.purity);
    setNum("color_per_tip", d.colorPerTip);
  }

  // ------------------------------------------------------------------ presets

  void refreshPresetMenu(int select) {
    const std::vector<Preset> ps = BrushPresets::read();
    preset_->resetOptions();
    preset_->appendOption("(none)");
    for (const Preset& p : ps) preset_->appendOption(p.name);
    if (select >= 0) preset_->setValue(select);
    else if (cv(preset_) > int(ps.size())) preset_->setValue(0);
  }

  void savePreset() {
    std::string name = sv(presetName_);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) {
      sendMessage(OFX::Message::eMessageError, "", "NPaintShop: type a name before saving a preset.");
      return;
    }
    std::string line;
    char buf[128];
    for (const PDef& d : kBrush) {
      std::snprintf(buf, sizeof(buf), "%s=%.6g\t", d.name, getNum(d.name));
      line += buf;
    }
    line += "abr=" + esc(abrLoaded_ == sv(setName_) && !sv(setName_).empty()
                             ? npaint::BrushLibrary::pathFor(sv(setName_))
                             : sv(abrFile_)) + "\t";
    std::snprintf(buf, sizeof(buf), "brush=%d", cv(brush_));
    line += buf;
    std::vector<Preset> ps = BrushPresets::read();
    size_t at = ps.size();
    for (size_t i = 0; i < ps.size(); ++i)
      if (ps[i].name == name) at = i;
    if (at == ps.size()) ps.push_back(Preset{name, line});
    else ps[at].line = line;
    if (!BrushPresets::write(ps)) {
      sendMessage(OFX::Message::eMessageError, "", "NPaintShop: could not write " + BrushPresets::path());
      return;
    }
    refreshPresetMenu(int(at) + 1);
  }

  void deletePreset() {
    const int sel = cv(preset_) - 1;
    std::vector<Preset> ps = BrushPresets::read();
    if (sel < 0 || sel >= int(ps.size())) {
      sendMessage(OFX::Message::eMessageError, "", "NPaintShop: pick a preset to delete first.");
      return;
    }
    ps.erase(ps.begin() + sel);
    if (!BrushPresets::write(ps)) {
      sendMessage(OFX::Message::eMessageError, "", "NPaintShop: could not write " + BrushPresets::path());
      return;
    }
    refreshPresetMenu(0);
  }

  void applyPreset(int sel) {
    const std::vector<Preset> ps = BrushPresets::read();
    if (sel <= 0 || sel > int(ps.size())) return;
    const std::string line = ps[size_t(sel - 1)].line;
    for (const PDef& d : kBrush) setNum(d.name, d.def);
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
      if (key == "abr") { abr = unesc(val); continue; }
      if (key == "brush") { brushIdx = std::atoi(val.c_str()); continue; }
      if (findDef(key)) setNum(key.c_str(), std::atof(val.c_str()));
    }
    if (!abr.empty() && abr != abrLoaded_) {
      setName_->setValue("");
      brushSet_->setValue(0);
      abrFile_->setValue(abr);
      loadBrushes(true);
    }
    if (brushIdx >= 0 && brushIdx <= int(abr_.presets.size())) brush_->setValue(brushIdx);
  }


  // ------------------------------------------------------------------ readouts

  void reportPen() {
    const pen::Stats& s = pen::stats();
    char b[256];
    if (std::string(s.lastSource) == "none")
      std::snprintf(b, sizeof(b), "no pen pressure in the last stroke (mouse, or no tablet driver found)");
    else
      std::snprintf(b, sizeof(b), "%s - last stroke %.2f to %.2f", s.lastSource, pressMin_, pressMax_);
    setStatus(penStatus_, b);
  }

  // How the last stroke went, in words, so "it feels slow" can be pinned down: the
  // painting itself, the viewer updates, or the host not updating at all.
  void reportStroke() {
    const double secs = std::chrono::duration<double>(Clock::now() - strokeStart_).count();
    const long r = renders_.load();
    const long others = allRenders().load() - allAtStart_ - r;
    const double avgRender = r ? double(renderUs_.load()) / r / 1000.0 : 0.0;
    const double paintShare = secs > 0 ? double(paintUs_) / 1e6 / secs : 0.0;
    const long long first = firstRenderUs_.load();
    char firstTxt[48];
    if (first >= 0) std::snprintf(firstTxt, sizeof(firstTxt), "first after %.0f ms", first / 1000.0);
    else std::snprintf(firstTxt, sizeof(firstTxt), "none while drawing");
    // The plain-language line, as on the NDK node.
    {
      const long long wallUs = (long long)(secs * 1e6);
      const int pct = wallUs > 0 ? int(std::lround(100.0 * double(paintUs_) / double(wallUs))) : 0;
      const char* verdict = pct < 30 ? "kept up easily" : pct < 70 ? "kept up" : "struggled to keep up";
      auto timeText = [](long long u) {
        char t[32];
        if (u >= 1000000) std::snprintf(t, sizeof(t), "%.1f s", double(u) / 1e6);
        else std::snprintf(t, sizeof(t), "%.0f ms", double(u) / 1e3);
        return std::string(t);
      };
      setStatus(strokeStatus_, timeText(paintUs_) + " of painting in a " + timeText(wallUs) + " stroke (" +
                                   std::to_string(pct) + "%): " + verdict);
    }
    if (cloneStroke_) {
      char c[256];
      const int miss = engine_.bgMisses();
      std::snprintf(c, sizeof(c), "%s%s: %d tile%s read%s", cloneFromB_ ? "input B" : "input A",
                    cloneHadPlate_ ? "" : " (not available here - paint layers only)", engine_.bgHits(),
                    engine_.bgHits() == 1 ? "" : "s",
                    miss ? (", " + std::to_string(miss) + " could not be read").c_str() : "");
      setStatus(cloneStatus_, c);
    }
    char b[512];
    std::snprintf(b, sizeof(b),
                  "%.1f s, %ld pen moves; painting %.0f%% of it; asked for %ld updates, got %ld "
                  "(%.0f ms each, %s), %ld skipped as unchanged, %ld elsewhere; %ld overlay redraws; "
                  "live preview: %ld tile updates, drawn %ld times, up to %ld tiles; %d instance(s)",
                  secs, moves_, paintShare * 100.0, refreshes_, r, avgRender, firstTxt, identities_.load(),
                  others < 0 ? 0L : others, overlayDraws_, previewBuilt_, previewDraws_, previewTilesMax_,
                  liveInstances().load());
    setStatus(strokeDetails_, b);
  }

  void reportMemory() {
    size_t paint = 0;
    int frames = 0;
    {
      std::lock_guard<std::mutex> g(mutex_);
      for (Layer& l : layers_)
        for (auto& kv : l.canvases) {
          std::shared_lock<std::shared_mutex> cl(kv.second->mutex);
          paint += kv.second->memoryBytes();
          if (!kv.second->empty()) ++frames;
        }
    }
    size_t cache = 0;
    {
      std::lock_guard<std::mutex> g(inputMutex_);
      cache = inputTiles_.size() * sizeof(rp::SrcTile);
    }
    (void)frames;
    setStatus(memStatus_, "paint " + sizeText(paint) + "  \xc2\xb7  undo " + sizeText(engine_.historyBytes()) +
                              " of " + sizeText(size_t(std::max(64, undoMB_->getValue())) << 20) +
                              "  \xc2\xb7  clone cache " + sizeText(cache) + " of " +
                              sizeText(inputBudget_.load()));
    reportStorage();
  }
  void reportStorage() {
    std::string where;
    int unsaved = 0;
    {
      std::lock_guard<std::mutex> g(mutex_);
      where = embedded_ ? std::string("inside the script") : dirLocked();
      for (Layer& l : layers_)
        for (auto& kv : l.canvases) unsaved += kv.second->dirty ? 1 : 0;
    }
    std::string s = where;
    if (unsaved) s += " - " + std::to_string(unsaved) + (unsaved == 1 ? " canvas" : " canvases") + " not saved yet";
    else if (haveSaved_) {
      const std::time_t t = std::chrono::system_clock::to_time_t(savedAt_);
      char b[32];
      std::strftime(b, sizeof(b), "%H:%M:%S", std::localtime(&t));
      s += std::string(" - saved ") + b;
    }
    setStatus(storageStatus_, s);
  }

  // ------------------------------------------------------------------ state

  OFX::Clip *dst_ = nullptr, *src_ = nullptr;
  OFX::ChoiceParam *tool_, *brushSet_, *brush_, *preset_, *lifetime_, *channels_, *output_, *penSource_,
      *storage_, *layer_, *layerBlend_;
  OFX::RGBParam *color_, *bgColor_;
  OFX::BooleanParam *useFileSettings_, *layerVisible_, *layerLocked_;
  OFX::IntParam *history_, *rev_, *undoMB_, *refreshMs_;
  OFX::DoubleParam *layerOpacity_, *cloneTime_, *smudgeStrength_, *smudgeRate_;
  OFX::Double2DParam* cloneOffset_;
  OFX::StringParam *abrFile_, *setName_, *presetName_, *paintDir_, *paintId_, *paintData_, *layerData_,
      *layerName_;
  OFX::StringParam *penStatus_, *storageStatus_, *memStatus_, *strokeStatus_;

  // Guards everything the render threads read: layers_ and their canvases, blobs_,
  // absent_, id_, embedded_, userDir_.
  std::mutex mutex_;
  std::vector<Layer> layers_;
  int active_ = 0, nextId_ = 1;
  std::map<uint64_t, std::string> blobs_;  // embedded paint, base64, by layer and frame
  std::set<uint64_t> absent_;             // layer frames looked up and found to have no paint
  std::string id_;
  bool embedded_ = false;
  std::string userDir_;

  // UI thread only.
  rp::PaintEngine engine_;
  std::shared_ptr<rp::Canvas> strokeCanvas_;
  std::unique_ptr<OFX::Image> srcImage_;  // clone / smudge source, for one stroke
  std::vector<Shot> srcStack_;
  bool warnedNoSource_ = false;
  bool cloneSrcPending_ = false;
  double cloneSrcX_ = 0, cloneSrcY_ = 0;
  std::vector<std::string> layerMenu_;
  int strokeIndex_ = 0;
  rp::AbrFile abr_;
  std::string abrLoaded_;
  std::vector<std::string> sets_;
  Clock::time_point lastPaintRefresh_, strokeStart_;
  rp::BrushParams cached_;
  bool paramsDirty_ = true;
  int refreshMsCached_ = 40;
  std::mutex inputMutex_;
  std::unordered_map<uint64_t, std::shared_ptr<rp::SrcTile>> inputTiles_;
  double inputTime_ = -1e30;
  std::unique_ptr<OFX::Image> pickImage_;
  int strokeKey_ = kAllFrames;
  double strokeTime_ = 0;
  int previewColour_ = 0, previewOutput_ = 0, previewChannels_ = 0;
  OFX::ChoiceParam* previewColourParam_ = nullptr;
  OFX::ChoiceParam* cloneInput_ = nullptr;
  OFX::IntParam *cloneCacheMB_ = nullptr, *frameMB_ = nullptr;
  OFX::StringParam *strokeDetails_ = nullptr, *cloneStatus_ = nullptr;
  OFX::Clip* srcB_ = nullptr;
  std::atomic<size_t> inputBudget_{size_t(512) << 20};
  size_t frameBudget_ = size_t(2048) << 20;  // guarded by mutex_
  bool cloneStroke_ = false, cloneHadPlate_ = false, cloneFromB_ = false;
  long overlayDraws_ = 0, previewDraws_ = 0, previewTilesMax_ = 0, previewBuilt_ = 0;
  std::set<uint64_t> previewDirty_;
  std::vector<PreviewTile> previewReady_;
  int previewSerial_ = 0;
  float previewOpacity_ = 1.0f;
  Clock::time_point strokeEnd_;
  long rendersAtEnd_ = 0;
  long moves_ = 0, refreshes_ = 0, allAtStart_ = 0, rendersAtRefresh_ = 0;
  long long paintUs_ = 0;
  double pressMin_ = 1, pressMax_ = 0;
  std::chrono::system_clock::time_point savedAt_;
  bool haveSaved_ = false;
  // Written by render threads.
  std::atomic<long> renders_{0}, identities_{0};
  std::atomic<long long> renderUs_{0}, firstRenderUs_{-1}, lastRenderNs_{0};
};

// ---------------------------------------------------------------------------------
// The viewer overlay: the toolbar (shared with the NDK node, Overlay.h), the brush
// outline, and the pen.
//
// Pen positions arrive in the host's canonical coordinates: image pixels, scaled by
// the pixel aspect ratio horizontally. The canvas is in image pixels. The toolbar works
// in viewport pixels, so the matrices of the last draw are kept to map the pen into it.

class PaintOverlay : public OFX::OverlayInteract {
 public:
  PaintOverlay(OfxInteractHandle h, OFX::ImageEffect* e) : OFX::OverlayInteract(h) {
    (void)e;
    // Open the tablet now rather than on the first stroke, so the first stroke after
    // opening the panel already has pressure.
    pen::start();
  }

  bool draw(const OFX::DrawArgs& args) override {
    PaintEffect* fx = effect();
    fx->noteOverlayDraw();
    glGetDoublev(GL_MODELVIEW_MATRIX, mv_);
    glGetDoublev(GL_PROJECTION_MATRIX, pj_);
    glGetIntegerv(GL_VIEWPORT, vp_);
    haveMatrices_ = true;

    const rp::BrushParams& bp = fx->uiParams();
    const double par = fx->pixelAspect();
    drawPreview(par);
    if (hasPos_ && !overUI_) {
      const double r = std::max(1.0, double(bp.size) * 0.5);
      glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glEnable(GL_LINE_SMOOTH);
      glLineWidth(1.5f);
      for (int pass = 0; pass < 2; ++pass) {  // dark then light, visible on any image
        if (pass == 0) glColor4f(0, 0, 0, 0.6f); else glColor4f(1, 1, 1, 0.9f);
        const double o = pass == 0 ? args.pixelScale.x : 0.0;
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 64; ++i) {
          const double a = i * (6.283185307179586 / 64);
          glVertex2d(pos_.x + (r + o) * par * std::cos(a), pos_.y + (r + o) * std::sin(a));
        }
        glEnd();
      }
      // Shift held with a stroke behind it: show where Shift+click will draw the line.
      if (haveLast_ && mode_ == kIdle && currentMods().shift && !currentMods().ctrl) {
        glColor4f(1, 1, 1, 0.5f);
        glBegin(GL_LINES);
        glVertex2d(lastX_ * par, lastY_);
        glVertex2d(pos_.x, pos_.y);
        glEnd();
      }
      glPopAttrib();
      // Clone: where the brush is copying from. A pending Ctrl+click shows a cross at
      // the chosen spot; otherwise a ring follows the brush at the clone offset.
      if (fx->tool() == 2) {
        double sx = 0, sy = 0, dx = 0, dy = 0;
        const bool pending = fx->cloneSourcePending(sx, sy);
        if (!pending) {
          fx->cloneOffset(dx, dy);
          sx = pos_.x / par + dx;
          sy = pos_.y + dy;
        }
        glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(1.5f);
        glColor4f(1.0f, 0.75f, 0.2f, 0.9f);
        const double cx = sx * par, cyy = sy, arm = 8.0 * args.pixelScale.x;
        if (pending) {
          glBegin(GL_LINES);
          glVertex2d(cx - arm, cyy); glVertex2d(cx + arm, cyy);
          glVertex2d(cx, cyy - arm); glVertex2d(cx, cyy + arm);
          glEnd();
        } else {
          glBegin(GL_LINE_LOOP);
          for (int i = 0; i < 48; ++i) {
            const double a = i * (6.283185307179586 / 48);
            glVertex2d(cx + r * par * std::cos(a), cyy + r * std::sin(a));
          }
          glEnd();
        }
        glPopAttrib();
      }
      // The tip itself, faintly, for brushes that are not a plain circle.
      if (bp.tip || bp.dyn.roundness < 0.999f) {
        rp::DabShape sh;
        sh.diameter = bp.size;
        sh.hardness = bp.hardness;
        sh.angle = bp.dyn.angle;
        sh.roundness = bp.dyn.roundness;
        sh.flipX = bp.dyn.flipX;
        sh.flipY = bp.dyn.flipY;
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glScaled(par, 1.0, 1.0);
        ui_.drawCursorTip(float(pos_.x / par), float(pos_.y), sh, bp.tip);
        glPopMatrix();
      }
    }

    rpui::Overlay::State st = state();
    double mx = 0, my = 0;
    const bool hover = hasPos_ && toViewport(pos_, mx, my);
    ui_.setHover(hover, int(mx), int(my));
    ui_.draw(st);
    return true;
  }

  bool penMotion(const OFX::PenArgs& args) override {
    pos_ = args.penPosition;
    hasPos_ = true;
    PaintEffect* fx = effect();
    const double par = fx->pixelAspect();
    const double x = pos_.x / par, y = pos_.y;
    double mx = 0, my = 0;
    const bool inView = toViewport(pos_, mx, my);
    switch (mode_) {
      case kResize:
        fx->setSize(startSize_ + (pos_.x - downX_) / par);
        break;
      case kGrid:
        if (inView) {
          const float cell = ui_.cellSize();
          ui_.setGrid(gridCols_ + int(std::lround((mx - gridMx_) / cell)),
                      gridRows_ + int(std::lround((my - gridMy_) / cell)));
        }
        break;
      case kPending: {
        // Shift: the stroke's axis is whichever way the pen first moves clearly.
        const double dx = (pos_.x - downX_) / args.pixelScale.x, dy = (pos_.y - downY_) / args.pixelScale.y;
        if (std::hypot(dx, dy) > 4.0) {
          axis_ = std::fabs(dx) >= std::fabs(dy) ? kHorizontal : kVertical;
          mode_ = kStroke;
          fx->beginStroke(downX_ / par, downY_, pressure(args), args.time);
          fx->strokeTo(constrainX(x), constrainY(y), pressure(args));
        }
        break;
      }
      case kStroke:
        fx->strokeTo(constrainX(x), constrainY(y), pressure(args));
        break;
      case kPick:
        fx->pickColor(x, y, args.time);
        break;
      case kUI:
      case kIdle:
        overUI_ = inView && ui_.overUI(int(mx), int(my));
        if (pickHover_) {
          if (altDown()) fx->pickColor(x, y, args.time);
          else stopHoverPick();
        }
        break;
    }
    // A tablet can send several hundred moves a second. Redrawing the viewer for each
    // one keeps the host's interface thread too busy to show the renders it is asked
    // for, so the outline is redrawn at most about 60 times a second.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastRedraw_ > std::chrono::milliseconds(16)) {
      lastRedraw_ = now;
      _effect->redrawOverlays();
    }
    return mode_ != kIdle;
  }

  bool penDown(const OFX::PenArgs& args) override {
    pos_ = args.penPosition;
    hasPos_ = true;
    PaintEffect* fx = effect();
    const double par = fx->pixelAspect();
    double mx = 0, my = 0;
    if (toViewport(pos_, mx, my) && ui_.overUI(int(mx), int(my))) {
      mode_ = kUI;
      int idx = 0;
      const rpui::Action act = ui_.hitTest(int(mx), int(my), idx);
      clickToolbar(act, idx, mx, my);
      _effect->redrawOverlays();
      return true;
    }
    if (pickHover_) stopHoverPick();
    if (picking_) {  // Alt+C armed: this press samples colour, and keeps sampling as it drags
      fx->pickColor(pos_.x / par, pos_.y, args.time);
      mode_ = kPick;
      _effect->redrawOverlays();
      return true;
    }
    const Mods m = currentMods();
    downX_ = pos_.x;
    downY_ = pos_.y;
    axis_ = kFree;
    if (m.ctrl && m.shift) {
      mode_ = kResize;
      startSize_ = fx->brushSize();
    } else if (m.ctrl) {
      // Ctrl+click: the clone source. Only claimed for clone, so Ctrl keeps doing what
      // the host does with it (colour sampling in Nuke) for every other tool.
      if (fx->tool() != 2) {
        mode_ = kIdle;
        return false;
      }
      fx->setCloneSource(pos_.x / par, pos_.y);
      mode_ = kUI;
    } else if (m.shift) {
      mode_ = kPending;  // a click or a drag - not known yet
    } else {
      mode_ = kStroke;
      fx->beginStroke(pos_.x / par, pos_.y, pressure(args), args.time);
    }
    _effect->redrawOverlays();
    return true;  // the stroke is ours: do not let the viewer pan or pick
  }

  bool penUp(const OFX::PenArgs& args) override {
    PaintEffect* fx = effect();
    const double par = fx->pixelAspect();
    const Mode was = mode_;
    mode_ = kIdle;
    if (was == kPick) {
      picking_ = false;
      fx->endPick();
    } else if (was == kPending) {
      // Shift+click: a straight line from where the last stroke ended.
      const double x = downX_ / par, y = downY_;
      const double p = pressure(args);
      if (haveLast_) {
        fx->beginStroke(lastX_, lastY_, p, args.time);
        fx->strokeTo(x, y, p);
      } else {
        fx->beginStroke(x, y, p, args.time);
      }
      fx->endStroke();
      lastX_ = x; lastY_ = y; haveLast_ = true;
    } else if (was == kStroke) {
      fx->endStroke();
      lastX_ = constrainX(pos_.x / par);
      lastY_ = constrainY(pos_.y);
      haveLast_ = true;
    }
    _effect->redrawOverlays();
    return was != kIdle;
  }

  bool keyDown(const OFX::KeyArgs& a) override {
    if (modifier(a.keySymbol, true)) return false;  // never swallow modifiers
    if (!altDown()) return false;
    PaintEffect* fx = effect();
    int k = a.keySymbol;
    if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
    switch (k) {
      case 'b': fx->setTool(0); break;
      case 'e': fx->setTool(1); break;
      case 's': fx->setTool(2); break;
      case 'u': fx->setTool(3); break;
      case 'm': pickerOpen_ = !pickerOpen_; break;
      case 'l': layersOpen_ = !layersOpen_; break;
      case 'c':
        // Picks straight away from under the cursor, so no click is needed - a click
        // with Alt still held would pan Nuke's viewer instead. While Alt stays down the
        // colour follows the cursor; letting go of Alt ends it, and the next click
        // paints as usual.
        if (hasPos_) {
          const double par = fx->pixelAspect();
          fx->pickColor(pos_.x / par, pos_.y, a.time);
        }
        pickHover_ = true;  // keeps picking as the cursor moves, until Alt is let go
        break;
      case kOfxKey_bracketleft: fx->setSize(fx->brushSize() / 1.15); break;
      case kOfxKey_bracketright: fx->setSize(fx->brushSize() * 1.15); break;
      default: return false;
    }
    _effect->redrawOverlays();
    return true;
  }
  bool keyUp(const OFX::KeyArgs& a) override {
    modifier(a.keySymbol, false);
    if (pickHover_ && !altDown()) stopHoverPick();
    return false;
  }
  void loseFocus(const OFX::FocusArgs&) override {
    trackedMods() = Mods();
    altTracked_ = false;
    if (pickHover_) stopHoverPick();
    hasPos_ = false;
    _effect->redrawOverlays();
  }

 private:
  enum Mode { kIdle, kStroke, kPending, kResize, kUI, kGrid, kPick };
  enum Axis { kFree, kHorizontal, kVertical };

  PaintEffect* effect() { return static_cast<PaintEffect*>(_effect); }
  void stopHoverPick() {
    pickHover_ = false;
    effect()->endPick();
  }

  // The stroke being painted, drawn by the overlay because the host does not render
  // while the pen is down. See PaintEffect::takePreview.
  void drawPreview(double par) {
    PaintEffect* fx = effect();
    std::vector<PaintEffect::PreviewTile> tiles;
    int serial = 0;
    fx->takePreview(tiles, serial);
    if (serial != previewSerial_) {
      for (auto& kv : previewTex_)
        if (kv.second.id) glDeleteTextures(1, &kv.second.id);
      previewTex_.clear();
      previewSerial_ = serial;
    }
    for (PaintEffect::PreviewTile& t : tiles)
      previewTex_[t.key].upload(++uploads_, rp::TILE, rp::TILE, t.rgba);
    if (!fx->previewVisible() || previewTex_.empty()) return;
    fx->notePreviewDrawn(previewTex_.size());
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // the tiles are premultiplied
    for (const auto& kv : previewTex_) {
      const double x0 = double(rp::keyTx(kv.first)) * rp::TILE, y0 = double(rp::keyTy(kv.first)) * rp::TILE;
      rpui::blitTex(kv.second, rpui::Rect{float(x0 * par), float(y0), float(rp::TILE * par), float(rp::TILE)});
    }
    glPopAttrib();
  }

  rpui::Overlay::State state() {
    PaintEffect* fx = effect();
    rpui::Overlay::State st;
    st.tool = fx->tool();
    st.brush = fx->brushIndex();
    st.pickerOpen = pickerOpen_;
    st.layersOpen = layersOpen_;
    st.layers = fx->layerRows();
    st.activeLayer = fx->activeLayer();
    st.colorPicking = picking_ || pickHover_;
    st.params = fx->uiParams();
    for (int c = 0; c < 3; ++c) st.color[c] = st.params.color[c];
    st.canUndo = fx->canUndo();
    st.canRedo = fx->canRedo();
    st.tips = fx->tips();
    return st;
  }

  void clickToolbar(rpui::Action act, int idx, double mx, double my) {
    PaintEffect* fx = effect();
    switch (act) {
      case rpui::kToolPaint: fx->setTool(0); break;
      case rpui::kToolErase: fx->setTool(1); break;
      case rpui::kToolClone: fx->setTool(2); break;
      case rpui::kToolSmudge: fx->setTool(3); break;
      case rpui::kTogglePicker: pickerOpen_ = !pickerOpen_; break;
      case rpui::kToggleLayers: layersOpen_ = !layersOpen_; break;
      case rpui::kSelectLayer: fx->selectLayer(idx); break;
      case rpui::kToggleLayerVis: fx->toggleLayerVisible(idx); break;
      case rpui::kUndo: fx->stepHistory(-1); break;
      case rpui::kRedo: fx->stepHistory(+1); break;
      case rpui::kPagePrev: ui_.pagePrev(); break;
      case rpui::kPageNext: ui_.pageNext(); break;
      case rpui::kPickBrush: fx->pickBrush(idx); break;
      case rpui::kResizePicker:
        mode_ = kGrid;
        gridMx_ = mx;
        gridMy_ = my;
        gridCols_ = ui_.gridCols();
        gridRows_ = ui_.gridRows();
        break;
      default: break;
    }
  }

  // Canonical coordinates to viewport pixels, origin top-left (as the toolbar expects),
  // using the projection of the last draw.
  bool toViewport(const OfxPointD& p, double& mx, double& my) const {
    if (!haveMatrices_ || vp_[2] <= 0 || vp_[3] <= 0) return false;
    double in[4] = {p.x, p.y, 0.0, 1.0}, eye[4], clip[4];
    for (int r = 0; r < 4; ++r) {
      eye[r] = 0;
      for (int c = 0; c < 4; ++c) eye[r] += mv_[c * 4 + r] * in[c];
    }
    for (int r = 0; r < 4; ++r) {
      clip[r] = 0;
      for (int c = 0; c < 4; ++c) clip[r] += pj_[c * 4 + r] * eye[c];
    }
    if (std::fabs(clip[3]) < 1e-12) return false;
    const double nx = clip[0] / clip[3], ny = clip[1] / clip[3];
    const double wx = (nx * 0.5 + 0.5) * vp_[2];
    const double wy = (ny * 0.5 + 0.5) * vp_[3];
    mx = wx;
    my = double(vp_[3]) - wy;
    return true;
  }

  bool modifier(int sym, bool down) {
    if (sym == kOfxKey_Shift_L || sym == kOfxKey_Shift_R) trackedMods().shift = down;
    else if (sym == kOfxKey_Control_L || sym == kOfxKey_Control_R) trackedMods().ctrl = down;
    else if (sym == kOfxKey_Alt_L || sym == kOfxKey_Alt_R || sym == kOfxKey_Meta_L || sym == kOfxKey_Meta_R)
      altTracked_ = down;
    else return false;
    _effect->redrawOverlays();
    return true;
  }
  bool altDown() const {
#ifdef _WIN32
    return (GetKeyState(VK_MENU) & 0x8000) != 0;
#else
    return altTracked_;
#endif
  }

  double constrainX(double x) const { return axis_ == kVertical ? downX_ / effectPar() : x; }
  double constrainY(double y) const { return axis_ == kHorizontal ? downY_ : y; }
  double effectPar() const { return static_cast<PaintEffect*>(_effect)->pixelAspect(); }

  // Pressure from the tablet itself when there is any, otherwise what the host sent
  // (which in Nuke is always 1). A mouse ends up at 1 either way.
  double pressure(const OFX::PenArgs& a) {
    pen::start();
    const double tablet = pen::pressure(effect()->penMode());
    if (tablet >= 0.0) return std::min(1.0, std::max(0.0, tablet));
    const double host = a.penPressure;
    return (host > 0.0 && host <= 1.0) ? host : 1.0;
  }

  rpui::Overlay ui_;
  std::map<uint64_t, rpui::Tex> previewTex_;
  int previewSerial_ = -1;
  uint64_t uploads_ = 0;
  std::chrono::steady_clock::time_point lastRedraw_;
  bool pickHover_ = false;
  bool pickerOpen_ = false, layersOpen_ = true, picking_ = false, overUI_ = false, altTracked_ = false;
  double mv_[16] = {0}, pj_[16] = {0};
  GLint vp_[4] = {0, 0, 0, 0};
  bool haveMatrices_ = false;
  double gridMx_ = 0, gridMy_ = 0;
  int gridCols_ = 6, gridRows_ = 4;

  OfxPointD pos_{0, 0};
  bool hasPos_ = false;
  Mode mode_ = kIdle;
  Axis axis_ = kFree;
  double downX_ = 0, downY_ = 0, startSize_ = 50;
  bool haveLast_ = false;
  double lastX_ = 0, lastY_ = 0;  // where the last stroke ended, in canvas pixels
};

class OverlayDescriptor : public OFX::DefaultEffectOverlayDescriptor<OverlayDescriptor, PaintOverlay> {};

}  // namespace

// ---------------------------------------------------------------------------------
// Description

using namespace OFX;

mDeclarePluginFactory(NPaintShopFactory, {}, {});

void NPaintShopFactory::describe(ImageEffectDescriptor& desc) {
  desc.setLabels("NPaintShop", "NPaintShop", "NPaintShop");
  desc.setPluginGrouping("NPaintShop");
  desc.setPluginDescription(
      "Raster paint with pen pressure, Photoshop brushes and brush dynamics. Strokes are "
      "baked into pixels, so the node stays fast however much you paint.\n\n"
      "In the viewer, with the node's panel open:\n"
      "  drag              paint\n"
      "  Shift+drag        straight stroke, horizontal or vertical\n"
      "  Shift+click       straight line from where the last stroke ended\n"
      "  Ctrl+Shift+drag   resize the brush\n"
      "  Ctrl+click        set the clone source\n"
      "  Alt+B / E / S / U brush, eraser, clone, smudge\n"
      "  Alt+C             pick the colour under the cursor\n"
      "  Alt+[ / Alt+]     smaller / larger brush\n"
      "  Alt+M / Alt+L     brush grid, layer list\n"
      "  Ctrl+Z            undo\n\n"
      "Paint is saved with the script, to sidecar files or inside the script itself "
      "(Settings tab).\n\nBy Rahul Venugopal - www.rkvfx.com. Built using Claude Code.");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(true);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
  desc.setRenderThreadSafety(eRenderFullySafe);
  desc.setOverlayInteractDescriptor(new OverlayDescriptor);
}

namespace {

struct Builder {
  ImageEffectDescriptor& desc;
  PageParamDescriptor* page = nullptr;
  GroupParamDescriptor* tab = nullptr;
  GroupParamDescriptor* group = nullptr;

  template <class P> P* add(P* p, const char* label, const char* hint) {
    p->setLabels(label, label, label);
    if (hint) p->setHint(hint);
    if (group) p->setParent(*group);
    else if (tab) p->setParent(*tab);
    if (page) page->addChild(*p);
    return p;
  }
  // A tab in Nuke's properties panel. Nuke shows OFX pages as one long list, but it
  // turns a top-level group flagged with its "group is tab" extension into a real tab.
  // A host without the extension shows a group instead, which still keeps things tidy.
  void beginTab(const char* name, const char* label, bool open) {
    group = nullptr;
    GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    g->getPropertySet().propSetInt("FnOfxParamPropGroupIsTab", 1, false);
    if (page) page->addChild(*g);
    tab = g;
  }
  void beginGroup(const char* name, const char* label, bool open) {
    GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    if (tab) g->setParent(*tab);
    if (page) page->addChild(*g);
    group = g;
  }
  void endGroup() { group = nullptr; }

  void brushParam(const PDef& d) {
    switch (d.type) {
      case kDouble: {
        DoubleParamDescriptor* p = add(desc.defineDoubleParam(d.name), d.label, d.hint);
        p->setDefault(d.def);
        p->setRange(d.lo, d.hi);
        p->setDisplayRange(d.lo, std::min(d.hi, std::string(d.name) == "size" ? 500.0 : d.hi));
        p->setAnimates(false);
        break;
      }
      case kInt: {
        IntParamDescriptor* p = add(desc.defineIntParam(d.name), d.label, d.hint);
        p->setDefault(int(d.def));
        p->setRange(int(d.lo), int(d.hi));
        p->setDisplayRange(int(d.lo), int(d.hi));
        p->setAnimates(false);
        break;
      }
      case kBool: {
        BooleanParamDescriptor* p = add(desc.defineBooleanParam(d.name), d.label, d.hint);
        p->setDefault(d.def != 0);
        p->setAnimates(false);
        break;
      }
      case kChoice: {
        ChoiceParamDescriptor* p = add(desc.defineChoiceParam(d.name), d.label, d.hint);
        for (const char* const* c = d.choices; *c; ++c) p->appendOption(*c);
        p->setDefault(int(d.def));
        p->setAnimates(false);
        break;
      }
    }
  }
  void brush(const char* name) { brushParam(*findDef(name)); }

  ChoiceParamDescriptor* choice(const char* name, const char* label, const char* const* opts, int def,
                                const char* hint) {
    ChoiceParamDescriptor* p = add(desc.defineChoiceParam(name), label, hint);
    for (const char* const* c = opts; *c; ++c) p->appendOption(*c);
    p->setDefault(def);
    p->setAnimates(false);
    return p;
  }
  PushButtonParamDescriptor* button(const char* name, const char* label, const char* hint) {
    return add(desc.definePushButtonParam(name), label, hint);
  }
  StringParamDescriptor* string(const char* name, const char* label, StringTypeEnum type, const char* hint) {
    StringParamDescriptor* p = add(desc.defineStringParam(name), label, hint);
    p->setStringType(type);
    p->setAnimates(false);
    return p;
  }
  // A read-only line the plugin writes into. Not undoable: a readout changing must
  // never become a step in the user's undo history.
  void status(const char* name, const char* label, const char* def, const char* hint) {
    StringParamDescriptor* p = string(name, label, eStringTypeSingleLine, hint);
    p->setDefault(def);
    p->setEnabled(false);
    p->setEvaluateOnChange(false);
    p->setCanUndo(false);
  }
  // Fixed text, for the shortcuts and about sections.
  void label(const char* name, const char* label, const char* text) {
    StringParamDescriptor* p = string(name, label, eStringTypeLabel, nullptr);
    p->setDefault(text);
    p->setEvaluateOnChange(false);
  }
  StringParamDescriptor* hidden(const char* name, bool undo) {
    StringParamDescriptor* p = desc.defineStringParam(name);
    p->setLabels(name, name, name);
    p->setStringType(eStringTypeSingleLine);
    p->setIsSecret(true);
    p->setAnimates(false);
    p->setEvaluateOnChange(false);
    p->setCanUndo(undo);
    if (page) page->addChild(*p);
    return p;
  }
};

}  // namespace

void NPaintShopFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) {
  ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->setTemporalClipAccess(false);
  src->setSupportsTiles(true);
  src->setOptional(true);
  // A second input to clone from, as on the NDK node. OFX only allows extra inputs in
  // the general context; a host that picks the filter context gets A only.
  if (context == eContextGeneral) {
    ClipDescriptor* bclip = desc.defineClip("B");
    bclip->addSupportedComponent(ePixelComponentRGBA);
    bclip->setTemporalClipAccess(false);
    bclip->setSupportsTiles(true);
    bclip->setOptional(true);
  }
  ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->setSupportsTiles(true);

  Builder b{desc};

  // ---- main tab
  b.page = desc.definePageParam("Controls");
  b.beginTab("tab_paint", "NPaintShop", true);
  b.choice("layer", "layer", kPaintMenu, 0,
           "The layer strokes go onto. Manage the stack on the Layers tab, or with Alt+L in the viewer.");
  {
    BooleanParamDescriptor* v = b.add(desc.defineBooleanParam("layer_visible"), "visible",
                                      "Hide this layer without deleting its paint.");
    v->setDefault(true);
    v->setAnimates(false);
  }
  b.choice("tool", "tool", kTools, 0,
           "paint: lay down colour.  erase: remove paint (never the input).  clone: copy the image "
           "from the clone offset; Ctrl+click in the viewer sets the source.  smudge: drag colour along.");
  {
    RGBParamDescriptor* c = b.add(desc.defineRGBParam("color"), "color", "Brush colour.");
    c->setDefault(1, 1, 1);
    c->setAnimates(false);
  }
  b.brush("brush_blend");
  {
    ChoiceParamDescriptor* p = b.choice("brush_set", "brush set", kNoneMenu, 0,
                                        "An installed brush set (Settings tab). The script stores the set's "
                                        "name, so it keeps working when the .abr moves or sits elsewhere on "
                                        "another machine.");
    (void)p;
  }
  b.choice("brush", "brush", kRoundMenu, 0,
           "'round' is the built-in brush (size + hardness). Brushes from the brush set or brush file "
           "are listed after it.");
  {
    BooleanParamDescriptor* p = b.add(desc.defineBooleanParam("use_file_settings"), "use brush file settings",
                                      "Picking a brush from the file also takes its size, spacing, hardness, "
                                      "angle, roundness and dynamics. Turn off to keep your own settings.");
    p->setDefault(true);
    p->setAnimates(false);
  }
  for (const char* n : {"size", "hardness", "opacity", "flow", "spacing", "smoothing", "pressure_size", "pressure_flow"})
    b.brush(n);
  b.choice("clone_input", "clone from", kCloneInputs, 0,
           "Which image the clone tool copies from. 'B' uses the second input, so a cleanplate or "
           "another element can be painted in. Falls back to A when nothing is connected to B.");
  {
    Double2DParamDescriptor* o = b.add(desc.defineDouble2DParam("clone_offset"), "clone offset",
                                       "Clone source relative to the brush, in pixels. Set by Ctrl+click, then painting.");
    o->setDefault(50, 0);
    o->setAnimates(false);
    o->setUseHostNativeOverlayHandle(false);
    DoubleParamDescriptor* t = b.add(desc.defineDoubleParam("clone_time"), "clone time offset",
                                     "Frames to offset the clone source by, so a rig can be painted out "
                                     "using a frame where it is not there. -1 clones from the previous frame.");
    t->setDefault(0);
    t->setRange(-1000, 1000);
    t->setDisplayRange(-24, 24);
    t->setAnimates(false);
    DoubleParamDescriptor* ss = b.add(desc.defineDoubleParam("smudge_strength"), "smudge strength",
                                      "How much of the carried colour the smudge brush lays down on each dab.");
    ss->setDefault(0.6);
    ss->setRange(0, 1);
    ss->setDisplayRange(0, 1);
    ss->setAnimates(false);
    DoubleParamDescriptor* sr = b.add(desc.defineDoubleParam("smudge_rate"), "smudge pickup",
                                      "How quickly the smudge brush replaces the colour it carries with what it "
                                      "passes over. 0 drags the starting colour the whole way; 1 barely moves anything.");
    sr->setDefault(0.35);
    sr->setRange(0, 1);
    sr->setDisplayRange(0, 1);
    sr->setAnimates(false);
  }
  b.choice("preset", "preset", kNoneMenu, 0,
           "Saved brush settings, shared with the NDK version of NPaintShop. Picking one applies it. "
           "Save and delete presets on the Settings tab.");
  b.button("brush_reset", "Reset brush", "Puts every brush and dynamics setting back to its default.");
  b.button("undo_paint", "Undo", "Undoes the last stroke or layer change. Ctrl+Z does the same.");
  b.button("redo_paint", "Redo", "Redoes what Undo took back.");
  b.button("clear_paint", "Clear frame", "Removes all paint on the current frame (undoable).");
  b.choice("lifetime", "lifetime", kLifetime, 0,
           "all frames: one painted layer shown on every frame.\nsingle frame: separate paint per frame.");
  b.choice("paint_channels", "paint into", kPaintChannels, 0,
           "Which channels strokes affect. 'rgb' leaves the input alpha untouched; 'alpha' paints a matte.");
  b.choice("output", "output", kOutput, 0,
           "paint over input: the normal result.\npaint only: the paint alone on transparent black, "
           "premultiplied, with alpha - write it out and Merge (over) it onto the plate on a machine "
           "without NPaintShop.");
  {
    // One per stroke; the host's undo puts it back, and the plugin follows.
    IntParamDescriptor* h = b.add(desc.defineIntParam("history"), "strokes",
                                  "Strokes applied. Ctrl+Z and Ctrl+Shift+Z move this.");
    h->setDefault(0);
    h->setEnabled(false);
    h->setAnimates(false);
    h->setEvaluateOnChange(false);
  }

  // ---- layers tab
  b.beginTab("tab_layers", "Layers", false);
  b.string("layer_name", "name", eStringTypeSingleLine, "Renames the selected layer.");
  {
    DoubleParamDescriptor* o = b.add(desc.defineDoubleParam("layer_opacity"), "opacity", "Opacity of the selected layer.");
    o->setDefault(1);
    o->setRange(0, 1);
    o->setDisplayRange(0, 1);
    o->setAnimates(false);
  }
  b.choice("layer_blend", "blend", rp::blendNames(), 0,
           "How the selected layer combines with everything below it. Normal, multiply, screen, plus, "
           "minus, difference, darken and lighten are safe on over-range values; the others assume 0..1.");
  {
    BooleanParamDescriptor* l = b.add(desc.defineBooleanParam("layer_locked"), "locked",
                                      "Stops strokes landing on the selected layer by accident.");
    l->setDefault(false);
    l->setAnimates(false);
  }
  b.button("layer_add", "Add", "Adds an empty layer above the selected one.");
  b.button("layer_duplicate", "Duplicate", "Copies the selected layer, pixels included.");
  b.button("layer_delete", "Delete", "Deletes the selected layer and its paint. A node always keeps one.");
  b.button("layer_up", "Move up", "Moves the selected layer up the stack.");
  b.button("layer_down", "Move down", "Moves the selected layer down the stack.");
  b.button("layer_merge", "Merge down", "Composites the selected layer into the one below, on every frame.");
  b.button("layer_flatten", "Flatten visible", "Merges every visible layer into the bottom one.");
  b.label("layer_note", "", "Layer changes undo with Ctrl+Z, like strokes.");

  // ---- dynamics tab
  b.beginTab("tab_dynamics", "Dynamics", false);
  {
    RGBParamDescriptor* c = desc.defineRGBParam("bg_color");
    c->setLabels("background colour", "background colour", "background colour");
    c->setHint("The colour fg/bg jitter mixes towards.");
    c->setDefault(0, 0, 0);
    c->setAnimates(false);
    const char* lastGroup = nullptr;
    int gi = 0;
    for (const PDef& d : kBrush) {
      if (d.page != kDyn) continue;
      if (d.group != lastGroup) {
        b.endGroup();
        if (d.group) {
          char gname[16];
          std::snprintf(gname, sizeof(gname), "dyn_group%d", gi++);
          b.beginGroup(gname, d.group, false);
          if (std::string(d.group) == "colour dynamics") {
            c->setParent(*b.group);
            b.page->addChild(*c);
          }
        }
        lastGroup = d.group;
      }
      b.brushParam(d);
    }
    b.endGroup();
  }

  // ---- settings tab
  b.beginTab("tab_settings", "Settings", false);
  b.beginGroup("brush_library", "brush library", true);
  {
    StringParamDescriptor* f = b.string("abr_file", "brush file", eStringTypeFilePath,
                                        "Photoshop brush file (.abr). Use it where it sits, or press Install "
                                        "to copy it into your brush library so scripts stop depending on this path.");
    f->setFilePathExists(true);
  }
  b.button("brush_install", "Install", "Copy the brush file into your brush library and select it.");
  b.button("brush_remove", "Remove", "Delete the selected brush set from your own library.");
  b.endGroup();

  b.beginGroup("presets_group", "brush presets", true);
  b.string("preset_name", "name", eStringTypeSingleLine, "Name to save the current brush settings under.");
  b.button("preset_save", "Save preset", nullptr);
  b.button("preset_delete", "Delete preset", "Deletes the preset picked on the main tab.");
  b.endGroup();

  b.beginGroup("storage_group", "storage", true);
  b.choice("storage", "store paint", kStorage, 0,
           "sidecar files: paint goes to files in the paint directory, keeping scripts small. Copy that "
           "folder along with the script.\ninside the script: paint is written into the .nk itself. One "
           "file to move around, but large paint makes the script large and slower to save and open.");
  {
    StringParamDescriptor* d = b.string("paint_dir", "paint directory", eStringTypeDirectoryPath,
                                        "Where sidecar files go. Empty uses ~/.nuke/NPaintShop/paint "
                                        "(or NPAINTSHOP_PAINT_DIR). Each node gets its own folder inside.");
    d->setFilePathExists(false);
  }
  b.status("storage_status", "saving to", "", "Where this node's paint is saved, and whether all of it is.");
  b.button("save_paint", "Save paint now",
           "Writes unsaved paint immediately. It is also written whenever the script is saved.");
  b.button("reload_paint", "Reload from disk",
           "Discards unsaved paint and the undo history, and reloads the saved paint.");
  b.endGroup();

  b.beginGroup("memory_group", "memory", true);
  {
    IntParamDescriptor* u = b.add(desc.defineIntParam("undo_memory"), "undo memory",
                                  "Megabytes of undo history to keep. Each undo step holds a copy of the "
                                  "tiles its stroke changed, 256 KB apiece. The oldest steps are dropped "
                                  "first, and the most recent stroke can always be undone. Set "
                                  "NPAINTSHOP_UNDO_MB to change the default.");
    u->setDefault(envMB("NPAINTSHOP_UNDO_MB", 1024));
    u->setRange(64, 65536);
    u->setDisplayRange(64, 8192);
    u->setAnimates(false);
    u->setEvaluateOnChange(false);
    IntParamDescriptor* c = b.add(desc.defineIntParam("clone_cache"), "clone cache",
                                  "Megabytes of input image kept in memory for clone, smudge, the live "
                                  "preview and colour picking. Each 128x128 tile costs 272 KB, so the "
                                  "default holds about 1900 tiles - plenty for HD or 4K. Set "
                                  "NPAINTSHOP_CLONE_CACHE_MB to change the default.");
    c->setDefault(envMB("NPAINTSHOP_CLONE_CACHE_MB", 512));
    c->setRange(128, 65536);
    c->setDisplayRange(128, 8192);
    c->setAnimates(false);
    c->setEvaluateOnChange(false);
    IntParamDescriptor* f = b.add(desc.defineIntParam("frame_memory"), "other frames",
                                  "Megabytes of paint to keep in memory for frames other than the one you "
                                  "are on. Past this, the frames furthest from the current one are released; "
                                  "with sidecar files, unsaved ones are written first, so nothing is lost. "
                                  "0 never releases unsaved frames. Only matters with lifetime set to "
                                  "'single frame'. Set NPAINTSHOP_FRAME_MB to change the default.");
    f->setDefault(envMB("NPAINTSHOP_FRAME_MB", 2048));
    f->setRange(0, 65536);
    f->setDisplayRange(0, 16384);
    f->setAnimates(false);
    f->setEvaluateOnChange(false);
  }
  b.status("memory_status", "in use", "",
           "Memory this node is using right now, updated after each stroke.\npaint: your painted layers. "
           "This is your work, not a cache.\nundo: undo history, against its limit above.\nclone cache: "
           "input image kept for clone, preview and colour picking, against its limit above.");
  b.button("free_memory", "Free memory",
           "Empties the clone cache, discards this node's undo history, and saves and releases paint on "
           "every frame except the current one. Nothing painted is lost, but strokes made before this can "
           "no longer be undone.");
  b.endGroup();

  b.beginGroup("performance_group", "performance", true);
  {
    IntParamDescriptor* r = b.add(desc.defineIntParam("refresh_ms"), "refresh every",
                                  "Milliseconds between viewer refreshes after a stroke changes the image. "
                                  "Raise it if painting on a huge image still feels heavy.");
    r->setDefault(40);
    r->setRange(10, 1000);
    r->setDisplayRange(10, 250);
    r->setAnimates(false);
    r->setEvaluateOnChange(false);
  }
  b.status("stroke_status", "last stroke", "",
           "How hard the brush had to work on the last stroke: the time spent painting against how long "
           "the stroke took. Well under half means the brush kept up with your hand. Near the whole "
           "stroke means the brush itself is the bottleneck: a smaller brush or larger spacing will help.");
  b.choice("preview_colour", "preview colour", kPreviewColour, 0,
           "While the pen is down Nuke does not update the viewer, so NPaintShop draws the stroke "
           "itself until the real render arrives. Set this to match your viewer's colour transform "
           "so the stroke does not change shade when you lift the pen.");
  b.endGroup();

  b.beginGroup("tablet_group", "tablet", true);
  b.choice("pen_pressure", "pen pressure", kPenSource, 0,
           "Where pen pressure is read from. Automatic uses Windows Ink when the tablet sends it and "
           "Wintab (Wacom and most others) otherwise. Pick one if automatic chooses wrong.");
  b.status("pen_status", "pressure", "paint a stroke with the pen to check", "What the last stroke received.");
  b.endGroup();

  // Raw numbers, for bug reports. Closed by default so they do not clutter the panel.
  b.beginGroup("diagnostics_group", "details for bug reports", false);
  b.status("stroke_details", "stroke", "",
           "The last stroke in full: pen moves, time, how much was painting, how many viewer updates "
           "were asked for and arrived, and the live preview. Please copy this into bug reports about speed.");
  b.status("clone_status", "clone source", "",
           "What the last clone or smudge stroke found behind the brush. Pixels with no source are left "
           "untouched rather than painted.");
  b.endGroup();

  b.beginGroup("shortcuts_group", "shortcuts", false);
  // Key in the label column, action beside it, as in the NDK node's list. Nuke shows
  // an OFX label-type string as its text alone, so these are read-only text fields.
  // Named k3_* so values saved by earlier betas cannot override the current text.
  auto key = [&](const char* name, const char* keys, const char* what) {
    StringParamDescriptor* p = b.string(name, keys, eStringTypeSingleLine, nullptr);
    p->setDefault(what);
    p->setEnabled(false);
    p->setEvaluateOnChange(false);
    p->setCanUndo(false);
  };
  key("k3_alt_b", "Alt+B", "brush");
  key("k3_alt_e", "Alt+E", "erase");
  key("k3_alt_s", "Alt+S", "clone stamp");
  key("k3_alt_u", "Alt+U", "smudge");
  key("k3_alt_c", "Alt+C", "pick colour (keep Alt held and move to keep picking)");
  key("k3_alt_lb", "Alt+[", "smaller brush");
  key("k3_alt_rb", "Alt+]", "larger brush");
  key("k3_alt_m", "Alt+M", "brush menu");
  key("k3_alt_l", "Alt+L", "layer list");
  key("k3_drag", "Drag", "paint");
  key("k3_ctrl_click", "Ctrl+click", "set the clone source");
  key("k3_shift_drag", "Shift+drag", "straight stroke, horizontal or vertical");
  key("k3_shift_click", "Shift+click", "straight line from the end of the last stroke");
  key("k3_resize", "Ctrl+Shift+drag", "resize the brush");
  key("k3_undo", "Ctrl+Z", "undo (Ctrl+Shift+Z redo)");
  b.endGroup();

  b.beginGroup("about_group", "about", false);
  b.label("about_author", "author", "Rahul Venugopal");
  b.label("about_site", "website", "www.rkvfx.com");
  b.label("about_build", "build", "OFX 0.9 beta, " __DATE__);
  b.label("about_claude", "", "Built using Claude Code");
  b.endGroup();

  // ---- hidden
  b.hidden("brush_set_name", true);  // the set by name, so menu order never matters
  b.hidden("paint_id", false);
  b.hidden("paint_data", false);
  b.hidden("layer_data", false);  // the layer stack's names and settings
  {
    // Changed by the plugin to make the host re-render. Not undoable, or Ctrl+Z would
    // step through dozens of invisible refreshes before reaching a stroke.
    IntParamDescriptor* rev = desc.defineIntParam("paint_rev");
    rev->setLabels("paint revision", "paint revision", "paint revision");
    rev->setDefault(0);
    rev->setIsSecret(true);
    rev->setAnimates(false);
    rev->setCanUndo(false);
    b.page->addChild(*rev);
  }
}

ImageEffect* NPaintShopFactory::createInstance(OfxImageEffectHandle h, ContextEnum) {
  return new PaintEffect(h);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
  static NPaintShopFactory p(kPluginId, 1, 0);
  ids.push_back(&p);
}
}  // namespace Plugin
}  // namespace OFX
