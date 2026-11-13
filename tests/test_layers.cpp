// Tests for the layer stack's metadata round trip and merge bookkeeping.
//
// PaintStore lives inside the plugin translation unit, so this test includes it
// directly and runs it against the stub DDImage headers - no Nuke, no GL, no Op.
#include "NPaintShop.cpp"

#include <cmath>
#include <set>
#include <cstdio>

static bool near(float a, float b, float e = 1e-5f) { return std::fabs(a - b) <= e; }

static int g_fail = 0;
#define CHECK(c) \
  do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
  // ---- round trip, including names that collide with the field separators
  {
    PaintStore s;
    s.addLayer("Background");
    s.addLayer("Shadows | pass;1 100%");
    s.addLayer("  spaces  and\ttabs ");
    s.layers[0].opacity = 0.25f;
    s.layers[0].blend = rp::kMultiply;
    s.layers[0].visible = false;
    s.layers[1].locked = true;
    s.layers[2].blend = rp::kSoftLight;
    s.active = 2;
    const std::string data = s.serialize();
    CHECK(data.rfind("v1;", 0) == 0);

    PaintStore t;
    t.deserialize(data);
    CHECK(t.layers.size() == 3);
    CHECK(t.active == 2);
    CHECK(t.nextId == s.nextId);
    if (t.layers.size() == 3) {
      CHECK(t.layers[0].name == "Background");
      CHECK(t.layers[1].name == "Shadows | pass;1 100%");
      CHECK(t.layers[2].name == "  spaces  and\ttabs ");
      CHECK(near(t.layers[0].opacity, 0.25f));
      CHECK(t.layers[0].blend == rp::kMultiply);
      CHECK(!t.layers[0].visible);
      CHECK(t.layers[1].locked);
      CHECK(t.layers[2].blend == rp::kSoftLight);
      CHECK(t.layers[0].id == s.layers[0].id);
    }
    // serialising the reloaded stack must give byte-identical text
    CHECK(t.serialize() == data);
  }

  // ---- malformed input must never crash or wipe a good stack
  {
    PaintStore s;
    s.addLayer("Keep me");
    const std::string good = s.serialize();
    for (const char* bad : {"", "v1", "v1;", "v2;1;0|1,1,0,1,0,x", "v1;1;0|",
                            "v1;abc;0|1,1,0,1,0,n", "v1;1;99|1,1,0,1,0,n",
                            "v1;1;0|1,1,99,1,0,n", "garbage"}) {
      PaintStore t;
      t.addLayer("Keep me");
      t.deserialize(bad);
      CHECK(!t.layers.empty());
      for (const Layer& l : t.layers) {
        CHECK(l.blend >= 0 && l.blend < int(rp::kBlendCount));
        CHECK(!l.name.empty());
      }
      CHECK(t.active >= 0 && t.active < int(t.layers.size()));
    }
    CHECK(s.serialize() == good);
  }

  // ---- reloading keeps the pixels already in memory for a layer with the same id
  {
    PaintStore s;
    s.addLayer("One");
    auto c = std::make_shared<rp::Canvas>();
    c->getOrCreate(0, 0)->px[3] = 1.0f;
    s.layers[0].canvases[PaintStore::kAllFrames] = c;
    const std::string data = s.serialize();
    s.deserialize(data);  // reload onto itself
    CHECK(s.layers.size() == 1);
    CHECK(s.layers[0].canvases.count(PaintStore::kAllFrames) == 1);
    CHECK(s.layers[0].canvases[PaintStore::kAllFrames].get() == c.get());
  }

  // ---- file naming is stable per layer id and distinct per frame
  {
    CHECK(PaintStore::fileFor("/d", 3, PaintStore::kAllFrames) == "/d/paint.L03.rpt");
    CHECK(PaintStore::fileFor("/d", 3, 101) == "/d/paint.L03.0101.rpt");
    CHECK(PaintStore::fileFor("/d", 12, 7) == "/d/paint.L12.0007.rpt");
    CHECK(PaintStore::legacyFileFor("/d", PaintStore::kAllFrames) == "/d/paint.rpt");
    CHECK(PaintStore::fileFor("/d", 1, 5) != PaintStore::fileFor("/d", 2, 5));
  }

  // ---- ids keep rising, so a deleted layer's files are never reused by a new one
  {
    PaintStore s;
    s.addLayer("a");
    s.addLayer("b");
    const int idA = s.layers[0].id, idB = s.layers[1].id;
    s.layers.erase(s.layers.begin());
    s.addLayer("c");
    CHECK(s.layers.back().id != idA);
    CHECK(s.layers.back().id != idB);
  }

  // ---- contributes() drives whether a layer renders at all
  {
    Layer l;
    CHECK(l.contributes());
    l.visible = false;
    CHECK(!l.contributes());
    l.visible = true; l.opacity = 0.0f;
    CHECK(!l.contributes());
  }

  // ---- brush presets: round trip, replacement, escaping, malformed lines
  {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / "rp_presets_test.txt";
    fs::remove(tmp, ec);
    BrushPresets::path() = tmp.generic_string();

    CHECK(BrushPresets::read().empty());  // missing file is simply no presets

    std::vector<Preset> ps;
    CHECK(BrushPresets::put(ps, "Soft round", "size=50\thardness=0.1\tabr=\tbrush=0") == 0);
    CHECK(BrushPresets::put(ps, "Grit\tsplatter 50%", "size=120\tscatter=2") == 1);
    CHECK(BrushPresets::write(ps));

    std::vector<Preset> back = BrushPresets::read();
    CHECK(back.size() == 2);
    if (back.size() == 2) {
      CHECK(back[0].name == "Soft round");
      CHECK(back[1].name == "Grit\tsplatter 50%");  // tabs in a name survive escaping
      std::string v;
      CHECK(BrushPresets::field(back[0].line, "size", v) && v == "50");
      CHECK(BrushPresets::field(back[0].line, "hardness", v) && v == "0.1");
      CHECK(BrushPresets::field(back[0].line, "brush", v) && v == "0");
      CHECK(BrushPresets::field(back[0].line, "abr", v) && v.empty());
      CHECK(!BrushPresets::field(back[0].line, "nope", v));
      // a key must match whole, not as a prefix of another
      CHECK(BrushPresets::field(back[1].line, "scatter", v) && v == "2");
      CHECK(!BrushPresets::field(back[1].line, "scat", v));
    }

    // saving the same name again replaces rather than duplicates
    CHECK(BrushPresets::put(back, "Soft round", "size=9") == 0);
    CHECK(back.size() == 2);
    CHECK(BrushPresets::write(back));
    CHECK(BrushPresets::read().size() == 2);
    std::string v;
    CHECK(BrushPresets::field(BrushPresets::read()[0].line, "size", v) && v == "9");

    // deleting leaves the other one intact
    back.erase(back.begin());
    CHECK(BrushPresets::write(back));
    CHECK(BrushPresets::read().size() == 1);
    CHECK(BrushPresets::read()[0].name == "Grit\tsplatter 50%");

    // junk lines are skipped, not treated as presets
    {
      std::ofstream f(tmp.generic_string(), std::ios::app);
      f << "no tab here\n\n\tempty name\n";
    }
    CHECK(BrushPresets::read().size() == 1);

    // every setting in the reset table is unique and has a plausible default
    std::set<std::string> seen;
    for (const BrushSetting& bs : kBrushSettings) {
      CHECK(seen.insert(bs.knob).second);
      CHECK(bs.def >= 0);
    }
    CHECK(seen.count("size") && seen.count("flow_minimum") && seen.count("dyn_seed"));
    CHECK(!seen.count("color"));  // colour is deliberately not reset or stored

    fs::remove(tmp, ec);
  }

  std::printf(g_fail ? "%d CHECKS FAILED\n" : "layer and preset checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
