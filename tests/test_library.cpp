// NPaintShop - tests for the installed brush library (no Nuke needed).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
namespace fs = std::filesystem;
#include "BrushLibrary.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
  const fs::path root = fs::temp_directory_path() / "npaintshop_libtest";
  fs::remove_all(root);
  fs::create_directories(root / "src");
  fs::create_directories(root / "pub" / "NPaintShop" / "brushes");
#ifdef _WIN32
  _putenv_s("NPAINTSHOP_BRUSHES", (root / "user").string().c_str());
  _putenv_s("NUKE_PATH", (root / "pub").string().c_str());
#else
  setenv("NPAINTSHOP_BRUSHES", (root / "user").string().c_str(), 1);
  setenv("NUKE_PATH", (root / "pub").string().c_str(), 1);
#endif
  auto write = [](const fs::path& p, const char* t) { std::ofstream f(p); f << t; };
  write(root / "src" / "Reid_Southen.abr", "x");
  write(root / "src" / "notes.txt", "x");
  write(root / "pub" / "NPaintShop" / "brushes" / "Studio.abr", "x");

  CHECK(npaint::BrushLibrary::list() == std::vector<std::string>{"Studio"});  // published set visible

  std::string err;
  CHECK(npaint::BrushLibrary::install((root / "src" / "notes.txt").string(), &err).empty());
  CHECK(!err.empty());

  const std::string n = npaint::BrushLibrary::install((root / "src" / "Reid_Southen.abr").string(), &err);
  CHECK(n == "Reid_Southen");
  auto l = npaint::BrushLibrary::list();
  CHECK(l.size() == 2 && l[0] == "Reid_Southen" && l[1] == "Studio");
  CHECK(!npaint::BrushLibrary::pathFor("Reid_Southen").empty());
  CHECK(!npaint::BrushLibrary::pathFor("Studio").empty());
  CHECK(npaint::BrushLibrary::pathFor("nope").empty());

  // Installing twice is fine and does not duplicate.
  CHECK(npaint::BrushLibrary::install((root / "src" / "Reid_Southen.abr").string(), &err) == "Reid_Southen");
  CHECK(npaint::BrushLibrary::list().size() == 2);

  // A published set cannot be removed through the plugin.
  CHECK(!npaint::BrushLibrary::remove("Studio", &err));
  CHECK(npaint::BrushLibrary::pathFor("Studio") != "");

  CHECK(npaint::BrushLibrary::remove("Reid_Southen", &err));
  CHECK(npaint::BrushLibrary::list() == std::vector<std::string>{"Studio"});

  fs::remove_all(root);
  std::printf(fails ? "%d FAILED\n" : "brush library ok\n", fails);
  return fails ? 1 : 0;
}
