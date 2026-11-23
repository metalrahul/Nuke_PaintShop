// Tablet pressure for the OFX version, read straight from the operating system.
//
// Nuke knows the pen pressure (the NDK plugin gets it), but it does not pass it to OFX
// plugins: an OFX overlay always sees 1.0. So on Windows the plugin listens for itself,
// in two ways, and uses whichever one is producing pressure:
//
//   Windows Ink  - the pointer messages Windows sends for a pen (WM_POINTER*). Nuke's
//                  Qt uses these too. A message hook on the interface thread sees each
//                  one just before Qt does, so by the time Nuke calls the overlay with a
//                  pen move, the pressure of that same move has already been recorded.
//   Wintab       - the tablet vendors' own interface (Wacom and most others), for
//                  drivers set not to use Windows Ink. A system context is opened, so
//                  the pen keeps moving the cursor exactly as before.
//
// Both are read-only listeners. If neither is available, or a pen is not in use,
// pressure() reports nothing and the caller falls back to what the host sent.
// Elsewhere (Linux, macOS) this is a stub for now.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace pen {

struct Stats {
  long inkEvents = 0, wintabPackets = 0;
  double inkMin = 1e9, inkMax = -1e9, tabMin = 1e9, tabMax = -1e9;
  bool inkHooked = false, wintabOpen = false;
  const char* lastSource = "none";
};

#ifdef _WIN32

namespace detail {

using Clock = std::chrono::steady_clock;

inline std::atomic<double>& inkPressure() { static std::atomic<double> v{-1.0}; return v; }
inline std::atomic<int64_t>& inkTime() { static std::atomic<int64_t> v{0}; return v; }
inline Stats& stats() { static Stats s; return s; }
inline int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ---- Windows Ink

inline HHOOK& hook() { static HHOOK h = nullptr; return h; }
// Takes the hook out again when the plugin's DLL is unloaded: a hook left pointing into
// an unloaded DLL would crash the host on the next message.
struct Unhook {
  ~Unhook() { if (hook()) { UnhookWindowsHookEx(hook()); hook() = nullptr; } }
};
inline Unhook& unhookAtExit() { static Unhook u; return u; }

inline LRESULT CALLBACK onMessage(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && lp) {
    const MSG* m = reinterpret_cast<const MSG*>(lp);
    if (m->message == WM_POINTERUPDATE || m->message == WM_POINTERDOWN) {
      const UINT32 id = GET_POINTERID_WPARAM(m->wParam);
      POINTER_INPUT_TYPE type = PT_POINTER;
      if (GetPointerType(id, &type) && type == PT_PEN) {
        POINTER_PEN_INFO info;
        if (GetPointerPenInfo(id, &info) && (info.penMask & PEN_MASK_PRESSURE)) {
          const double p = double(info.pressure) / 1024.0;  // Windows reports 0..1024
          inkPressure().store(p);
          inkTime().store(nowMs());
          Stats& s = stats();
          ++s.inkEvents;
          if (p < s.inkMin) s.inkMin = p;
          if (p > s.inkMax) s.inkMax = p;
        }
      }
    }
  }
  return CallNextHookEx(hook(), code, wp, lp);
}

// ---- Wintab. The structures are those of the published Wintab specification,
// declared here so the build needs no SDK; the DLL is loaded only if it is installed.

typedef DWORD WTPKT;
typedef DWORD FIX32;
DECLARE_HANDLE(HCTX);
struct AXIS { LONG axMin; LONG axMax; UINT axUnits; FIX32 axResolution; };
struct LOGCONTEXTW {
  WCHAR lcName[40];
  UINT lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
  WTPKT lcPktData, lcPktMode, lcMoveMask;
  DWORD lcBtnDnMask, lcBtnUpMask;
  LONG lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
  LONG lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
  FIX32 lcSensX, lcSensY, lcSensZ;
  BOOL lcSysMode;
  int lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
  FIX32 lcSysSensX, lcSysSensY;
};
constexpr UINT WTI_DEFSYSCTX = 4, WTI_DEVICES = 100, DVC_NPRESSURE = 15;
constexpr WTPKT PK_X = 0x0080, PK_Y = 0x0100, PK_NORMAL_PRESSURE = 0x0400;
constexpr UINT CXO_MESSAGES = 0x0004;
// Fields in the order Wintab lays them out (by bit, lowest first), matching lcPktData.
struct Packet { LONG x; LONG y; UINT pressure; };

typedef UINT(WINAPI* WTInfoW_t)(UINT, UINT, LPVOID);
typedef HCTX(WINAPI* WTOpenW_t)(HWND, LOGCONTEXTW*, BOOL);
typedef int(WINAPI* WTPacketsGet_t)(HCTX, int, LPVOID);

struct Wintab {
  HMODULE dll = nullptr;
  HCTX ctx = nullptr;
  WTPacketsGet_t packetsGet = nullptr;
  double maxPressure = 1.0;
  double last = -1.0;
  int64_t lastTime = 0;  // when packets last arrived, to tell a pen from a mouse
  bool loaded = false;    // the DLL was looked for
  int64_t lastTry = -100000;
};
inline Wintab& wintab() { static Wintab w; return w; }

inline void openWintab() {
  Wintab& w = wintab();
  if (w.ctx) return;
  // Retried, at most once a second, until it opens: the context needs one of the
  // host's own windows, and there may not be an active one yet when the panel opens.
  const int64_t now = nowMs();
  if (now - w.lastTry < 1000) return;
  w.lastTry = now;
  if (!w.loaded) {
    w.loaded = true;
    w.dll = LoadLibraryW(L"Wintab32.dll");
  }
  if (!w.dll) return;
  auto info = reinterpret_cast<WTInfoW_t>(GetProcAddress(w.dll, "WTInfoW"));
  auto open = reinterpret_cast<WTOpenW_t>(GetProcAddress(w.dll, "WTOpenW"));
  w.packetsGet = reinterpret_cast<WTPacketsGet_t>(GetProcAddress(w.dll, "WTPacketsGet"));
  if (!info || !open || !w.packetsGet) return;
  LOGCONTEXTW lc{};
  if (!info(WTI_DEFSYSCTX, 0, &lc)) return;
  AXIS ax{};
  if (info(WTI_DEVICES, DVC_NPRESSURE, &ax) && ax.axMax > 0) w.maxPressure = double(ax.axMax);
  // The default *system* context: the pen still drives the cursor as before. Only the
  // pressure is asked for, and packets are queued for polling rather than sent as
  // window messages, so nothing reaches Nuke's windows that was not there before.
  lstrcpynW(lc.lcName, L"NPaintShop pressure", 40);
  lc.lcOptions &= ~CXO_MESSAGES;
  // Position is asked for as well as pressure, purely so that moving the pen produces
  // packets. With pressure alone a stroke at steady pressure produced none, the reading
  // went stale, and the stroke fell back to full pressure.
  lc.lcPktData = PK_X | PK_Y | PK_NORMAL_PRESSURE;
  lc.lcPktMode = 0;
  lc.lcMoveMask = PK_X | PK_Y | PK_NORMAL_PRESSURE;
  // Only the host's own window: the foreground window may belong to another program.
  HWND hwnd = GetActiveWindow();
  if (!hwnd) return;
  w.ctx = open(hwnd, &lc, TRUE);
  stats().wintabOpen = w.ctx != nullptr;
}

// The newest Wintab pressure, or -1. Drains the queue: only the latest sample matters.
inline double pollWintab() {
  Wintab& w = wintab();
  if (!w.ctx || !w.packetsGet) return -1.0;
  Packet buf[128];
  int n;
  while ((n = w.packetsGet(w.ctx, 128, buf)) > 0) {
    w.last = double(buf[n - 1].pressure) / w.maxPressure;
    w.lastTime = nowMs();
    Stats& s = stats();
    s.wintabPackets += n;
    if (w.last < s.tabMin) s.tabMin = w.last;
    if (w.last > s.tabMax) s.tabMax = w.last;
  }
  return w.last;
}

}  // namespace detail

// Call from the interface thread (an overlay action) before the first pressure() call.
inline void start() {
  if (!detail::hook()) {
    detail::hook() = SetWindowsHookExW(WH_GETMESSAGE, detail::onMessage, nullptr, GetCurrentThreadId());
    detail::stats().inkHooked = detail::hook() != nullptr;
    detail::unhookAtExit();
  }
  detail::openWintab();
}

// Which source to listen to: the "pen pressure" setting.
enum Mode { kAuto = 0, kInk = 1, kWintab = 2, kOff = 3 };

// The pen's current pressure in 0..1, or a negative number if no tablet source has
// reported anything recently (a mouse, or no driver).
//
// "Recently" matters. Wintab keeps its last reading forever, so without the time check
// a mouse stroke made after the pen was put down would paint at the pen's last pressure
// - usually 0, which paints nothing. A pen near the tablet sends packets constantly, so
// a quiet queue means the pointer is something else.
inline double pressure(int mode = kAuto) {
  if (mode == kOff) return -1.0;
  const double tab = detail::pollWintab();
  const int64_t now = detail::nowMs();
  // Windows Ink wins when it is live: it is the same pointer stream Qt is following.
  if (mode != kWintab && now - detail::inkTime().load() < 250) {
    detail::stats().lastSource = "Windows Ink";
    return detail::inkPressure().load();
  }
  if (mode != kInk && tab >= 0.0 && now - detail::wintab().lastTime < 250) {
    detail::stats().lastSource = "Wintab";
    return tab;
  }
  detail::stats().lastSource = "none";
  return -1.0;
}

inline const Stats& stats() { return detail::stats(); }

#else  // not Windows: nothing yet

enum Mode { kAuto = 0, kInk = 1, kWintab = 2, kOff = 3 };
inline void start() {}
inline double pressure(int = kAuto) { return -1.0; }
inline const Stats& stats() { static Stats s; return s; }

#endif

}  // namespace pen
