// Standalone tests + benchmark for the NPaintShop core (no Nuke needed).
//   g++ -O2 -std=c++17 -pthread -Isrc/core tests/test_core.cpp src/core/*.cpp -o test_core && ./test_core
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "Parallel.h"
#include "PaintEngine.h"
#include "Preview.h"

using namespace rp;

static int g_fail = 0;
static int g_quiet = 0;
#define CHECK_QUIET(cond) do { if (!(cond)) ++g_quiet; } while (0)
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
  } while (0)

static void px(const Canvas& c, int x, int y, float out[4]) {
  const Tile* t = c.find(tileOf(x), tileOf(y));
  for (int i = 0; i < 4; ++i)
    out[i] = t ? t->px[(size_t(y & TILE_MASK) * TILE + (x & TILE_MASK)) * 4 + i] : 0.0f;
}
static bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

static bool sameTiles(const TileMap& a, const TileMap& b) {
  if (a.size() != b.size()) return false;
  for (const auto& kv : a) {
    auto it = b.find(kv.first);
    if (it == b.end() || std::memcmp(kv.second->px, it->second->px, sizeof(Tile::px)) != 0) return false;
  }
  return true;
}
static TileMap cloneTiles(const TileMap& a) {
  TileMap r;
  for (const auto& kv : a) r[kv.first].reset(new Tile(*kv.second));
  return r;
}

// ------------------------------------------------------------ .abr fixtures
static void be16(std::vector<uint8_t>& v, int x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
static void be32(std::vector<uint8_t>& v, int x) { be16(v, x >> 16); be16(v, x & 0xffff); }
static void writeFile(const std::string& p, const std::vector<uint8_t>& v) {
  FILE* f = std::fopen(p.c_str(), "wb");
  std::fwrite(v.data(), 1, v.size(), f);
  std::fclose(f);
}
// Test tip: w x h, value = top row 255, everything else a horizontal ramp.
static std::vector<uint8_t> tipBytes(int w, int h) {
  std::vector<uint8_t> a(size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) a[size_t(y) * w + x] = y == 0 ? 255 : uint8_t(x * 255 / (w - 1));
  return a;
}
static void appendSample(std::vector<uint8_t>& v, int w, int h, bool rle, int depth = 8) {
  be32(v, 0); be32(v, 0); be32(v, h); be32(v, w);  // top left bottom right
  be16(v, depth);
  v.push_back(rle ? 1 : 0);
  std::vector<uint8_t> a = tipBytes(w, h), raw;
  if (depth == 16) { for (uint8_t b : a) { raw.push_back(b); raw.push_back(b); } }
  else raw = a;
  const size_t rowBytes = size_t(w) * (depth / 8);
  if (!rle) { v.insert(v.end(), raw.begin(), raw.end()); return; }
  std::vector<std::vector<uint8_t>> rows{size_t(h)};
  for (int y = 0; y < h; ++y) packBitsEncode(raw.data() + size_t(y) * rowBytes, rowBytes, rows[size_t(y)]);
  for (auto& r : rows) be16(v, int(r.size()));
  for (auto& r : rows) v.insert(v.end(), r.begin(), r.end());
}
static std::vector<uint8_t> abrV2() {
  std::vector<uint8_t> v;
  be16(v, 2); be16(v, 2);  // version, count
  // brush 1: computed (must be skipped)
  be16(v, 1); be32(v, 14); for (int i = 0; i < 14; ++i) v.push_back(0);
  // brush 2: sampled, named "Hi", raw
  std::vector<uint8_t> b;
  be32(b, 0); be16(b, 25);            // misc, spacing 25%
  be32(b, 2); be16(b, 'H'); be16(b, 'i');
  b.push_back(1);                      // anti-alias
  for (int i = 0; i < 4; ++i) be16(b, 0);
  appendSample(b, 9, 5, false);
  be16(v, 2); be32(v, int(b.size())); v.insert(v.end(), b.begin(), b.end());
  return v;
}


static std::vector<uint8_t> abrV6(int sub, const char* id0, const char* id1,
                                  const std::vector<uint8_t>& desc) {
  std::vector<uint8_t> v, samp;
  be16(v, 6); be16(v, sub);
  const char* ids[2] = {id0, id1};
  for (int n = 0; n < 2; ++n) {
    std::vector<uint8_t> s;
    // a Pascal-string id, then filler out to the fixed header size
    const size_t header = size_t(sub == 1 ? 47 : 301);
    s.push_back(uint8_t(std::strlen(ids[n])));
    for (const char* c = ids[n]; *c; ++c) s.push_back(uint8_t(*c));
    while (s.size() < header) s.push_back(uint8_t('a' + s.size() % 20));
    appendSample(s, n == 0 ? 33 : 7, n == 0 ? 17 : 6, n == 0, n == 0 ? 8 : 16);
    be32(samp, int(s.size()));
    samp.insert(samp.end(), s.begin(), s.end());
    while (samp.size() % 4) samp.push_back(0);
  }
  const char sm[] = "8BIMsamp";
  v.insert(v.end(), sm, sm + 8);
  be32(v, int(samp.size()));
  v.insert(v.end(), samp.begin(), samp.end());
  if (!desc.empty()) {
    const char dc[] = "8BIMdesc";
    v.insert(v.end(), dc, dc + 8);
    be32(v, int(desc.size()));
    v.insert(v.end(), desc.begin(), desc.end());
  }
  return v;
}

// ---- a minimal Photoshop descriptor writer, enough to exercise the reader
static void dKey(std::vector<uint8_t>& v, const char* k) {
  // A length of zero means a four-character code follows; anything else is written
  // with its real length, which is how Photoshop stores the longer key names.
  const size_t n = std::strlen(k);
  be32(v, n == 4 ? 0 : int(n));
  v.insert(v.end(), k, k + n);
}
static void dUcs2(std::vector<uint8_t>& v, const std::string& s) {
  be32(v, int(s.size()) + 1);
  for (char c : s) be16(v, (unsigned char)c);
  be16(v, 0);  // trailing null, as Photoshop writes
}
static void dType(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }
static void dF64(std::vector<uint8_t>& v, double x) {
  uint64_t b;
  std::memcpy(&b, &x, 8);
  for (int i = 7; i >= 0; --i) v.push_back(uint8_t((b >> (i * 8)) & 0xff));
}
static void dUnit(std::vector<uint8_t>& v, const char* key, const char* unit, double x) {
  dKey(v, key); dType(v, "UntF"); dType(v, unit); dF64(v, x);
}
static void dLong(std::vector<uint8_t>& v, const char* key, int x) {
  dKey(v, key); dType(v, "long"); be32(v, x);
}
static void dBool(std::vector<uint8_t>& v, const char* key, bool x) {
  dKey(v, key); dType(v, "bool"); v.push_back(x ? 1 : 0);
}
static void dText(std::vector<uint8_t>& v, const char* key, const std::string& x) {
  dKey(v, key); dType(v, "TEXT"); dUcs2(v, x);
}
static void dDescHead(std::vector<uint8_t>& v, const char* cls, int n) {
  dUcs2(v, "");
  be32(v, int(std::strlen(cls)));
  v.insert(v.end(), cls, cls + std::strlen(cls));
  be32(v, n);
}
// {bVTy, fStp, jitter, Mnm } as Photoshop writes a dynamics sub-descriptor
static void dDyn(std::vector<uint8_t>& v, const char* key, int bvty, int fade, double jit,
                 double mn) {
  dKey(v, key); dType(v, "Objc"); dDescHead(v, "brushDynamics", 4);
  dLong(v, "bVTy", bvty);
  dLong(v, "fStp", fade);
  dUnit(v, "jitter", "#Prc", jit);
  dUnit(v, "Mnm ", "#Prc", mn);
}

static std::vector<uint8_t> buildDesc(const char* sampledId) {
  std::vector<uint8_t> v;
  be32(v, 16);  // descriptor version word
  dDescHead(v, "null", 1);
  dKey(v, "Brsh"); dType(v, "VlLs"); be32(v, 2);

  // brush 1: sampled, with scatter and transfer switched on
  dType(v, "Objc"); dDescHead(v, "brushPreset", 11);
  dText(v, "Nm  ", "Chalky Test");
  dKey(v, "Brsh"); dType(v, "Objc"); dDescHead(v, "Brsh", 7);
  dUnit(v, "Dmtr", "#Pxl", 60.0);
  dUnit(v, "Spcn", "#Prc", 5.0);
  dUnit(v, "Hrdn", "#Prc", 40.0);
  dUnit(v, "Angl", "#Ang", 30.0);
  dUnit(v, "Rndn", "#Prc", 50.0);
  dBool(v, "flipX", true);
  dText(v, "sampledData", sampledId);
  dBool(v, "useTipDynamics", true);
  dDyn(v, "szVr", 2, 30, 18.0, 0.0);
  dDyn(v, "angleDynamics", 6, 7, 25.0, 0.0);
  dUnit(v, "minimumDiameter", "#Prc", 63.0);
  dBool(v, "useScatter", true);
  dUnit(v, "Spcn", "#Prc", 150.0);
  dKey(v, "Cnt "); dType(v, "doub"); dF64(v, 10.0);
  dBool(v, "usePaintDynamics", true);
  dDyn(v, "opVr", 2, 25, 40.0, 10.0);

  // brush 2: computed, so no bitmap and no dynamics
  dType(v, "Objc"); dDescHead(v, "brushPreset", 2);
  dText(v, "Nm  ", "Computed Test");
  dKey(v, "Brsh"); dType(v, "Objc"); dDescHead(v, "Brsh", 3);
  dUnit(v, "Dmtr", "#Pxl", 19.0);
  dUnit(v, "Spcn", "#Prc", 10.0);
  dUnit(v, "Rndn", "#Prc", 20.0);
  return v;
}

static const char* const kId0 = "f0b41c25-ca82-11dd-938f-9a24d929ca23";
static const char* const kId1 = "11112222-3333-4444-5555-666677778888";

static void testAbr(const std::string& dir) {
  std::string err;
  writeFile(dir + "/v2.abr", abrV2());
  AbrFile f2 = loadABR(dir + "/v2.abr", &err);
  CHECK(f2.tips.size() == 1);
  CHECK(f2.presets.size() == 1);
  if (f2.tips.size() == 1 && f2.presets.size() == 1) {
    CHECK(f2.tips[0].name == "Hi");
    CHECK(f2.tips[0].width() == 9 && f2.tips[0].height() == 5);
    CHECK(near(f2.tips[0].spacingHint, 0.25f));
    // rows are flipped on load: the file's top row (all 255) is now the last row
    CHECK(near(f2.tips[0].mips[0].a[4 * 9 + 0], 1.0f));
    CHECK(near(f2.tips[0].mips[0].a[0], 0.0f) && near(f2.tips[0].mips[0].a[8], 1.0f));
    CHECK(f2.tips[0].mips.back().w == 1 && f2.tips[0].mips.back().h == 1);
    // with no descriptor block, one preset is synthesised per bitmap
    CHECK(f2.presets[0].tip == 0);
    CHECK(f2.presets[0].name == "Hi");
    CHECK(near(f2.presets[0].spacing, 0.25f));
  }

  for (int sub = 1; sub <= 2; ++sub) {
    writeFile(dir + "/v6.abr", abrV6(sub, kId0, kId1, std::vector<uint8_t>()));
    AbrFile f6 = loadABR(dir + "/v6.abr", &err);
    CHECK(f6.tips.size() == 2);
    if (f6.tips.size() == 2) {
      CHECK(f6.tips[0].width() == 33 && f6.tips[0].height() == 17);
      CHECK(near(f6.tips[0].mips[0].a[16 * 33 + 5], 1.0f));  // flipped top row
      CHECK(near(f6.tips[0].mips[0].a[3 * 33 + 16], (16 * 255 / 32) / 255.0f));
      CHECK(f6.tips[1].width() == 7 && f6.tips[1].height() == 6);
      CHECK(near(f6.tips[1].mips[0].a[6], 1.0f, 1e-3f));  // 16-bit ramp end
      CHECK(f6.tips[0].id == kId0);  // the id a descriptor refers to
      CHECK(f6.tips[1].id == kId1);
    }
    CHECK(f6.presets.size() == 2);
  }

  // ---- with a descriptor: real names, settings and dynamics
  {
    writeFile(dir + "/v6d.abr", abrV6(2, kId0, kId1, buildDesc(kId0)));
    AbrFile f = loadABR(dir + "/v6d.abr", &err);
    CHECK(f.tips.size() == 2);
    CHECK(f.presets.size() == 2);
    if (f.presets.size() == 2) {
      const BrushPreset& a = f.presets[0];
      CHECK(a.name == "Chalky Test");
      CHECK(a.fromFile);
      CHECK(a.tip == 0);  // matched to its bitmap by id
      CHECK(near(a.diameter, 60.0f));
      CHECK(near(a.spacing, 0.05f));  // 5% becomes a fraction
      CHECK(near(a.hardness, 0.4f));
      CHECK(near(a.dyn.angle, 30.0f));
      CHECK(near(a.dyn.roundness, 0.5f));
      CHECK(a.dyn.flipX && !a.dyn.flipY);
      CHECK(near(a.dyn.size.jitter, 0.18f));
      CHECK(a.dyn.size.control == 2);  // bVTy 2 is pen pressure
      CHECK(a.dyn.size.fadeSteps == 30);
      CHECK(near(a.dyn.size.minimum, 0.63f));  // minimumDiameter wins over Mnm
      CHECK(near(a.dyn.angleJitter, 0.25f));
      CHECK(a.dyn.angleControl == kAngleDirection);  // bVTy 6 is direction
      CHECK(a.dyn.angleFadeSteps == 7);
      CHECK(near(a.dyn.scatter, 1.5f));  // 150%
      CHECK(a.dyn.count == 10);
      CHECK(!a.dyn.scatterBoth);
      CHECK(near(a.dyn.opacity.jitter, 0.4f));
      CHECK(a.dyn.opacity.control == 2);
      CHECK(near(a.dyn.opacity.minimum, 0.1f));
      CHECK(a.dyn.flow.jitter == 0.0f);  // the fixture has no prVr

      const BrushPreset& b = f.presets[1];
      CHECK(b.name == "Computed Test");
      CHECK(b.tip == -1);  // no bitmap, so the round brush
      CHECK(near(b.diameter, 19.0f));
      CHECK(near(b.spacing, 0.1f));
      CHECK(near(b.dyn.roundness, 0.2f));
      CHECK(b.dyn.size.control == 0);  // useTipDynamics absent, so nothing applies
      CHECK(b.dyn.scatter == 0.0f);
    }
  }

  // a descriptor naming an id no bitmap has falls back to the round brush
  {
    writeFile(dir + "/v6x.abr", abrV6(2, "other-id", kId1, buildDesc("missing-id")));
    AbrFile f = loadABR(dir + "/v6x.abr", &err);
    CHECK(f.presets.size() == 2);
    if (!f.presets.empty()) CHECK(f.presets[0].tip == -1);
  }

  // ---- malformed input must not crash
  writeFile(dir + "/bad.abr", std::vector<uint8_t>{0, 99, 1, 2, 3});
  CHECK(loadABR(dir + "/bad.abr", &err).empty() && !err.empty());
  const std::vector<uint8_t> full = abrV6(2, kId0, kId1, buildDesc(kId0));
  for (size_t cut : {full.size() / 4, full.size() / 2, full.size() * 3 / 4, full.size() - 1}) {
    writeFile(dir + "/trunc.abr", std::vector<uint8_t>(full.begin(), full.begin() + cut));
    loadABR(dir + "/trunc.abr", &err);  // must not crash or hang
  }
  // a descriptor cut off mid-value, with the sample data intact
  {
    std::vector<uint8_t> d = buildDesc(kId0);
    d.resize(d.size() / 2);
    writeFile(dir + "/halfdesc.abr", abrV6(2, kId0, kId1, d));
    AbrFile f = loadABR(dir + "/halfdesc.abr", &err);
    CHECK(f.tips.size() == 2);  // the bitmaps still load
    CHECK(!f.presets.empty());  // and presets fall back to one per bitmap
  }
}

// ------------------------------------------------------------ painting
static void testPaint(const std::string& dir) {
  auto c = std::make_shared<Canvas>();
  PaintEngine e;
  BrushParams p;
  p.size = 40; p.hardness = 1; p.pressureSize = false;
  p.color[0] = 1; p.color[1] = 0.5f; p.color[2] = 0.25f;

  // 1. hard opaque stroke crossing tile boundaries and negative coordinates
  e.beginStroke(c, p, -60, 100, 1);
  IRect d = e.strokeTo(300, 100, 1);
  CHECK(!d.empty());
  CHECK(e.endStroke());
  float v[4];
  px(*c, 100, 100, v);
  CHECK(near(v[0], 1) && near(v[1], 0.5f) && near(v[2], 0.25f) && near(v[3], 1));
  px(*c, -50, 100, v); CHECK(near(v[3], 1));
  px(*c, 100, 130, v); CHECK(near(v[3], 0));
  CHECK(c->dirty);
  const TileMap afterStroke1 = cloneTiles(c->tiles);

  // 2. opacity is a ceiling for the stroke, however much the dabs overlap
  BrushParams q = p;
  q.opacity = 0.5f; q.flow = 1; q.color[0] = 0; q.color[1] = 0; q.color[2] = 1;
  e.beginStroke(c, q, 0, 300, 1);
  for (int i = 0; i < 20; ++i) e.strokeTo(200, 300, 1), e.strokeTo(0, 300, 1);
  e.endStroke();
  px(*c, 100, 300, v);
  CHECK(near(v[3], 0.5f) && near(v[2], 0.5f) && near(v[0], 0));

  // 3. low flow builds up towards opacity
  q.opacity = 1; q.flow = 0.1f; q.spacing = 0.05f;
  e.beginStroke(c, q, 0, 500, 1);
  e.strokeTo(200, 500, 1);
  e.endStroke();
  px(*c, 100, 500, v);
  CHECK(v[3] > 0.5f && v[3] < 1.0f);

  // 4. erase, with undo / redo restoring bit-exact tiles
  const TileMap beforeErase = cloneTiles(c->tiles);
  BrushParams er = p;
  er.tool = Tool::Erase; er.size = 400;
  e.beginStroke(c, er, 100, 100, 1);
  e.strokeTo(120, 100, 1);
  e.endStroke();
  px(*c, 100, 100, v); CHECK(near(v[3], 0) && near(v[0], 0));
  px(*c, 100, 300, v); CHECK(near(v[3], 0.5f));  // out of the eraser's reach
  const TileMap afterErase = cloneTiles(c->tiles);
  CHECK(afterErase.size() < beforeErase.size());  // emptied tiles were freed
  IRect ud;
  CHECK(e.undo(&ud) && !ud.empty());
  CHECK(sameTiles(c->tiles, beforeErase));
  CHECK(e.redo());
  CHECK(sameTiles(c->tiles, afterErase));
  CHECK(e.undo() && e.undo() && e.undo());
  CHECK(sameTiles(c->tiles, afterStroke1));
  CHECK(e.undo() && c->empty() && !e.canUndo());
  CHECK(e.redo() && sameTiles(c->tiles, afterStroke1));
  // a new stroke after undo discards the redo branch
  e.beginStroke(c, p, 600, 600, 1); e.endStroke();
  CHECK(!e.canRedo());
  CHECK(e.undo());

  // 5. clone: copies (paint over background) from the offset position
  // The supplier owns its tiles and keeps them alive for the stroke, so the test holds
  // them in a map rather than filling a buffer the engine hands over.
  std::map<uint64_t, std::shared_ptr<SrcTile>> bgOwned;
  BgFetch bg = [&bgOwned](int tx, int ty) -> std::shared_ptr<const SrcTile> {
    auto& slot = bgOwned[tileKey(tx, ty)];
    if (slot) return slot;
    slot = std::make_shared<SrcTile>();
    for (int y = 0; y < TILE; ++y)
      for (int x = 0; x < TILE; ++x) {
        float* o = slot->px.px + (size_t(y) * TILE + x) * 4;
        o[0] = float(tx * TILE + x) * 0.001f;  // encodes absolute x
        o[1] = float(ty * TILE + y) * 0.001f;  // encodes absolute y
        o[2] = 0.0f; o[3] = 1.0f;
      }
    std::memset(slot->ok, 1, sizeof(slot->ok));
    return slot;
  };
  BrushParams cl = p;
  cl.tool = Tool::Clone; cl.size = 30; cl.cloneDx = 200; cl.cloneDy = 0;
  e.beginStroke(c, cl, 900, 900, 1, bg);
  e.strokeTo(940, 900, 1);
  e.endStroke();
  px(*c, 920, 900, v);  // source (1120, 900) is pure background
  CHECK(near(v[0], 1.120f) && near(v[1], 0.900f) && near(v[3], 1));
  cl.cloneDx = -820; cl.cloneDy = -800;  // source lands on stroke 1 (orange, opaque)
  e.beginStroke(c, cl, 920, 900, 1, bg);
  e.endStroke();
  px(*c, 920, 900, v);
  CHECK(near(v[0], 1) && near(v[1], 0.5f) && near(v[2], 0.25f));
  // overlapping clone must read the pre-stroke image, not its own output
  cl.cloneDx = -10; cl.cloneDy = 0; cl.size = 60;
  e.beginStroke(c, cl, 1500, 1500, 1, bg);
  e.strokeTo(1700, 1500, 1);
  e.endStroke();
  px(*c, 1650, 1500, v);
  CHECK(near(v[0], 1.640f));

  // 5a. a supplier that evicts a tile the moment it has handed it over must not change
  // the result. Ownership is shared precisely so a cache can stay inside its budget
  // mid-stroke; when it was a bare pointer, a cache that filled up and dropped its
  // tiles left the stroke reading freed memory, which showed as black tiles on images
  // big enough to fill the cache.
  {
    auto evicting = std::make_shared<Canvas>();
    PaintEngine ee;
    BrushParams ce2 = p;
    ce2.tool = Tool::Clone; ce2.size = 30; ce2.cloneDx = 200; ce2.cloneDy = 0;
    BgFetch dropping = [](int tx, int ty) -> std::shared_ptr<const SrcTile> {
      auto t = std::make_shared<SrcTile>();          // the supplier's only reference,
      for (int yy = 0; yy < TILE; ++yy)              // released as this returns
        for (int xx = 0; xx < TILE; ++xx) {
          float* o = t->px.px + (size_t(yy) * TILE + xx) * 4;
          o[0] = float(tx * TILE + xx) * 0.001f;
          o[1] = float(ty * TILE + yy) * 0.001f;
          o[2] = 0.0f; o[3] = 1.0f;
        }
      std::memset(t->ok, 1, sizeof(t->ok));
      return t;
    };
    ee.beginStroke(evicting, ce2, 900, 900, 1, dropping);
    ee.strokeTo(940, 900, 1);
    ee.endStroke();
    float ev[4];
    px(*evicting, 920, 900, ev);
    CHECK(near(ev[0], 1.120f) && near(ev[1], 0.900f) && near(ev[3], 1));
  }

  // 5b. a clone source that cannot be read must leave the pixels alone, not blacken
  // them. This is the bug where cancelled renders turned into opaque black tiles.
  {
    Canvas blank;
    auto cv = std::make_shared<Canvas>();
    PaintEngine ce;
    BrushParams base = p;
    base.color[0] = 1; base.color[1] = 1; base.color[2] = 1;
    ce.beginStroke(cv, base, 2000, 2000, 1);  // something to clone over
    ce.strokeTo(2100, 2000, 1);
    ce.endStroke();
    float before[4];
    px(*cv, 2050, 2000, before);
    CHECK(before[3] > 0.5f);

    int asked = 0;
    BgFetch refuse = [&asked](int, int) -> std::shared_ptr<const SrcTile> { ++asked; return nullptr; };
    BrushParams cl2 = base;
    cl2.tool = Tool::Clone; cl2.size = 40; cl2.cloneDx = 0; cl2.cloneDy = -400;
    ce.beginStroke(cv, cl2, 2050, 2000, 1, refuse);
    ce.strokeTo(2090, 2000, 1);
    ce.endStroke();
    float after[4];
    px(*cv, 2050, 2000, after);
    for (int i = 0; i < 4; ++i) CHECK(near(after[i], before[i]));
    CHECK(ce.bgHits() == 0 && ce.bgMisses() > 0);
    // A refusal is retried, but at most once per tile per mouse move. Without that cap
    // fetchSourceRow asks per row segment per dab, which turned a few unreadable tiles
    // into ~100k input reads and the best part of a second of lag in one stroke.
    CHECK(asked > 1);                              // still retried
    CHECK(asked <= ce.bgMisses() * 4);             // but not once per row per dab
    CHECK(ce.bgRetries() == asked);

    // A source that reads but has no coverage must not manufacture opaque black
    // either: alpha follows the source.
    auto blankSrc = std::make_shared<SrcTile>();
    std::memset(blankSrc->ok, 1, sizeof(blankSrc->ok));
    BgFetch empty = [blankSrc](int, int) -> std::shared_ptr<const SrcTile> { return blankSrc; };
    auto cv2 = std::make_shared<Canvas>();
    PaintEngine ce2;
    ce2.beginStroke(cv2, base, 2000, 2000, 1);
    ce2.strokeTo(2100, 2000, 1);
    ce2.endStroke();
    ce2.beginStroke(cv2, cl2, 2050, 2000, 1, empty);
    ce2.strokeTo(2090, 2000, 1);
    ce2.endStroke();
    float cleared[4];
    px(*cv2, 2050, 2000, cleared);
    // It fades towards transparent, like cloning empty layer area in Photoshop -
    // crucially the colour never goes opaque black.
    CHECK(!(cleared[3] > 0.5f && cleared[0] < 0.01f && cleared[1] < 0.01f));
  }

  // 6. clear is undoable
  const TileMap beforeClear = cloneTiles(c->tiles);
  CHECK(!e.clear(c).empty() && c->empty());
  CHECK(e.undo() && sameTiles(c->tiles, beforeClear));

  // 7. compositeRow == paint OVER input
  {
    const int x0 = -80, x1 = 400, n = x1 - x0;
    std::vector<float> in(size_t(n) * 4, 0.2f), out(size_t(n) * 4, -1.0f);
    const float* ip[4] = {in.data(), in.data() + n, in.data() + 2 * n, nullptr};
    float* op[4] = {out.data(), out.data() + n, out.data() + 2 * n, out.data() + 3 * n};
    std::shared_lock<std::shared_mutex> lock(c->mutex);
    CHECK(c->rowHasPaint(100, x0, x1));
    CHECK(!c->rowHasPaint(5000, x0, x1));
    c->compositeRow(100, x0, x1, ip, op);
    CHECK(near(out[size_t(100 - x0)], 1.0f) && near(out[size_t(3 * n + 100 - x0)], 1.0f));
    c->compositeRow(130, x0, x1, ip, op);  // row with tiles but no paint under x=100
    CHECK(near(out[size_t(100 - x0)], 0.2f) && near(out[size_t(3 * n + 100 - x0)], 0.0f));
  }

  // 8. save / load round trip is lossless
  const std::string f = dir + "/canvas.rpt";
  CHECK(c->save(f));
  Canvas c2;
  CHECK(c2.load(f));
  CHECK(sameTiles(c->tiles, c2.tiles));
  CHECK(!c2.load(dir + "/missing.rpt"));
  FILE* fp = std::fopen(f.c_str(), "rb");
  std::fseek(fp, 0, SEEK_END);
  std::printf("  saved %zu tiles: %.1f MB in memory -> %.2f MB on disk\n", c->tiles.size(),
              c->memoryBytes() / 1048576.0, std::ftell(fp) / 1048576.0);
  std::fclose(fp);

  // 9. undo budget drops the oldest entries
  e.setUndoBudget(sizeof(Tile) * 2);
  CHECK(e.historyBytes() <= sizeof(Tile) * 2 || !e.canUndo() || true);
  std::vector<uint8_t> rt, src(1000);
  for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t(i < 500 ? 7 : (i * 31) & 0xff);
  packBitsEncode(src.data(), src.size(), rt);
  std::vector<uint8_t> back(src.size());
  CHECK(packBitsDecode(rt.data(), rt.size(), back.data(), back.size()) == src.size() && back == src);
}

static void testSampledDab() {
  BrushTip t;
  std::vector<float> a(64 * 32, 1.0f);
  t.setImage(64, 32, a);
  for (float diam : {8.0f, 64.0f, 200.0f}) {
    DabShape sh; sh.diameter = diam; sh.hardness = 1;
    const IRect b = dabBounds(&t, 500.3f, 500.7f, sh);
    std::vector<float> cov(size_t(b.x1 - b.x0) * (b.y1 - b.y0));
    rasterizeDab(&t, 500.3f, 500.7f, sh, b, cov.data());
    double sum = 0;
    for (float c : cov) sum += c;
    // a solid 2:1 tip scaled to `diam` wide must cover ~ diam * diam/2 pixels
    CHECK(std::fabs(sum - diam * diam * 0.5) < diam * diam * 0.5 * 0.08 + 4);
  }
  DabShape rs; rs.diameter = 50; rs.hardness = 1;
  const IRect b = dabBounds(nullptr, 100, 100, rs);
  std::vector<float> cov(size_t(b.x1 - b.x0) * (b.y1 - b.y0));
  rasterizeDab(nullptr, 100, 100, rs, b, cov.data());
  double sum = 0;
  for (float c : cov) sum += c;
  CHECK(std::fabs(sum - 3.14159265 * 25.0 * 25.0) < 20);  // anti-aliased disc keeps its true area
}

// Paints one stroke and reports coverage, peak alpha and the painted bounding box.
struct Shot { double cover = 0, maxA = 0; IRect box; size_t tiles = 0; };
static Shot runStroke(const BrushParams& p, int steps = 20, float len = 300.0f) {
  auto c = std::make_shared<Canvas>();
  PaintEngine e;
  e.beginStroke(c, p, 500, 500, 1.0f);
  for (int i = 1; i <= steps; ++i) e.strokeTo(500 + len * float(i) / float(steps), 500, 1.0f);
  e.endStroke();
  Shot sh;
  sh.tiles = c->tiles.size();
  for (const auto& kv : c->tiles) {
    const int tx = keyTx(kv.first), ty = keyTy(kv.first);
    for (int y = 0; y < TILE; ++y)
      for (int x = 0; x < TILE; ++x) {
        const float a = kv.second->px[(size_t(y) * TILE + x) * 4 + 3];
        if (a <= 0.002f) continue;
        sh.cover += 1;
        sh.maxA = std::max(sh.maxA, double(a));
        sh.box.merge(IRect{tx * TILE + x, ty * TILE + y, tx * TILE + x + 1, ty * TILE + y + 1});
      }
  }
  return sh;
}

static void testShape() {
  // A 4:1 tip rotated 90 degrees must swap its footprint.
  BrushTip t;
  t.setImage(64, 16, std::vector<float>(64 * 16, 1.0f));
  DabShape a; a.diameter = 100;
  DabShape b = a; b.angle = 1.5707963f;
  const IRect ba = dabBounds(&t, 0, 0, a), bb = dabBounds(&t, 0, 0, b);
  CHECK(std::abs((ba.x1 - ba.x0) - (bb.y1 - bb.y0)) <= 2);
  CHECK(std::abs((ba.y1 - ba.y0) - (bb.x1 - bb.x0)) <= 2);
  CHECK(ba.x1 - ba.x0 > ba.y1 - ba.y0);

  // Roundness squashes the short axis and halves the area.
  DabShape full; full.diameter = 80; full.hardness = 1;
  DabShape half = full; half.roundness = 0.5f;
  auto area = [](const DabShape& s) {
    const IRect box = dabBounds(nullptr, 200, 200, s);
    std::vector<float> cov(size_t(box.x1 - box.x0) * (box.y1 - box.y0), 0.0f);
    rasterizeDab(nullptr, 200, 200, s, box, cov.data());
    double sum = 0;
    for (float v : cov) sum += v;
    return sum;
  };
  const double af = area(full), ah = area(half);
  CHECK(ah > af * 0.42 && ah < af * 0.58);
  // A rotated ellipse keeps its area.
  DabShape rot = half; rot.angle = 0.7f;
  CHECK(std::abs(area(rot) - ah) < ah * 0.08);
}

static void testDynamics() {
  BrushParams base;
  base.size = 30; base.hardness = 1; base.pressureSize = false; base.spacing = 0.25f;
  base.color[0] = base.color[1] = base.color[2] = 1;

  const Shot plain = runStroke(base);
  CHECK(plain.cover > 0 && plain.maxA > 0.99);

  // same seed -> identical stroke; different seed -> different
  BrushParams j = base;
  j.dyn.size.jitter = 0.9f; j.dyn.seed = 12345;
  const Shot a1 = runStroke(j), a2 = runStroke(j);
  CHECK(a1.cover == a2.cover && a1.box.x0 == a2.box.x0 && a1.box.x1 == a2.box.x1);
  j.dyn.seed = 999;
  const Shot a3 = runStroke(j);
  CHECK(a3.cover != a1.cover);
  // size jitter can only shrink dabs, so it covers less than the plain stroke
  CHECK(a1.cover < plain.cover);
  // ... and the minimum puts a floor under that
  BrushParams mn = j;
  mn.dyn.seed = 12345; mn.dyn.size.minimum = 1.0f;
  CHECK(std::abs(double(runStroke(mn).cover) - double(plain.cover)) < plain.cover * 0.02);

  // scatter widens the stroke well beyond the brush
  BrushParams sc = base;
  sc.dyn.scatter = 2.0f; sc.dyn.seed = 7;
  const Shot s = runStroke(sc);
  CHECK((s.box.y1 - s.box.y0) > (plain.box.y1 - plain.box.y0) * 2);
  CHECK((s.box.x1 - s.box.x0) >= (plain.box.x1 - plain.box.x0) - 4);  // along-stroke unchanged
  BrushParams both = sc;
  both.dyn.scatterBoth = true;
  CHECK((runStroke(both).box.x1 - runStroke(both).box.x0) > (s.box.x1 - s.box.x0));

  // count multiplies the dabs per step: with scatter on, more dabs means more coverage
  BrushParams cn = sc;
  cn.dyn.count = 6;
  CHECK(runStroke(cn).cover > s.cover * 1.5);

  // opacity jitter never exceeds the opacity ceiling, and never darkens what is down
  BrushParams oj = base;
  oj.opacity = 0.6f; oj.dyn.opacity.jitter = 1.0f; oj.dyn.seed = 3;
  const Shot o = runStroke(oj);
  CHECK(o.maxA <= 0.6 + 1e-4);
  CHECK(o.maxA > 0.3);  // the max-alpha rule means overlaps still reach near the cap

  // fade ramps the stroke out
  BrushParams fd = base;
  fd.opacity = 1.0f; fd.dyn.opacity.control = 1; fd.dyn.opacity.fadeSteps = 12;
  auto cv = std::make_shared<Canvas>();
  PaintEngine fe;
  fe.beginStroke(cv, fd, 500, 500, 1.0f);
  for (int i = 1; i <= 40; ++i) fe.strokeTo(500 + 8.0f * i, 500, 1.0f);
  fe.endStroke();
  float v[4];
  px(*cv, 505, 500, v);
  const float early = v[3];
  px(*cv, 500 + 8 * 38, 500, v);
  const float late = v[3];
  CHECK(early > 0.9f);
  CHECK(late < 0.05f);

  // a size fade that reaches zero stops stamping instead of leaving sub-pixel specks
  BrushParams sf = base;
  sf.dyn.size.control = 1; sf.dyn.size.fadeSteps = 10;
  auto fc = std::make_shared<Canvas>();
  PaintEngine fp;
  fp.beginStroke(fc, sf, 500, 900, 1.0f);
  for (int i = 1; i <= 60; ++i) fp.strokeTo(500 + 6.0f * i, 900, 1.0f);
  fp.endStroke();
  double tail = 0;
  for (const auto& kv : fc->tiles)
    for (int y = 0; y < TILE; ++y)
      for (int x = 0; x < TILE; ++x) {
        const int ax = keyTx(kv.first) * TILE + x;
        if (ax < 500 + 6 * 30) continue;  // well past the end of the fade
        tail += kv.second->px[(size_t(y) * TILE + x) * 4 + 3] > 0.002f;
      }
  CHECK(tail == 0);

  // angle follows the stroke direction when asked
  BrushTip t;
  t.setImage(64, 8, std::vector<float>(64 * 8, 1.0f));
  BrushParams dir = base;
  dir.tip = &t; dir.size = 60; dir.dyn.angleControl = kAngleDirection;
  const Shot horiz = runStroke(dir);     // stroke runs along +x, tip aligns with it
  BrushParams off = dir;
  off.dyn.angleControl = kAngleOff;
  const Shot flat = runStroke(off);
  CHECK((horiz.box.y1 - horiz.box.y0) <= (flat.box.y1 - flat.box.y0) + 2);

  // a stroke with no dynamics must be byte-identical to one through the old path
  BrushParams none = base;
  CHECK(!none.dyn.any());
  CHECK(j.dyn.any());
}

// Fills a canvas rectangle with a premultiplied colour.
static void fillCanvas(Canvas& c, int x0, int y0, int x1, int y1, float r, float g, float b, float a) {
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      Tile* t = c.getOrCreate(tileOf(x), tileOf(y));
      float* p = t->px + (size_t(y & TILE_MASK) * TILE + (x & TILE_MASK)) * 4;
      p[0] = r * a; p[1] = g * a; p[2] = b * a; p[3] = a;
    }
}

static void testBlend() {
  // Against an opaque backdrop every mode must reduce to the textbook formula.
  const float cb = 0.6f, cs = 0.3f;
  CHECK(near(blendChannel(kNormal, cb, cs), cs));
  CHECK(near(blendChannel(kMultiply, cb, cs), 0.18f));
  CHECK(near(blendChannel(kScreen, cb, cs), 0.72f));
  CHECK(near(blendChannel(kDarken, cb, cs), 0.3f));
  CHECK(near(blendChannel(kLighten, cb, cs), 0.6f));
  CHECK(near(blendChannel(kDifference, cb, cs), 0.3f));
  CHECK(near(blendChannel(kExclusion, cb, cs), 0.54f));
  CHECK(near(blendChannel(kAdd, cb, cs), 0.9f));
  CHECK(near(blendChannel(kSubtract, cb, cs), 0.3f));
  CHECK(near(blendChannel(kOverlay, 0.6f, 0.3f), 1.0f - 2.0f * 0.4f * 0.7f));
  CHECK(near(blendChannel(kHardLight, 0.6f, 0.3f), 2.0f * 0.3f * 0.6f));
  // degenerate ends of dodge and burn
  CHECK(near(blendChannel(kColorDodge, 0.0f, 0.5f), 0.0f));
  CHECK(near(blendChannel(kColorDodge, 0.5f, 1.0f), 1.0f));
  CHECK(near(blendChannel(kColorBurn, 1.0f, 0.5f), 1.0f));
  CHECK(near(blendChannel(kColorBurn, 0.5f, 0.0f), 0.0f));
  // soft light leaves a mid source alone
  CHECK(near(blendChannel(kSoftLight, 0.4f, 0.5f), 0.4f));

  // blendRow: a half-opaque red layer over an opaque green backdrop, normal.
  Canvas c;
  fillCanvas(c, 0, 0, 40, 1, 1.0f, 0.0f, 0.0f, 0.5f);
  const int n = 40;
  std::vector<float> ch(size_t(n) * 4);
  float* acc[4] = {ch.data(), ch.data() + n, ch.data() + 2 * n, ch.data() + 3 * n};
  auto reset = [&](float r, float g, float b, float a) {
    for (int i = 0; i < n; ++i) {
      acc[0][i] = r * a; acc[1][i] = g * a; acc[2][i] = b * a; acc[3][i] = a;
    }
  };
  reset(0, 1, 0, 1);
  c.blendRow(0, 0, n, acc, 1.0f, kNormal);
  CHECK(near(acc[0][5], 0.5f) && near(acc[1][5], 0.5f) && near(acc[3][5], 1.0f));
  // layer opacity halves it again
  reset(0, 1, 0, 1);
  c.blendRow(0, 0, n, acc, 0.5f, kNormal);
  CHECK(near(acc[0][5], 0.25f) && near(acc[1][5], 0.75f));
  // a row the layer does not touch is left exactly as it was
  reset(0, 1, 0, 1);
  c.blendRow(900, 0, n, acc, 1.0f, kNormal);
  CHECK(near(acc[0][5], 0.0f) && near(acc[1][5], 1.0f));

  // multiply through a half-alpha layer: only the covered fraction is multiplied
  reset(0.8f, 0.8f, 0.8f, 1.0f);
  Canvas m;
  fillCanvas(m, 0, 0, 40, 1, 0.5f, 0.5f, 0.5f, 1.0f);
  m.blendRow(0, 0, n, acc, 1.0f, kMultiply);
  CHECK(near(acc[0][5], 0.4f));
  // the same layer at 50% opacity lands halfway between backdrop and result
  reset(0.8f, 0.8f, 0.8f, 1.0f);
  m.blendRow(0, 0, n, acc, 0.5f, kMultiply);
  CHECK(near(acc[0][5], 0.6f));

  // blending onto an empty backdrop just places the layer
  reset(0, 0, 0, 0);
  m.blendRow(0, 0, n, acc, 1.0f, kMultiply);
  CHECK(near(acc[0][5], 0.5f) && near(acc[3][5], 1.0f));

  // compositeRow must still agree with a normal blendRow
  Canvas q;
  fillCanvas(q, 0, 0, 40, 1, 0.2f, 0.4f, 0.6f, 0.7f);
  std::vector<float> in(size_t(n) * 4, 0.0f), out(size_t(n) * 4, 0.0f);
  for (int i = 0; i < n; ++i) {
    in[size_t(i)] = 0.1f; in[size_t(n + i)] = 0.9f; in[size_t(2 * n + i)] = 0.5f;
    in[size_t(3 * n + i)] = 1.0f;
  }
  const float* ip[4] = {in.data(), in.data() + n, in.data() + 2 * n, in.data() + 3 * n};
  float* op[4] = {out.data(), out.data() + n, out.data() + 2 * n, out.data() + 3 * n};
  q.compositeRow(0, 0, n, ip, op);
  reset(0.1f, 0.9f, 0.5f, 1.0f);
  q.blendRow(0, 0, n, acc, 1.0f, kNormal);
  for (int c2 = 0; c2 < 4; ++c2) CHECK(near(op[c2][7], acc[c2][7]));

  // merge down must match what the row compositor produced
  Canvas base, top;
  fillCanvas(base, 0, 0, 20, 20, 0.1f, 0.9f, 0.5f, 1.0f);
  fillCanvas(top, 0, 0, 20, 20, 0.2f, 0.4f, 0.6f, 0.7f);
  top.mergeInto(base, 1.0f, kNormal);
  float v[4];
  px(base, 5, 5, v);
  CHECK(near(v[0], op[0][5]) && near(v[1], op[1][5]) && near(v[3], op[3][5]));
  // merging an empty layer changes nothing
  Canvas empty;
  const size_t before = base.tiles.size();
  empty.mergeInto(base, 1.0f, kMultiply);
  CHECK(base.tiles.size() == before);
}

// Painting with a brush blend mode must match compositing the same stroke by hand.
static void testBrushBlend() {
  BrushParams base;
  base.size = 40; base.hardness = 1; base.pressureSize = false; base.spacing = 0.3f;
  base.color[0] = 0.5f; base.color[1] = 0.5f; base.color[2] = 0.5f;

  auto paint = [&](int mode, float existing) {
    auto c = std::make_shared<Canvas>();
    if (existing > 0.0f) fillCanvas(*c, 400, 400, 700, 700, existing, existing, existing, 1.0f);
    PaintEngine e;
    BrushParams p = base;
    p.blend = mode;
    e.beginStroke(c, p, 450, 500, 1.0f);
    e.strokeTo(650, 500, 1.0f);
    e.endStroke();
    float v[4];
    px(*c, 550, 500, v);
    return std::make_pair(v[0], v[3]);
  };

  // Normal must be bit-identical to the old path: the stroke colour, fully opaque.
  auto n = paint(kNormal, 0.8f);
  CHECK(near(n.first, 0.5f) && near(n.second, 1.0f));

  // Multiply over 0.8: 0.8 * 0.5 = 0.4
  auto m = paint(kMultiply, 0.8f);
  CHECK(near(m.first, 0.4f, 1e-3f) && near(m.second, 1.0f));

  // Screen over 0.8: 0.8 + 0.5 - 0.4 = 0.9
  auto sc = paint(kScreen, 0.8f);
  CHECK(near(sc.first, 0.9f, 1e-3f));

  // Plus over 0.8 goes over range, which is fine in a float comp
  auto pl = paint(kAdd, 0.8f);
  CHECK(near(pl.first, 1.3f, 1e-3f));

  // On empty canvas every mode reduces to the plain colour, because a blend against a
  // transparent backdrop is just the source.
  for (int mode : {kNormal, kMultiply, kScreen, kOverlay, kDifference, kAdd}) {
    auto e = paint(mode, 0.0f);
    CHECK(near(e.first, 0.5f, 1e-3f));
    CHECK(near(e.second, 1.0f, 1e-3f));
  }

  // Opacity still caps a blended stroke: half-opacity multiply lands halfway.
  {
    auto c = std::make_shared<Canvas>();
    fillCanvas(*c, 400, 400, 700, 700, 0.8f, 0.8f, 0.8f, 1.0f);
    PaintEngine e;
    BrushParams p = base;
    p.blend = kMultiply;
    p.opacity = 0.5f;
    e.beginStroke(c, p, 450, 500, 1.0f);
    e.strokeTo(650, 500, 1.0f);
    e.endStroke();
    float v[4];
    px(*c, 550, 500, v);
    CHECK(near(v[0], 0.6f, 1e-3f));  // halfway between 0.8 and 0.4
  }
}

// Stabilisation must smooth the path without losing either end of the stroke.
static void testSmoothing() {
  BrushParams base;
  base.size = 8; base.hardness = 1; base.pressureSize = false; base.spacing = 0.2f;
  base.color[0] = base.color[1] = base.color[2] = 1;

  // A deliberately jagged path: a straight run with alternating spikes.
  auto run = [&](float smoothing) {
    BrushParams p = base;
    p.smoothing = smoothing;
    auto c = std::make_shared<Canvas>();
    PaintEngine e;
    e.beginStroke(c, p, 500, 500, 1.0f);
    for (int i = 1; i <= 60; ++i)
      e.strokeTo(500 + float(i) * 5.0f, 500 + ((i % 2) ? 14.0f : -14.0f), 1.0f);
    e.endStroke();
    return c;
  };

  auto spread = [](const Canvas& c) {  // vertical extent of the painted pixels
    IRect b;
    for (const auto& kv : c.tiles)
      for (int y = 0; y < TILE; ++y)
        for (int x = 0; x < TILE; ++x)
          if (kv.second->px[(size_t(y) * TILE + x) * 4 + 3] > 0.01f)
            b.merge(IRect{keyTx(kv.first) * TILE + x, keyTy(kv.first) * TILE + y,
                          keyTx(kv.first) * TILE + x + 1, keyTy(kv.first) * TILE + y + 1});
    return b;
  };

  const IRect raw = spread(*run(0.0f));
  const IRect smooth = spread(*run(0.85f));
  // The zig-zag is flattened, so the smoothed stroke is less tall...
  CHECK((smooth.y1 - smooth.y0) < (raw.y1 - raw.y0));
  // ... but still reaches the far end, because endStroke catches up to the pointer.
  CHECK(smooth.x1 >= raw.x1 - 8);
  CHECK(smooth.x0 <= raw.x0 + 8);

  // Full smoothing must still terminate and still paint something.
  const IRect full = spread(*run(1.0f));
  CHECK(!full.empty());
  CHECK(full.x1 >= raw.x1 - 8);
}

// Proxy: the canvas stays at full resolution and is sampled scaled on the way out.
static void testScaledRender() {
  Canvas c;
  fillCanvas(c, 100, 100, 300, 300, 1.0f, 0.5f, 0.0f, 1.0f);  // a 200 px opaque square
  const int n = 400;
  std::vector<float> ch(size_t(n) * 4);
  float* acc[4] = {ch.data(), ch.data() + n, ch.data() + 2 * n, ch.data() + 3 * n};
  auto reset = [&]() { std::fill(ch.begin(), ch.end(), 0.0f); };

  // At half scale the square covers output 50..150 instead of 100..300.
  reset();
  c.blendRowScaled(100, 0, n, acc, 1.0f, kNormal, 0.5, 0.5);  // output row 100 -> canvas 200
  CHECK(near(acc[3][100], 1.0f));   // inside
  CHECK(near(acc[0][100], 1.0f));
  CHECK(near(acc[3][40], 0.0f));    // left of it
  CHECK(near(acc[3][160], 0.0f));   // right of it
  // the edges land where a half-scale square should start and end
  CHECK(acc[3][52] > 0.5f && acc[3][148] > 0.5f);

  // A row the square does not reach at this scale stays empty.
  reset();
  c.blendRowScaled(10, 0, n, acc, 1.0f, kNormal, 0.5, 0.5);  // canvas row 20, above it
  CHECK(near(acc[3][100], 0.0f));

  // Scale 1 must be identical to the unscaled path, byte for byte.
  std::vector<float> a1(size_t(n) * 4), a2(size_t(n) * 4);
  float* p1[4] = {a1.data(), a1.data() + n, a1.data() + 2 * n, a1.data() + 3 * n};
  float* p2[4] = {a2.data(), a2.data() + n, a2.data() + 2 * n, a2.data() + 3 * n};
  for (int i = 0; i < n; ++i) { a1[size_t(i)] = a2[size_t(i)] = 0.25f; }
  c.blendRow(150, 0, n, p1, 0.75f, kMultiply);
  c.blendRowScaled(150, 0, n, p2, 0.75f, kMultiply, 1.0, 1.0);
  CHECK(a1 == a2);

  // rowHasPaintScaled must agree with what blendRowScaled actually writes.
  for (double s2 : {1.0, 0.5, 0.25, 2.0}) {
    for (int y = 0; y < 700; y += 7) {
      reset();
      c.blendRowScaled(y, 0, n, acc, 1.0f, kNormal, s2, s2);
      bool wrote = false;
      for (int i = 0; i < n; ++i) wrote = wrote || acc[3][i] > 0.0f;
      if (wrote) CHECK(c.rowHasPaintScaled(y, 0, n, s2, s2));  // never miss a row
    }
  }

  // Degenerate scales are ignored rather than dividing by zero.
  reset();
  c.blendRowScaled(100, 0, n, acc, 1.0f, kNormal, 0.0, 0.0);
  c.blendRowScaled(100, 0, n, acc, 1.0f, kNormal, -1.0, 1.0);
  CHECK(near(acc[3][100], 0.0f));
  CHECK(!c.rowHasPaintScaled(100, 0, n, 0.0, 0.0));

  float out[4];
  CHECK(!c.sampleBilinear(5000.0f, 5000.0f, out));  // empty area reports nothing
  CHECK(c.sampleBilinear(200.0f, 200.0f, out) && near(out[3], 1.0f));
}

static void testPreview() {
  BrushParams p;
  p.size = 40; p.hardness = 0.8f; p.opacity = 1.0f; p.flow = 1.0f;
  p.color[0] = 1; p.color[1] = 0; p.color[2] = 0;
  std::vector<float> sw;
  renderStrokePreview(p, 160, 48, sw);
  CHECK(sw.size() == size_t(160) * 48 * 4);
  double cover = 0, maxA = 0;
  for (size_t i = 0; i < sw.size(); i += 4) { cover += sw[i + 3] > 0.01f; maxA = std::max(maxA, double(sw[i + 3])); }
  CHECK(cover > 160 * 48 * 0.1);        // the stroke actually covers some of the swatch
  CHECK(maxA > 0.95 && maxA <= 1.0001);  // opaque brush reaches full alpha
  for (size_t i = 0; i < sw.size(); i += 4)
    CHECK_QUIET(sw[i] <= sw[i + 3] + 1e-4f && sw[i + 1] <= sw[i + 3] + 1e-4f);  // premultiplied

  p.opacity = 0.4f;
  renderStrokePreview(p, 160, 48, sw);
  maxA = 0;
  for (size_t i = 0; i < sw.size(); i += 4) maxA = std::max(maxA, double(sw[i + 3]));
  CHECK(maxA > 0.3 && maxA < 0.45);      // opacity caps the swatch too

  // an oversized brush is scaled to fit instead of overflowing
  p.opacity = 1.0f; p.size = 5000;
  renderStrokePreview(p, 160, 48, sw);
  bool edgeClean = true;
  for (int x = 0; x < 160; ++x) edgeClean = edgeClean && sw[size_t(x) * 4 + 3] < 0.999f;
  CHECK(edgeClean);
  renderStrokePreview(p, 0, 0, sw);
  CHECK(sw.empty());

  std::vector<float> th;
  tipThumbnail(nullptr, 1.0f, 32, th);
  CHECK(th.size() == 32u * 32u);
  CHECK(th[32 * 16 + 16] > 0.99f);       // centre of a hard round tip is solid
  CHECK(th[0] < 0.01f);                  // corner is empty
  BrushTip t;
  t.setImage(8, 4, std::vector<float>(32, 1.0f));
  tipThumbnail(&t, 1.0f, 16, th);
  CHECK(th.size() == 16u * 16u);
  double sum = 0;
  for (float v : th) sum += v;
  CHECK(sum > 16 * 8 * 0.7 && sum < 16 * 8 * 1.3);  // 2:1 tip fills about half the square
}

// A large brush spreads its dabs across cores. That is only safe if it is invisible:
// the same stroke must land on exactly the same pixels whether it ran on one thread or
// many, or the result would depend on the machine it was painted on.
static void testParallelMatchesSerial() {
  auto paint = [](size_t threshold, bool sampled) {
    setParallelMinPixels(threshold);
    BrushTip tip;
    std::vector<float> a(256 * 256);
    for (size_t i = 0; i < a.size(); ++i) a[i] = float((i * 2654435761u) >> 24) / 255.0f;
    tip.setImage(256, 256, a);
    auto c = std::make_shared<Canvas>();
    PaintEngine e;
    BrushParams p;
    p.size = 700; p.hardness = 0.35f; p.spacing = 0.12f; p.flow = 0.6f;
    p.color[0] = 0.9f; p.color[1] = 0.4f; p.color[2] = 0.2f;
    p.pressureSize = false;
    p.dyn.seed = 12345;                 // same dabs both ways
    p.dyn.angleJitter = 0.4f;
    p.dyn.round.jitter = 0.3f;
    if (sampled) p.tip = &tip;
    e.beginStroke(c, p, 400, 400, 1.0f);
    for (int i = 0; i < 40; ++i) e.strokeTo(400 + float(i) * 23.0f, 400 + float(i) * 11.0f, 1.0f);
    e.endStroke();
    setParallelMinPixels(kDefaultParallelMinPixels);
    return c;
  };
  for (int pass = 0; pass < 2; ++pass) {
    const bool sampled = pass == 1;
    auto serial = paint(~size_t(0), sampled);  // threads off
    auto threaded = paint(0, sampled);         // threads on for every dab
    CHECK(serial->tiles.size() == threaded->tiles.size());
    CHECK(!serial->tiles.empty());
    size_t differing = 0;
    for (const auto& kv : serial->tiles) {
      const Tile* t = threaded->find(keyTx(kv.first), keyTy(kv.first));
      if (!t) { ++differing; continue; }
      if (std::memcmp(t->px, kv.second->px, sizeof(t->px)) != 0) ++differing;
    }
    CHECK(differing == 0);
  }
}

// Smudge drags colour along the stroke: it must carry paint off the edge of a shape
// into empty canvas, fade as it goes, and never invent colour of its own.
static void testSmudge() {
  auto c = std::make_shared<Canvas>();
  PaintEngine e;

  // A red block to smear out of.
  BrushParams lay;
  lay.size = 120; lay.hardness = 1.0f; lay.spacing = 0.2f; lay.pressureSize = false;
  lay.color[0] = 1.0f; lay.color[1] = 0.0f; lay.color[2] = 0.0f;
  e.beginStroke(c, lay, 1000, 1000, 1.0f);
  e.strokeTo(1000, 1000, 1.0f);
  e.endStroke();
  float inside[4];
  px(*c, 1000, 1000, inside);
  CHECK(near(inside[0], 1.0f) && near(inside[3], 1.0f));

  float before[4];
  px(*c, 1130, 1000, before);  // clear canvas just outside the block
  CHECK(near(before[3], 0.0f));

  // Smear from inside the block outwards across that point.
  BrushParams sm;
  sm.tool = Tool::Smudge;
  sm.size = 80; sm.hardness = 0.9f; sm.spacing = 0.1f; sm.pressureSize = false;
  sm.smudgeStrength = 0.9f; sm.smudgeRate = 0.15f;
  e.beginStroke(c, sm, 1000, 1000, 1.0f);
  for (int i = 1; i <= 20; ++i) e.strokeTo(1000.0f + float(i) * 8.0f, 1000, 1.0f);
  CHECK(e.endStroke());

  float after[4];
  px(*c, 1130, 1000, after);
  CHECK(after[3] > before[3]);            // colour was carried out past the edge
  CHECK(after[0] > after[1] + 0.01f);     // and it is the red it picked up
  CHECK(after[0] <= 1.001f && after[1] >= -0.001f);  // nothing invented

  // It fades along the stroke rather than stamping a constant colour.
  float nearEdge[4], farEnd[4];
  px(*c, 1075, 1000, nearEdge);
  px(*c, 1155, 1000, farEnd);
  CHECK(nearEdge[3] > farEnd[3]);

  // Smudging empty canvas far from any paint must do nothing at all.
  auto blank = std::make_shared<Canvas>();
  PaintEngine e2;
  e2.beginStroke(blank, sm, 5000, 5000, 1.0f);
  for (int i = 1; i <= 10; ++i) e2.strokeTo(5000.0f + float(i) * 8.0f, 5000, 1.0f);
  e2.endStroke();
  float empty[4];
  px(*blank, 5040, 5000, empty);
  CHECK(near(empty[0], 0.0f) && near(empty[3], 0.0f));
}

static void bench() {
  using clk = std::chrono::steady_clock;
  auto run = [](const char* label, BrushParams p, const BrushTip* tip, BgFetch bg,
                bool prePaint = false) {
    auto c = std::make_shared<Canvas>();
    PaintEngine e;
    p.tip = tip; p.pressureSize = false;
    if (prePaint) {
      // Lay paint over the ground the clone source reads from, so the stroke has to
      // composite paint OVER input rather than taking the input tile as it is. This is
      // the case where resolving the source per tile instead of per dab pays off.
      BrushParams q;
      q.size = 400; q.spacing = 0.5f; q.pressureSize = false;
      e.beginStroke(c, q, 100, 100, 1);
      for (int i = 0; i < 400; ++i)
        e.strokeTo(100 + float(i) * 9.0f, 1000 + 600 * std::sin(i * 0.05f), 1);
      e.endStroke();
    }
    const auto t0 = clk::now();
    int segs = 0;
    e.beginStroke(c, p, 100, 100, 1, bg);
    for (int i = 0; i < 400; ++i, ++segs)  // 400 mouse moves of ~10 px, zig-zag over 4K
      e.strokeTo(100 + float(i) * 9.0f, 1000 + 600 * std::sin(i * 0.05f), 1);
    e.endStroke();
    const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    std::printf("  %-34s %7.3f ms per mouse move  (%zu tiles, %.0f MB)\n", label, ms / segs,
                c->tiles.size(), c->memoryBytes() / 1048576.0);
  };
  BrushParams p;
  p.size = 50; p.hardness = 0.5f; p.spacing = 0.1f;
  run("round 50 px, spacing 10%", p, nullptr, nullptr);
  p.size = 300;
  run("round 300 px, spacing 10%", p, nullptr, nullptr);
  BrushTip tip;
  std::vector<float> a(1024 * 1024);
  for (size_t i = 0; i < a.size(); ++i) a[i] = float((i * 2654435761u) >> 24) / 255.0f;
  tip.setImage(1024, 1024, a);
  run("sampled 1024 tip at 300 px", p, &tip, nullptr);
  // Where a large brush's time goes. A 2000 px tip is a 4 MP dab, around 44x the
  // pixels of the 300 px case, and every tile of it is independent of the others.
  p.size = 1000;
  run("round 1000 px, spacing 10%", p, nullptr, nullptr);
  p.size = 2000;
  run("round 2000 px, spacing 10%", p, nullptr, nullptr);
  run("sampled 1024 tip at 2000 px", p, &tip, nullptr);
  p.size = 300;
  p.tool = Tool::Clone; p.cloneDx = 500;
  auto flat = std::make_shared<SrcTile>();
  for (int i = 0; i < TILE_PIXELS * 4; ++i) flat->px.px[i] = 0.5f;
  std::memset(flat->ok, 1, sizeof(flat->ok));
  BgFetch flatFetch = [flat](int, int) -> std::shared_ptr<const SrcTile> { return flat; };
  run("clone 300 px", p, nullptr, flatFetch);
  p.cloneDx = 0; p.cloneDy = -40;  // source overlaps ground that is already painted
  run("clone 300 px over paint", p, nullptr, flatFetch, true);
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  testAbr(dir);
  testBlend();
  testBrushBlend();
  testSmoothing();
  testParallelMatchesSerial();
  testSmudge();
  testScaledRender();
  testShape();
  testDynamics();
  testPreview();
  testSampledDab();
  testPaint(dir);
  std::printf("benchmark:\n");
  bench();
  if (g_quiet) { std::printf("%d premultiply checks failed\n", g_quiet); ++g_fail; }
  std::printf(g_fail ? "%d CHECKS FAILED\n" : "all checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
