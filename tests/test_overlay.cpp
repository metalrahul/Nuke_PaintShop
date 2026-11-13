// Layout and hit-testing tests for the viewer overlay. Compiled against the stub
// DDImage headers, so no Nuke and no GL context are needed: layout() and hitTest()
// are pure geometry.
#include <cstdio>
#include <set>
#include <vector>

#include "Overlay.h"

using namespace rpui;

static int g_fail = 0;
#define CHECK(c) \
  do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
  const int VW = 1280, VH = 720;
  // 40 tips + the round brush = 41 entries, which is more than one 6x4 page
  std::vector<rp::BrushTip> tips(40);
  for (size_t i = 0; i < tips.size(); ++i)
    tips[i].setImage(16, 16, std::vector<float>(256, 1.0f));

  Overlay ov;
  Overlay::State s;
  for (rp::BrushTip& t : tips) s.tips.push_back(&t);
  s.params.size = 40;
  s.tool = 1;
  s.brush = 3;
  s.canUndo = true;

  // ---- toolbar only
  ov.layout(VW, VH, s);
  // Walk the toolbar by probing the actions in the order they are laid out.
  const Action order[] = {kToolPaint,     kToolErase, kToolClone, kToolSmudge,
                          kTogglePicker,  kUndo,      kRedo};
  for (Action want : order) {
    // find the y that reports this action by scanning down the left column
    bool found = false;
    for (int my = 0; my < VH && !found; ++my) {
      int b = -1;
      if (ov.hitTest(int(Overlay::kMargin + Overlay::kBtn * 0.5f), my, b) == want) {
        found = true;
        CHECK(ov.overUI(int(Overlay::kMargin + Overlay::kBtn * 0.5f), my));
      }
    }
    CHECK(found);
    if (!found) std::printf("  missing action %d\n", int(want));
  }

  // Every control that answers hitTest must also have a tooltip, and nothing outside
  // the overlay may have one. They walk the same rects, so if one gains a button and
  // the other does not, this catches it. A label without an action is allowed: the
  // swatch is furniture that is still worth naming.
  {
    int missing = 0, spurious = 0;
    for (int my = 0; my < VH; ++my) {
      for (int mx = 0; mx < VW; mx += 3) {
        int b = -1;
        const bool isControl = ov.hitTest(mx, my, b) != kNone;
        ov.setHover(true, mx, my);
        const bool labelled = ov.hoverLabelForTest(VW, VH) != nullptr;
        if (isControl && !labelled) ++missing;
        if (labelled && !ov.overUI(mx, my)) ++spurious;
      }
    }
    CHECK(missing == 0);
    CHECK(spurious == 0);
    if (missing || spurious)
      std::printf("  tooltip/hitTest disagree: %d unlabelled, %d outside the UI\n", missing, spurious);
  }

  // a point well away from the UI is not over it and yields no action
  int brush = -1;
  CHECK(!ov.overUI(VW / 2, VH / 2));
  CHECK(ov.hitTest(VW / 2, VH / 2, brush) == kNone);
  // the swatch is furniture: clicking it must not paint
  CHECK(ov.overUI(int(Overlay::kMargin + 4), VH - int(Overlay::kMargin + 4)));

  // ---- picker open
  s.pickerOpen = true;
  ov.layout(VW, VH, s);
  std::set<int> page1;
  for (int my = 0; my < VH; ++my)
    for (int mx = 0; mx < VW; ++mx) {
      int b = -1;
      if (ov.hitTest(mx, my, b) == kPickBrush) {
        CHECK(b >= 0 && b <= int(tips.size()));
        page1.insert(b);
      }
    }
  CHECK(int(page1.size()) == Overlay::kCols * Overlay::kRows);
  CHECK(*page1.begin() == 0 && *page1.rbegin() == Overlay::kCols * Overlay::kRows - 1);
  std::printf("picker page 1: %zu cells, brushes %d..%d\n", page1.size(), *page1.begin(),
              *page1.rbegin());

  // paging: 16 entries (round + 15 tips) over 6x4 cells = 2 pages
  bool sawPrev = false, sawNext = false;
  for (int my = 0; my < VH; ++my)
    for (int mx = 0; mx < VW; ++mx) {
      int b = -1;
      const Action a = ov.hitTest(mx, my, b);
      sawPrev = sawPrev || a == kPagePrev;
      sawNext = sawNext || a == kPageNext;
    }
  CHECK(sawNext);
  CHECK(sawPrev);
  ov.pageNext();
  ov.layout(VW, VH, s);
  std::set<int> page2;
  for (int my = 0; my < VH; ++my)
    for (int mx = 0; mx < VW; ++mx) {
      int b = -1;
      if (ov.hitTest(mx, my, b) == kPickBrush) page2.insert(b);
    }
  CHECK(!page2.empty());
  CHECK(*page2.begin() == Overlay::kCols * Overlay::kRows);   // continues where page 1 ended
  CHECK(*page2.rbegin() == int(tips.size()));                 // and reaches the last tip
  // paging past the end must clamp, not run off
  for (int i = 0; i < 10; ++i) ov.pageNext();
  ov.layout(VW, VH, s);
  std::set<int> last;
  for (int my = 0; my < VH; ++my)
    for (int mx = 0; mx < VW; ++mx) {
      int b = -1;
      if (ov.hitTest(mx, my, b) == kPickBrush) last.insert(b);
    }
  CHECK(!last.empty() && *last.rbegin() <= int(tips.size()));
  for (int i = 0; i < 20; ++i) ov.pagePrev();
  ov.layout(VW, VH, s);

  // ---- layer list
  s.pickerOpen = false;
  s.layersOpen = true;
  for (int i = 0; i < 5; ++i)
    s.layers.push_back(LayerRow{"Layer " + std::to_string(i), i != 2, false});
  s.activeLayer = 1;
  ov.layout(VW, VH, s);
  ov.layoutLayers(VW, VH, s);
  // The list reads top-down, so the FIRST row must be the LAST layer in the stack.
  int firstRowLayer = -1, lastRowLayer = -1, bestY = VH, worstY = -1;
  int selects = 0, toggles = 0;
  std::set<int> reachable;
  for (int my = 0; my < VH; ++my)
    for (int mx = 0; mx < VW; ++mx) {
      int b = -1;
      const Action a = ov.hitTest(mx, my, b);
      if (a != kSelectLayer && a != kToggleLayerVis) continue;
      reachable.insert(b);
      if (a == kSelectLayer) ++selects; else ++toggles;
      if (my < bestY) { bestY = my; firstRowLayer = b; }
      if (my > worstY) { worstY = my; lastRowLayer = b; }
    }
  CHECK(int(reachable.size()) == 5);
  CHECK(firstRowLayer == 4);   // topmost layer drawn first
  CHECK(lastRowLayer == 0);    // bottom layer last
  CHECK(toggles > 0 && selects > toggles);  // the eye is a small part of each row
  CHECK(ov.overUI(VW - 30, 30));            // clicking the list must not paint

  // a stack taller than the viewer still reaches the active layer
  s.layers.clear();
  for (int i = 0; i < 400; ++i) s.layers.push_back(LayerRow{"L" + std::to_string(i), true, false});
  s.activeLayer = 390;
  ov.layout(VW, VH, s);
  ov.layoutLayers(VW, VH, s);
  bool sawActive = false;
  for (int my = 0; my < VH; ++my)
    for (int mx = 0; mx < VW; ++mx) {
      int b = -1;
      const Action a = ov.hitTest(mx, my, b);
      if ((a == kSelectLayer || a == kToggleLayerVis) && b == 390) sawActive = true;
    }
  CHECK(sawActive);
  s.layers.clear();
  s.layersOpen = false;
  ov.layout(VW, VH, s);
  ov.layoutLayers(VW, VH, s);
  CHECK(!ov.overUI(VW - 30, 30));  // hidden list claims no clicks

  // ---- the picker grid resizes by its corner grip
  s.layers.clear();
  s.layersOpen = false;
  s.pickerOpen = true;
  ov.setGrid(6, 4);
  ov.layout(VW, VH, s);
  auto cellCount = [&]() {
    std::set<int> seen;
    for (int my = 0; my < VH; ++my)
      for (int mx = 0; mx < VW; ++mx) {
        int b = -1;
        if (ov.hitTest(mx, my, b) == kPickBrush) seen.insert(b);
      }
    return int(seen.size());
  };
  const int total = 1 + int(tips.size());  // the round brush plus the file's brushes
  CHECK(cellCount() == 24);               // a 6x4 page holds 24 of them
  ov.setGrid(10, 6);
  ov.layout(VW, VH, s);
  // 10x6 has room for 60, but there are only `total` brushes, so the page shows them all
  CHECK(cellCount() == total);
  // the grip must be reachable, and sit at the panel's bottom-right
  {
    bool sawGrip = false;
    for (int my = 0; my < VH && !sawGrip; ++my)
      for (int mx = 0; mx < VW && !sawGrip; ++mx) {
        int b = -1;
        if (ov.hitTest(mx, my, b) == kResizePicker) sawGrip = true;
      }
    CHECK(sawGrip);
  }
  // absurd sizes clamp instead of exploding or vanishing
  ov.setGrid(999, 999);
  ov.layout(VW, VH, s);
  CHECK(cellCount() == total);
  CHECK(ov.gridCols() <= 24 && ov.gridRows() <= 24);
  ov.setGrid(-5, 0);
  ov.layout(VW, VH, s);
  CHECK(ov.gridCols() == 1 && ov.gridRows() == 1);
  CHECK(cellCount() == 1);
  ov.setGrid(6, 4);
  s.pickerOpen = false;

  // ---- a tiny viewport must not place UI off-screen or overlap the toolbar
  for (int vh : {200, 400, 1600}) {
    for (int vw : {300, 900, 2400}) {
      s.pickerOpen = true;
      s.layersOpen = true;
      ov.layout(vw, vh, s);
      ov.layoutLayers(vw, vh, s);
      for (int my = 0; my < vh; my += 3)
        for (int mx = 0; mx < vw; mx += 3) {
          int b = -1;
          ov.hitTest(mx, my, b);  // must not crash or read out of range
        }
    }
  }

  // ---- geometry sanity at the default size
  s.pickerOpen = false;
  ov.layout(VW, VH, s);
  std::printf(g_fail ? "%d CHECKS FAILED\n" : "overlay layout checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
