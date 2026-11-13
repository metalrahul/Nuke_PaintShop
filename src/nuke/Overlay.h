// NPaintShop - viewer overlay: toolbar, brush picker, swatch and cursor preview.
//
// The NDK exposes no API for adding a native viewer toolbar (ViewerContext::menu() is
// pop-up menus only), so this draws its own UI in OpenGL inside Knob::draw_handle and
// hit-tests the mouse itself. Everything here is screen-space in widget pixels, so the
// UI keeps a constant size as the viewer zooms.
//
// All GL state is saved and restored; nothing leaks into Nuke's own viewer drawing.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "DDImage/gl.h"
#include "Preview.h"

namespace rpui {

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;  // y measured from the BOTTOM of the viewport
  bool contains(float px, float py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

enum Action {
  kNone = 0, kToolPaint, kToolErase, kToolClone, kToolSmudge, kTogglePicker, kToggleLayers,
  kUndo, kRedo, kPagePrev, kPageNext, kPickBrush, kSelectLayer, kToggleLayerVis,
  kPickColor, kResizePicker
};

// What the overlay needs to know about one layer to draw the list.
struct LayerRow {
  std::string name;
  bool visible = true, locked = false;
};

// ---------------------------------------------------------------- GL helpers

// Switches to a pixel-space projection for the duration of its scope and restores
// whatever Nuke had set up afterwards.
struct ScreenSpace {
  GLint vp[4] = {0, 0, 0, 0};
  ScreenSpace() {
    glGetIntegerv(GL_VIEWPORT, vp);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, vp[2] > 0 ? vp[2] : 1, 0, vp[3] > 0 ? vp[3] : 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // textures below are premultiplied
  }
  ~ScreenSpace() {
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
  }
  int w() const { return vp[2]; }
  int h() const { return vp[3]; }
};

// A lazily uploaded 8-bit RGBA texture. `key` identifies the content: when it changes
// the texture is re-uploaded, otherwise the cached one is reused.
struct Tex {
  GLuint id = 0;
  uint64_t key = 0;
  int w = 0, h = 0;

  bool current(uint64_t k) const { return id != 0 && key == k && glIsTexture(id); }

  void upload(uint64_t k, int width, int height, const std::vector<unsigned char>& rgba) {
    if (width <= 0 || height <= 0) return;
    if (id == 0 || !glIsTexture(id)) glGenTextures(1, &id);
    if (id == 0) return;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    key = k;
    w = width;
    h = height;
  }
};

inline unsigned char toByte(float v) {
  const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return (unsigned char)(c * 255.0f + 0.5f);
}

// Premultiplied float RGBA -> bytes.
inline void packRGBA(const std::vector<float>& src, std::vector<unsigned char>& dst) {
  dst.resize(src.size());
  for (size_t i = 0; i < src.size(); ++i) dst[i] = toByte(src[i]);
}

// Coverage -> a premultiplied tinted RGBA image.
inline void packCoverage(const std::vector<float>& a, float r, float g, float b,
                         std::vector<unsigned char>& dst) {
  dst.resize(a.size() * 4);
  for (size_t i = 0; i < a.size(); ++i) {
    const float v = a[i] < 0.0f ? 0.0f : (a[i] > 1.0f ? 1.0f : a[i]);
    dst[i * 4 + 0] = toByte(r * v);
    dst[i * 4 + 1] = toByte(g * v);
    dst[i * 4 + 2] = toByte(b * v);
    dst[i * 4 + 3] = toByte(v);
  }
}

inline void fillRect(const Rect& q, float r, float g, float b, float a) {
  glColor4f(r * a, g * a, b * a, a);  // premultiplied, matching the blend mode
  glBegin(GL_QUADS);
  glVertex2f(q.x, q.y);
  glVertex2f(q.x + q.w, q.y);
  glVertex2f(q.x + q.w, q.y + q.h);
  glVertex2f(q.x, q.y + q.h);
  glEnd();
}

inline void strokeRect(const Rect& q, float r, float g, float b, float a, float width = 1.0f) {
  glColor4f(r * a, g * a, b * a, a);
  glLineWidth(width);
  glBegin(GL_LINE_LOOP);
  glVertex2f(q.x + 0.5f, q.y + 0.5f);
  glVertex2f(q.x + q.w - 0.5f, q.y + 0.5f);
  glVertex2f(q.x + q.w - 0.5f, q.y + q.h - 0.5f);
  glVertex2f(q.x + 0.5f, q.y + q.h - 0.5f);
  glEnd();
  glLineWidth(1.0f);
}

inline void blitTex(const Tex& t, const Rect& q, float alpha = 1.0f) {
  if (t.id == 0) return;
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, t.id);
  glColor4f(alpha, alpha, alpha, alpha);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0); glVertex2f(q.x, q.y);
  glTexCoord2f(1, 0); glVertex2f(q.x + q.w, q.y);
  glTexCoord2f(1, 1); glVertex2f(q.x + q.w, q.y + q.h);
  glTexCoord2f(0, 1); glVertex2f(q.x, q.y + q.h);
  glEnd();
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------- icons
// Drawn as vector shapes in a 0..1 box so they scale with the button size.

inline void iconBrush(const Rect& q) {
  const float x = q.x, y = q.y, s = q.w;
  glBegin(GL_QUADS);  // handle
  glVertex2f(x + 0.60f * s, y + 0.86f * s);
  glVertex2f(x + 0.82f * s, y + 0.64f * s);
  glVertex2f(x + 0.62f * s, y + 0.44f * s);
  glVertex2f(x + 0.40f * s, y + 0.66f * s);
  glEnd();
  glBegin(GL_TRIANGLES);  // bristles
  glVertex2f(x + 0.38f * s, y + 0.64f * s);
  glVertex2f(x + 0.60f * s, y + 0.42f * s);
  glVertex2f(x + 0.18f * s, y + 0.18f * s);
  glEnd();
}

inline void iconEraser(const Rect& q) {
  const float x = q.x, y = q.y, s = q.w;
  glBegin(GL_QUADS);  // body
  glVertex2f(x + 0.18f * s, y + 0.34f * s);
  glVertex2f(x + 0.56f * s, y + 0.34f * s);
  glVertex2f(x + 0.82f * s, y + 0.60f * s);
  glVertex2f(x + 0.44f * s, y + 0.60f * s);
  glEnd();
  glBegin(GL_QUADS);  // base
  glVertex2f(x + 0.18f * s, y + 0.22f * s);
  glVertex2f(x + 0.56f * s, y + 0.22f * s);
  glVertex2f(x + 0.56f * s, y + 0.34f * s);
  glVertex2f(x + 0.18f * s, y + 0.34f * s);
  glEnd();
}

inline void circleAt(float cx, float cy, float r, int segs, bool filled) {
  glBegin(filled ? GL_TRIANGLE_FAN : GL_LINE_LOOP);
  if (filled) glVertex2f(cx, cy);
  for (int i = 0; i <= segs; ++i) {
    const float a = float(i) * 6.2831853f / float(segs);
    glVertex2f(cx + r * std::cos(a), cy + r * std::sin(a));
  }
  glEnd();
}

// A fingertip dragging colour: a tapering smear that fades out behind a round tip,
// which is what the tool does to the pixels.
inline void iconSmudge(const Rect& q) {
  const float x = q.x, y = q.y, s = q.w;
  circleAt(x + 0.68f * s, y + 0.34f * s, 0.15f * s, 20, true);  // the tip
  glBegin(GL_TRIANGLES);                                        // the smear behind it
  glVertex2f(x + 0.68f * s, y + 0.20f * s);
  glVertex2f(x + 0.68f * s, y + 0.48f * s);
  glVertex2f(x + 0.16f * s, y + 0.74f * s);
  glEnd();
  glBegin(GL_LINE_STRIP);  // a wisp trailing off, to read as fading rather than solid
  glVertex2f(x + 0.20f * s, y + 0.62f * s);
  glVertex2f(x + 0.34f * s, y + 0.70f * s);
  glVertex2f(x + 0.52f * s, y + 0.74f * s);
  glEnd();
}

inline void iconClone(const Rect& q) {
  const float x = q.x, y = q.y, s = q.w;
  glBegin(GL_QUADS);  // stamp head
  glVertex2f(x + 0.22f * s, y + 0.30f * s);
  glVertex2f(x + 0.78f * s, y + 0.30f * s);
  glVertex2f(x + 0.78f * s, y + 0.42f * s);
  glVertex2f(x + 0.22f * s, y + 0.42f * s);
  glEnd();
  glBegin(GL_QUADS);  // shaft
  glVertex2f(x + 0.44f * s, y + 0.42f * s);
  glVertex2f(x + 0.56f * s, y + 0.42f * s);
  glVertex2f(x + 0.56f * s, y + 0.62f * s);
  glVertex2f(x + 0.44f * s, y + 0.62f * s);
  glEnd();
  circleAt(x + 0.50f * s, y + 0.70f * s, 0.14f * s, 20, true);
}

inline void iconGrid(const Rect& q) {
  const float x = q.x, y = q.y, s = q.w, d = 0.16f * s;
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j) {
      const float cx = x + (0.34f + 0.32f * i) * s, cy = y + (0.34f + 0.32f * j) * s;
      glBegin(GL_QUADS);
      glVertex2f(cx - d * 0.5f, cy - d * 0.5f);
      glVertex2f(cx + d * 0.5f, cy - d * 0.5f);
      glVertex2f(cx + d * 0.5f, cy + d * 0.5f);
      glVertex2f(cx - d * 0.5f, cy + d * 0.5f);
      glEnd();
    }
}

inline void iconEyedropper(const Rect& q) {
  const float x = q.x, y = q.y, s = q.w;
  glBegin(GL_QUADS);  // barrel
  glVertex2f(x + 0.44f * s, y + 0.52f * s);
  glVertex2f(x + 0.70f * s, y + 0.78f * s);
  glVertex2f(x + 0.60f * s, y + 0.88f * s);
  glVertex2f(x + 0.34f * s, y + 0.62f * s);
  glEnd();
  glBegin(GL_TRIANGLES);  // tip
  glVertex2f(x + 0.14f * s, y + 0.16f * s);
  glVertex2f(x + 0.44f * s, y + 0.46f * s);
  glVertex2f(x + 0.30f * s, y + 0.60f * s);
  glEnd();
}

inline void iconLayers(const Rect& q) {
  const float x = q.x, y = q.y, s = q.w;
  for (int i = 0; i < 3; ++i) {
    const float oy = y + (0.26f + 0.17f * i) * s;
    glBegin(GL_LINE_LOOP);
    glVertex2f(x + 0.22f * s, oy);
    glVertex2f(x + 0.50f * s, oy - 0.10f * s);
    glVertex2f(x + 0.78f * s, oy);
    glVertex2f(x + 0.50f * s, oy + 0.10f * s);
    glEnd();
  }
}

// Curved arrow; dir = -1 for undo (points left), +1 for redo.
inline void iconArrow(const Rect& q, int dir) {
  const float x = q.x, y = q.y, s = q.w;
  const float cx = x + 0.5f * s, cy = y + 0.44f * s, r = 0.26f * s;
  glLineWidth(2.0f);
  glBegin(GL_LINE_STRIP);
  for (int i = 0; i <= 18; ++i) {
    const float a = 3.14159265f * (0.08f + 0.84f * float(i) / 18.0f);
    glVertex2f(cx + dir * r * std::cos(a), cy + r * std::sin(a));
  }
  glEnd();
  glLineWidth(1.0f);
  const float tipX = cx + dir * r * std::cos(3.14159265f * 0.92f);
  const float tipY = cy + r * std::sin(3.14159265f * 0.92f);
  glBegin(GL_TRIANGLES);
  glVertex2f(tipX, tipY - 0.17f * s);
  glVertex2f(tipX - dir * 0.16f * s, tipY + 0.04f * s);
  glVertex2f(tipX + dir * 0.10f * s, tipY + 0.08f * s);
  glEnd();
}

// ---------------------------------------------------------------- the overlay

class Overlay {
 public:
  struct State {
    int tool = 0;                                 // 0 paint, 1 erase, 2 clone
    int brush = 0;                                // 0 round, 1.. index into tips
    bool pickerOpen = false;
    bool layersOpen = true;
    bool colorPicking = false;   // armed: the next click samples a colour
    float color[3] = {1, 1, 1};  // shown in the toolbar swatch
    bool canUndo = false, canRedo = false;
    // One entry per brush menu index above 0; null means the round brush.
    std::vector<const rp::BrushTip*> tips;
    rp::BrushParams params;                       // drives the swatch
    std::vector<LayerRow> layers;                 // bottom to top, as the stack is stored
    int activeLayer = 0;
  };

  // Where the pointer is, in widget pixels with the origin TOP-left exactly as
  // ViewerContext reports it. Flipped to GL coordinates inside draw(), once layout has
  // run, so this never depends on a viewport height left over from the last frame.
  void setHover(bool have, int mouseX, int mouseY) {
    haveHover_ = have;
    hoverMx_ = mouseX;
    hoverMy_ = mouseY;
  }

  static constexpr float kBtn = 30.0f, kGap = 4.0f, kMargin = 10.0f;
  static constexpr int kCell = 56, kCols = 6, kRows = 4;
  static constexpr float kPageH = 18.0f;
  static constexpr float kRowH = 22.0f, kListW = 190.0f, kEye = 14.0f;
  static constexpr float kGrip = 14.0f;

  // Lays the UI out for a viewport of the given size. Pure geometry, no GL, so the
  // click handler can reproduce exactly what was drawn.
  void layout(int vw, int vh, const State& s) {
    vw_ = vw; vh_ = vh;
    const float top = float(vh) - kMargin;
    float y = top - kBtn;
    auto put = [&](Rect& r) { r = Rect{kMargin, y, kBtn, kBtn}; y -= kBtn + kGap; };
    put(bPaint_); put(bErase_); put(bClone_); put(bSmudge_);
    y -= kGap * 2.0f;
    put(bPicker_); put(bLayers_);
    y -= kGap * 2.0f;
    put(bUndo_); put(bRedo_);

    swatch_ = Rect{kMargin, kMargin, 184.0f, 46.0f};

    cells_.clear();
    cellBrush_.clear();
    bPrev_ = bNext_ = Rect{};
    if (!s.pickerOpen) { picker_ = Rect{}; pages_ = 1; return; }

    const int count = 1 + int(s.tips.size());  // the round brush plus the file's brushes
    // The grid shrinks to the brushes it has and to the space available, so it never
    // runs off a small viewer and never reserves empty rows.
    const float left = kMargin + kBtn + kGap * 2.0f;
    const float pageStrip = kPageH + kGap;
    const int maxCols = int((float(vw) - left - kMargin - 2.0f * kGap) / float(kCell));
    const int cols = std::max(1, std::min(std::min(wantCols_, maxCols), count));
    const int maxRows = int((float(vh) - 2.0f * kMargin - 2.0f * kGap - pageStrip) / float(kCell));
    const int rows = std::max(1, std::min(wantRows_, maxRows));
    const int perPage = cols * rows;
    pages_ = (count + perPage - 1) / perPage;
    if (page_ >= pages_) page_ = pages_ - 1;
    if (page_ < 0) page_ = 0;
    const int onPage = std::min(count - page_ * perPage, perPage);
    const int usedRows = std::max(1, (onPage + cols - 1) / cols);
    const float showPage = pages_ > 1 ? pageStrip : 0.0f;
    const float pw = float(cols) * kCell + 2.0f * kGap;
    const float ph = float(usedRows) * kCell + 2.0f * kGap + showPage;
    picker_ = Rect{left, std::max(kMargin, top - ph), pw, ph};
    for (int i = 0; i < onPage; ++i) {
      const int cx = i % cols, cy = i / cols;
      cells_.push_back(Rect{picker_.x + kGap + float(cx) * kCell,
                            picker_.y + ph - kGap - float(cy + 1) * kCell,
                            float(kCell), float(kCell)});
      cellBrush_.push_back(page_ * perPage + i);
    }
    if (pages_ > 1) {
      bPrev_ = Rect{picker_.x + kGap, picker_.y + kGap, 26.0f, kPageH};
      bNext_ = Rect{picker_.x + kGap + 30.0f, picker_.y + kGap, 26.0f, kPageH};
    }
    // Drag handle in the panel's bottom-right corner, to grow the grid.
    grip_ = Rect{picker_.x + pw - kGrip, picker_.y, kGrip, kGrip};
  }

  // Grid size, driven by dragging the corner grip. Clamped here so the layout above
  // only ever has to clip to the viewport, never to nonsense.
  void setGrid(int cols, int rows) {
    wantCols_ = std::max(1, std::min(cols, 24));
    wantRows_ = std::max(1, std::min(rows, 24));
  }
  int gridCols() const { return wantCols_; }
  int gridRows() const { return wantRows_; }
  float cellSize() const { return float(kCell); }
  // Where the grip currently is, so a drag can be measured from its corner.
  Rect grip() const { return grip_; }

  // (public for the layout tests)
  // The layer list sits top right, drawn top to bottom with the topmost layer first,
  // which is the order people expect even though the stack is stored bottom up.
  void layoutLayers(int vw, int vh, const State& s) {
    rows_.clear();
    rowLayer_.clear();
    if (!s.layersOpen || s.layers.empty()) { list_ = Rect{}; return; }
    const int n = int(s.layers.size());
    const int maxRows = std::max(1, int((float(vh) - 2.0f * kMargin - kGap * 2.0f) / kRowH));
    const int shown = std::min(n, maxRows);
    // Keep the active layer on screen when the stack is taller than the viewer.
    const int activeRow = n - 1 - std::max(0, std::min(s.activeLayer, n - 1));
    int first = std::max(0, std::min(listTop_, n - shown));
    if (activeRow < first) first = activeRow;
    if (activeRow >= first + shown) first = activeRow - shown + 1;
    listTop_ = first;
    const float h = float(shown) * kRowH + 2.0f * kGap;
    list_ = Rect{float(vw) - kMargin - kListW, float(vh) - kMargin - h, kListW, h};
    for (int i = 0; i < shown; ++i) {
      const int row = first + i;          // 0 = topmost layer
      const int idx = n - 1 - row;        // index into the bottom-up stack
      rows_.push_back(Rect{list_.x + kGap, list_.y + h - kGap - float(i + 1) * kRowH,
                           kListW - 2.0f * kGap, kRowH});
      rowLayer_.push_back(idx);
    }
  }

  // Draws everything. Must be called with the viewer's GL context current.
  void draw(const State& s) {
    ScreenSpace ss;
    if (ss.w() <= 0 || ss.h() <= 0) return;
    layout(ss.w(), ss.h(), s);

    drawButton(bPaint_, s.tool == 0, true, 0);
    drawButton(bErase_, s.tool == 1, true, 1);
    drawButton(bClone_, s.tool == 2, true, 2);
    drawButton(bSmudge_, s.tool == 3, true, 8);
    // The eyedropper has no button of its own - Alt+C arms it, and the swatch at the
    // bottom of the toolbar already shows the current colour. A ring on the paint
    // button is the cue that the next click will sample instead of paint.
    if (s.colorPicking)
      strokeRect(Rect{bPaint_.x - 2.0f, bPaint_.y - 2.0f, bPaint_.w + 4.0f, bPaint_.h + 4.0f},
                 1.0f, 0.85f, 0.25f, 0.95f, 2.0f);
    drawButton(bPicker_, s.pickerOpen, true, 3);
    drawButton(bLayers_, s.layersOpen, true, 6);
    drawButton(bUndo_, false, s.canUndo, 4);
    drawButton(bRedo_, false, s.canRedo, 5);

    layoutLayers(ss.w(), ss.h(), s);
    if (s.pickerOpen) drawPicker(s);
    drawLayers(s);
    drawSwatch(s);
    drawTooltip();  // last, so nothing can draw over it
  }

  // Labels whatever the pointer is over. The icons are small and the eyedropper no
  // longer has a button of its own, so without this there is nothing telling you what
  // the toolbar does. Shown as soon as the pointer is over a control rather than after
  // a delay: a stationary mouse generates no events here, so a timed reveal would
  // never fire.
  void drawTooltip() {
    if (!haveHover_) return;
    hoverX_ = float(hoverMx_);
    hoverY_ = float(vh_) - float(hoverMy_);
    const char* label = hoverLabel();
    if (!label) return;

    // gl_text has no measuring call, so the box is sized from the character count.
    // Slightly generous is fine; slightly tight would clip the text.
    const float charW = 6.2f, padX = 7.0f, h = 18.0f;
    const float w = float(std::strlen(label)) * charW + padX * 2.0f;
    float x = hoverX_ + 14.0f, y = hoverY_ - h - 6.0f;
    if (x + w > float(vw_) - 2.0f) x = hoverX_ - w - 10.0f;  // flip rather than run off
    if (y < 2.0f) y = hoverY_ + 14.0f;
    x = std::max(2.0f, x);
    y = std::min(y, float(vh_) - h - 2.0f);

    const Rect box{x, y, w, h};
    fillRect(box, 0.05f, 0.05f, 0.06f, 0.94f);
    strokeRect(box, 0.75f, 0.75f, 0.78f, 0.55f);
    glColor4f(0.95f, 0.95f, 0.95f, 1.0f);
    DD::Image::gl_text(label, x + padX, y + 5.0f);
  }

  // Resolves the stored hover position the way draw() does, for tests that check the
  // labels against hitTest without a GL context.
  const char* hoverLabelForTest(int vw, int vh) {
    vw_ = vw;
    vh_ = vh;
    hoverX_ = float(hoverMx_);
    hoverY_ = float(vh_) - float(hoverMy_);
    return hoverLabel();
  }

  // What the pointer is over, or null. Mirrors hitTest so the two cannot disagree
  // about which control is where.
  const char* hoverLabel() const {
    const float x = hoverX_, y = hoverY_;
    if (bPaint_.contains(x, y)) return "Brush  (Alt+B)";
    if (bErase_.contains(x, y)) return "Eraser  (Alt+E)";
    if (bClone_.contains(x, y)) return "Clone stamp  (Alt+S)  -  Ctrl+click sets the source";
    if (bSmudge_.contains(x, y)) return "Smudge  (Alt+U)";
    if (bPicker_.contains(x, y)) return "Brushes  (Alt+M)  -  drag the corner to resize";
    if (bLayers_.contains(x, y)) return "Layers  (Alt+L)";
    if (bUndo_.contains(x, y)) return "Undo";
    if (bRedo_.contains(x, y)) return "Redo";
    if (grip_.w > 0 && grip_.contains(x, y)) return "Drag to resize the brush list";
    if (bPrev_.w > 0 && bPrev_.contains(x, y)) return "Previous page of brushes";
    if (bNext_.w > 0 && bNext_.contains(x, y)) return "Next page of brushes";
    if (swatch_.w > 0 && swatch_.contains(x, y))
      return "Current brush and colour  -  Alt+C picks a colour from the image";
    for (const Rect& r : rows_) {
      if (!r.contains(x, y)) continue;
      return x < r.x + kEye + 4.0f ? "Show or hide this layer" : "Click to paint on this layer";
    }
    return nullptr;
  }

  // Mouse position in widget pixels, origin TOP-left (as ViewerContext reports it).
  // Returns the action, and for kPickBrush sets brushOut to the chosen brush index.
  Action hitTest(int mouseX, int mouseY, int& brushOut) const {
    const float x = float(mouseX), y = float(vh_) - float(mouseY);  // to GL origin
    if (bPaint_.contains(x, y)) return kToolPaint;
    if (bErase_.contains(x, y)) return kToolErase;
    if (bClone_.contains(x, y)) return kToolClone;
    if (bSmudge_.contains(x, y)) return kToolSmudge;
    if (bPicker_.contains(x, y)) return kTogglePicker;
    if (bLayers_.contains(x, y)) return kToggleLayers;
    if (bUndo_.contains(x, y)) return kUndo;
    if (bRedo_.contains(x, y)) return kRedo;
    if (grip_.w > 0 && grip_.contains(x, y)) return kResizePicker;
    if (bPrev_.w > 0 && bPrev_.contains(x, y)) return kPagePrev;
    if (bNext_.w > 0 && bNext_.contains(x, y)) return kPageNext;
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (!rows_[i].contains(x, y)) continue;
      brushOut = rowLayer_[i];
      const Rect eye{rows_[i].x + 3.0f, rows_[i].y + (kRowH - kEye) * 0.5f, kEye, kEye};
      return eye.contains(x, y) ? kToggleLayerVis : kSelectLayer;
    }
    for (size_t i = 0; i < cells_.size(); ++i)
      if (cells_[i].contains(x, y)) { brushOut = cellBrush_[i]; return kPickBrush; }
    // A click anywhere else on the picker panel is swallowed so it does not paint.
    if (picker_.w > 0 && picker_.contains(x, y)) return kNone;
    return kNone;
  }

  // True if the point is over any overlay furniture, i.e. must not start a stroke.
  bool overUI(int mouseX, int mouseY) const {
    const float x = float(mouseX), y = float(vh_) - float(mouseY);
    const Rect* all[] = {&bPaint_, &bErase_,  &bClone_, &bSmudge_, &bPicker_,
                         &bLayers_, &bUndo_,   &bRedo_,  &swatch_,   &picker_, &list_};
    for (const Rect* r : all)
      if (r->w > 0 && r->contains(x, y)) return true;
    return false;
  }

  void pagePrev() { if (page_ > 0) --page_; }
  void pageNext() { if (page_ + 1 < pages_) ++page_; }

  // Draws the brush tip under the cursor, in IMAGE space. The rotation and roundness
  // are baked into the texture, so the cursor shows the dab the brush will actually
  // stamp rather than a generic circle.
  void drawCursorTip(float cx, float cy, const rp::DabShape& shape, const rp::BrushTip* tip) {
    const uint64_t k = shapeKey(tip, shape);
    if (!cursorTex_.current(k)) {
      rp::DabShape s = shape;
      s.diameter = kCursorRes;
      const rp::IRect b = rp::dabBounds(tip, 0.0f, 0.0f, s);
      const int tw = b.x1 - b.x0, th = b.y1 - b.y0;
      if (tw <= 0 || th <= 0 || tw > 4096 || th > 4096) return;
      std::vector<float> cov(size_t(tw) * th, 0.0f);
      rp::rasterizeDab(tip, -float(b.x0), -float(b.y0), s, rp::IRect{0, 0, tw, th}, cov.data());
      std::vector<unsigned char> bytes;
      packCoverage(cov, 1.0f, 1.0f, 1.0f, bytes);
      cursorTex_.upload(k, tw, th, bytes);
      cursorHW_ = float(tw) * 0.5f / kCursorRes;  // half-extent per unit diameter
      cursorHH_ = float(th) * 0.5f / kCursorRes;
    }
    if (cursorTex_.id == 0) return;
    const float hw = cursorHW_ * shape.diameter, hh = cursorHH_ * shape.diameter;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    blitTex(cursorTex_, Rect{cx - hw, cy - hh, hw * 2.0f, hh * 2.0f}, 0.45f);
    glPopAttrib();
  }

 private:
  static constexpr float kCursorRes = 128.0f;

  static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

  // Identity of a dab's full shape, so the cursor texture is rebuilt when it changes.
  static uint64_t shapeKey(const rp::BrushTip* tip, const rp::DabShape& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = mix(h, uint64_t(reinterpret_cast<uintptr_t>(tip)));
    h = mix(h, uint64_t(int(s.hardness * 1000.0f)));
    h = mix(h, uint64_t(int(s.angle * 1000.0f) & 0xffffff));
    h = mix(h, uint64_t(int(s.roundness * 1000.0f)));
    h = mix(h, uint64_t(s.flipX ? 1 : 0) | (uint64_t(s.flipY ? 1 : 0) << 1));
    return h ? h : 1;
  }

  static uint64_t tipKey(const rp::BrushTip* tip, float hardness) {
    uint64_t k = uint64_t(reinterpret_cast<uintptr_t>(tip));
    k ^= uint64_t(int(hardness * 1000.0f)) * 0x9e3779b97f4a7c15ull;
    return k ? k : 1;
  }

  static void buildTipTex(Tex& tex, uint64_t key, const rp::BrushTip* tip, float hardness,
                          int longest) {
    int tw = longest, th = longest;
    if (tip && tip->sampled()) {
      const float m = float(std::max(tip->width(), tip->height()));
      tw = std::max(1, int(float(longest) * tip->width() / m));
      th = std::max(1, int(float(longest) * tip->height() / m));
    }
    std::vector<float> cov(size_t(tw) * th, 0.0f);
    rp::DabShape sh;
    sh.diameter = float(longest);
    sh.hardness = hardness;
    rp::rasterizeDab(tip, float(tw) * 0.5f, float(th) * 0.5f, sh, rp::IRect{0, 0, tw, th},
                     cov.data());
    std::vector<unsigned char> bytes;
    packCoverage(cov, 1.0f, 1.0f, 1.0f, bytes);
    tex.upload(key, tw, th, bytes);
  }

  void drawButton(const Rect& r, bool active, bool enabled, int icon) {
    if (r.w <= 0) return;
    fillRect(r, active ? 0.32f : 0.16f, active ? 0.42f : 0.16f, active ? 0.58f : 0.17f, 0.88f);
    strokeRect(r, 0.0f, 0.0f, 0.0f, 0.55f);
    const float g = enabled ? (active ? 1.0f : 0.82f) : 0.38f;
    glColor4f(g, g, g, 1.0f);
    switch (icon) {
      case 0: iconBrush(r); break;
      case 1: iconEraser(r); break;
      case 2: iconClone(r); break;
      case 3: iconGrid(r); break;
      case 4: iconArrow(r, -1); break;
      case 5: iconArrow(r, +1); break;
      case 6: iconLayers(r); break;
      case 8: iconSmudge(r); break;
      default: iconEyedropper(r); break;
    }
  }

  void drawPicker(const State& s) {
    fillRect(picker_, 0.09f, 0.09f, 0.10f, 0.93f);
    strokeRect(picker_, 0.0f, 0.0f, 0.0f, 0.7f);
    for (size_t i = 0; i < cells_.size(); ++i) {
      const int idx = cellBrush_[i];
      const rp::BrushTip* tip =
          (idx > 0 && size_t(idx - 1) < s.tips.size()) ? s.tips[size_t(idx - 1)] : nullptr;
      const Rect& c = cells_[i];
      const bool sel = idx == s.brush;
      fillRect(Rect{c.x + 2, c.y + 2, c.w - 4, c.h - 4}, sel ? 0.30f : 0.16f,
               sel ? 0.40f : 0.16f, sel ? 0.56f : 0.17f, 0.95f);
      if (size_t(i) >= cellTex_.size()) cellTex_.resize(i + 1);
      const uint64_t k = tipKey(tip, s.params.hardness);
      if (!cellTex_[i].current(k)) buildTipTex(cellTex_[i], k, tip, s.params.hardness, 64);
      blitTex(cellTex_[i], Rect{c.x + 5, c.y + 5, c.w - 10, c.h - 10}, 0.95f);
      if (sel) strokeRect(Rect{c.x + 2, c.y + 2, c.w - 4, c.h - 4}, 0.55f, 0.75f, 1.0f, 1.0f, 2.0f);
    }
    if (bPrev_.w > 0) {
      drawPage(bPrev_, -1, page_ > 0);
      drawPage(bNext_, +1, page_ + 1 < pages_);
    }
    // corner grip: three diagonal ticks, the usual "drag to resize" cue
    glColor4f(0.75f, 0.78f, 0.85f, 0.9f);
    glBegin(GL_LINES);
    for (int i = 1; i <= 3; ++i) {
      const float d = float(i) * 4.0f;
      glVertex2f(grip_.x + grip_.w - d, grip_.y);
      glVertex2f(grip_.x + grip_.w, grip_.y + d);
    }
    glEnd();
  }

  void drawLayers(const State& s) {
    if (list_.w <= 0 || rows_.empty()) return;
    fillRect(list_, 0.09f, 0.09f, 0.10f, 0.92f);
    strokeRect(list_, 0.0f, 0.0f, 0.0f, 0.7f);
    for (size_t i = 0; i < rows_.size(); ++i) {
      const int idx = rowLayer_[i];
      if (idx < 0 || idx >= int(s.layers.size())) continue;
      const LayerRow& L = s.layers[size_t(idx)];
      const Rect& r = rows_[i];
      const bool sel = idx == s.activeLayer;
      if (sel) fillRect(r, 0.30f, 0.40f, 0.56f, 0.95f);
      // visibility box
      const Rect eye{r.x + 3.0f, r.y + (kRowH - kEye) * 0.5f, kEye, kEye};
      fillRect(eye, 0.06f, 0.06f, 0.07f, 0.9f);
      if (L.visible) fillRect(Rect{eye.x + 3, eye.y + 3, kEye - 6, kEye - 6}, 0.85f, 0.88f, 0.95f, 1.0f);
      strokeRect(eye, 0.0f, 0.0f, 0.0f, 0.8f);
      const float g = L.visible ? (sel ? 1.0f : 0.86f) : 0.45f;
      glColor4f(g, g, g, 1.0f);
      std::string label = L.name;
      if (L.locked) label += "  [L]";
      if (label.size() > 22) label = label.substr(0, 21) + "\u2026";
      DD::Image::gl_text(label.c_str(), r.x + kEye + 9.0f, r.y + 6.0f);
    }
  }

  void drawPage(const Rect& r, int dir, bool enabled) {
    fillRect(r, 0.18f, 0.18f, 0.19f, 0.9f);
    const float g = enabled ? 0.85f : 0.35f;
    glColor4f(g, g, g, 1.0f);
    glBegin(GL_TRIANGLES);
    glVertex2f(r.x + (dir < 0 ? r.w * 0.72f : r.w * 0.28f), r.y + r.h * 0.2f);
    glVertex2f(r.x + (dir < 0 ? r.w * 0.72f : r.w * 0.28f), r.y + r.h * 0.8f);
    glVertex2f(r.x + (dir < 0 ? r.w * 0.28f : r.w * 0.72f), r.y + r.h * 0.5f);
    glEnd();
  }

  void drawSwatch(const State& s) {
    const uint64_t k = swatchKey(s.params);
    if (!swatchTex_.current(k)) {
      std::vector<float> rgba;
      const int sw = int(swatch_.w), sh = int(swatch_.h);
      rp::renderStrokePreview(s.params, sw, sh, rgba);
      std::vector<unsigned char> bytes;
      packRGBA(rgba, bytes);
      swatchTex_.upload(k, sw, sh, bytes);
    }
    fillRect(swatch_, 0.09f, 0.09f, 0.10f, 0.9f);
    blitTex(swatchTex_, swatch_);
    strokeRect(swatch_, 0.0f, 0.0f, 0.0f, 0.7f);
  }

  static uint64_t swatchKey(const rp::BrushParams& p) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = mix(h, uint64_t(int(p.size * 16.0f)));
    h = mix(h, uint64_t(int(p.hardness * 1000.0f)));
    h = mix(h, uint64_t(int(p.opacity * 1000.0f)));
    h = mix(h, uint64_t(int(p.flow * 1000.0f)));
    h = mix(h, uint64_t(int(p.spacing * 1000.0f)));
    for (int i = 0; i < 3; ++i) h = mix(h, uint64_t(int(p.color[i] * 1000.0f)));
    h = mix(h, uint64_t(reinterpret_cast<uintptr_t>(p.tip)));
    // The swatch runs the real engine, so every dynamics setting changes it too.
    const rp::BrushDynamics& d = p.dyn;
    auto dyn = [&](const rp::DynControl& c) {
      h = mix(h, uint64_t(int(c.jitter * 1000.0f)));
      h = mix(h, uint64_t(int(c.minimum * 1000.0f)));
      h = mix(h, uint64_t(c.control) * 7919u + uint64_t(c.fadeSteps));
    };
    dyn(d.size); dyn(d.round); dyn(d.opacity); dyn(d.flow);
    h = mix(h, uint64_t(int(d.angle * 100.0f) & 0xffffff));
    h = mix(h, uint64_t(int(d.roundness * 1000.0f)));
    h = mix(h, uint64_t(int(d.angleJitter * 1000.0f)));
    h = mix(h, uint64_t(d.angleControl) * 104729u + uint64_t(d.angleFadeSteps));
    h = mix(h, uint64_t(int(d.scatter * 1000.0f)));
    h = mix(h, uint64_t(d.count) * 31u + uint64_t(int(d.countJitter * 1000.0f)));
    h = mix(h, uint64_t(d.scatterBoth ? 1 : 0) | (uint64_t(d.flipX ? 1 : 0) << 1) |
                   (uint64_t(d.flipY ? 1 : 0) << 2) | (uint64_t(d.flipXJitter ? 1 : 0) << 3) |
                   (uint64_t(d.flipYJitter ? 1 : 0) << 4));
    h = mix(h, uint64_t(d.seed));
    return h ? h : 1;
  }

  int vw_ = 0, vh_ = 0, page_ = 0, pages_ = 1;
  Rect bPaint_, bErase_, bClone_, bSmudge_, bPicker_, bLayers_, bUndo_, bRedo_;
  Rect bPrev_, bNext_;
  bool haveHover_ = false;
  int hoverMx_ = 0, hoverMy_ = 0;
  mutable float hoverX_ = 0.0f, hoverY_ = 0.0f;
  Rect swatch_, picker_, list_, grip_;
  int wantCols_ = kCols, wantRows_ = kRows;
  std::vector<Rect> rows_;
  std::vector<int> rowLayer_;
  int listTop_ = 0;
  std::vector<Rect> cells_;
  std::vector<int> cellBrush_;
  std::vector<Tex> cellTex_;
  Tex swatchTex_, cursorTex_;
  float cursorHW_ = 0.5f, cursorHH_ = 0.5f;
};

}  // namespace rpui
