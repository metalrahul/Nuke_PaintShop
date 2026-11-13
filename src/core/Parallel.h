// Spreading independent pixel work across cores, for the few places where a brush is
// large enough that it matters. No Nuke dependency: this builds and is benchmarked
// standalone with the rest of src/core.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rp {

// Below this many pixels of independent work the threads cost more than they save.
// The figure is Photocraft's, and the reasoning carries over: a large soft tip is
// megapixels per dab, a small one is a few thousand.
constexpr size_t kDefaultParallelMinPixels = 256 * 256;

inline std::atomic<size_t>& parallelMinPixels() {
  static std::atomic<size_t> v{kDefaultParallelMinPixels};
  return v;
}

// Tests move the threshold: SIZE_MAX keeps everything serial, 0 forces threads on.
// Both paths must produce the same pixels, which is what makes that a fair test.
inline void setParallelMinPixels(size_t n) {
  parallelMinPixels().store(n, std::memory_order_relaxed);
}
inline bool worthThreads(size_t pixels) {
  return pixels >= parallelMinPixels().load(std::memory_order_relaxed);
}

// How many threads are worth putting on `n` work items.
inline int parallelWidth(int n) {
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 1;
  return std::min(std::min(n, int(hw)), 16);
}

// Workers that stay alive between dabs.
//
// They used to be spawned per call and joined at the end of it, on the reasoning that
// a dab big enough to qualify takes milliseconds so the spawning would be noise. That
// was measured on a two-core machine, where it spawns exactly one thread. On a sixteen
// core Windows box it spawns fifteen, twice per dab - once for the coverage rows and
// once for the tiles - and Windows thread creation is expensive enough that a large
// brush spent more time making threads than painting. Hence a pool.
//
// Created on first use, so a session that never paints a large brush never makes one.
class Pool {
 public:
  static Pool& get() {
    static Pool p;
    return p;
  }
  int width() const { return int(workers_.size()) + 1; }

  void run(int n, const std::function<void(int)>& fn) {
    // Handed out in contiguous blocks, not one item at a time. A block is a run of
    // neighbouring scanlines or tiles, which keeps each worker on memory it has just
    // touched and cuts the atomic traffic by the block size. Several blocks per worker
    // so a slow one cannot hold up the rest.
    {
      std::lock_guard<std::mutex> lk(m_);
      fn_ = &fn;
      n_ = n;
      chunk_ = std::max(1, n / (width() * 4));
      next_.store(0, std::memory_order_relaxed);
      remaining_ = int(workers_.size());
      ++gen_;
    }
    cv_.notify_all();
    drain(fn, n);  // the calling thread takes a share rather than waiting on the others
    std::unique_lock<std::mutex> lk(m_);
    doneCv_.wait(lk, [&] { return remaining_ == 0; });
    fn_ = nullptr;
  }

 private:
  Pool() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2) return;
    const int extra = std::min(int(hw), 16) - 1;
    workers_.reserve(size_t(extra));
    for (int i = 0; i < extra; ++i) workers_.emplace_back([this] { loop(); });
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_)
      if (t.joinable()) t.join();
  }
  void drain(const std::function<void(int)>& fn, int n) {
    const int c = chunk_;
    for (int b = next_.fetch_add(c); b < n; b = next_.fetch_add(c)) {
      const int end = std::min(b + c, n);
      for (int i = b; i < end; ++i) fn(i);
    }
  }
  void loop() {
    uint64_t seen = 0;
    for (;;) {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
      if (stop_) return;
      seen = gen_;
      const std::function<void(int)>* fn = fn_;
      const int n = n_;
      lk.unlock();
      if (fn) drain(*fn, n);
      lk.lock();
      if (--remaining_ == 0) doneCv_.notify_one();
    }
  }

  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_, doneCv_;
  const std::function<void(int)>* fn_ = nullptr;
  std::atomic<int> next_{0};
  int n_ = 0, remaining_ = 0, chunk_ = 1;
  uint64_t gen_ = 0;
  bool stop_ = false;
};

// True while this thread is already inside a parallel region, so nested calls stay
// serial instead of deadlocking on a pool that is busy with the outer one.
inline bool& inParallel() {
  static thread_local bool v = false;
  return v;
}

// Runs fn(0..n-1), spreading it across cores when `pixels` of work justify it.
//
// A template, not a std::function, so the serial path below inlines the body exactly
// as a hand-written loop would. Photocraft found that merely routing a small tip
// through a parallel helper cost it half its rasterization time, and the small-brush
// cases in the benchmark are there to catch that happening here.
template <class F>
void parallelFor(int n, size_t pixels, const F& fn) {
  if (!worthThreads(pixels) || n < 2 || inParallel() || Pool::get().width() < 2) {
    for (int i = 0; i < n; ++i) fn(i);
    return;
  }
  inParallel() = true;
  const std::function<void(int)> job(fn);
  Pool::get().run(n, job);
  inParallel() = false;
}

// As parallelFor, over scanlines [y0, y1). Rows of a dab are independent, and a row is
// enough work that handing them out one at a time keeps the cores evenly fed.
template <class F>
void parallelForRows(int y0, int y1, size_t pixels, const F& row) {
  const int n = y1 - y0;
  if (n <= 0) return;
  if (!worthThreads(pixels) || n < 2 || inParallel() || Pool::get().width() < 2) {
    for (int y = y0; y < y1; ++y) row(y);
    return;
  }
  parallelFor(n, pixels, [&](int i) { row(y0 + i); });
}

}  // namespace rp
