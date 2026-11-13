// NPaintShop - the installed brush library.
//
// Split out so it can be tested without Nuke: it is all filesystem work and no NDK.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace npaint {
namespace fs = std::filesystem;

// Brush sets installed alongside the plugin rather than pointed at wherever the user
// happened to leave the .abr.
//
// A script stores the set's *name*, so it keeps working when the original file is
// moved, renamed or simply not on the machine that opens it - which an absolute path
// in abr_file never survives. Two places are searched: the user's own library, which
// is the only one written to, and a NPaintShop/brushes folder under any entry on
// NUKE_PATH, so a facility can publish one set of brushes to everybody read-only.
struct BrushLibrary {
  static std::string userDir() {
    if (const char* over = std::getenv("NPAINTSHOP_BRUSHES"))
      if (*over) return over;
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    std::error_code ec;
    if (!home || !*home)
      return (fs::temp_directory_path(ec) / "NPaintShop" / "brushes").generic_string();
    return (fs::path(home) / ".nuke" / "NPaintShop" / "brushes").generic_string();
  }

  static std::vector<std::string> dirs() {
    std::vector<std::string> out{userDir()};
    const char* np = std::getenv("NUKE_PATH");
    if (!np || !*np) return out;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string cur;
    for (const char* p = np;; ++p) {
      if (*p && *p != sep) { cur += *p; continue; }
      if (!cur.empty()) {
        const std::string d = (fs::path(cur) / "NPaintShop" / "brushes").generic_string();
        if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
        cur.clear();
      }
      if (!*p) break;
    }
    return out;
  }

  static bool isAbr(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e == ".abr";
  }

  // Installed set names, without extension, in a stable order. The user's own library
  // wins where a name appears in both, so a local copy can override a published one.
  static std::vector<std::string> list() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const std::string& d : dirs()) {
      if (!fs::is_directory(d, ec)) continue;
      for (const fs::directory_entry& e : fs::directory_iterator(d, ec)) {
        if (!e.is_regular_file(ec) || !isAbr(e.path())) continue;
        std::string n = e.path().stem().string();
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(std::move(n));
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  static std::string pathFor(const std::string& name) {
    if (name.empty()) return "";
    std::error_code ec;
    for (const std::string& d : dirs()) {
      if (!fs::is_directory(d, ec)) continue;
      for (const fs::directory_entry& e : fs::directory_iterator(d, ec)) {
        if (e.is_regular_file(ec) && isAbr(e.path()) && e.path().stem().string() == name)
          return e.path().generic_string();
      }
    }
    return "";
  }

  // Copies an .abr into the user's library. Returns the installed set's name.
  static std::string install(const std::string& src, std::string* err) {
    std::error_code ec;
    if (src.empty() || !fs::is_regular_file(src, ec)) {
      if (err) *err = "no brush file to install - set 'brush file' first";
      return "";
    }
    const fs::path from(src);
    if (!isAbr(from)) {
      if (err) *err = "that is not a .abr file";
      return "";
    }
    const fs::path dir(userDir());
    fs::create_directories(dir, ec);
    const fs::path to = dir / (from.stem().string() + ".abr");
    if (fs::exists(to, ec) && fs::equivalent(from, to, ec)) return from.stem().string();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      if (err) *err = "could not copy into " + dir.generic_string() + ": " + ec.message();
      return "";
    }
    return from.stem().string();
  }

  // Only ever removes from the user's own library; a published set is left alone.
  static bool remove(const std::string& name, std::string* err) {
    std::error_code ec;
    const fs::path p = fs::path(userDir()) / (name + ".abr");
    if (!fs::is_regular_file(p, ec)) {
      if (err)
        *err = "'" + name + "' is not in your own brush library, so it cannot be removed here";
      return false;
    }
    if (!fs::remove(p, ec) || ec) {
      if (err) *err = "could not delete " + p.generic_string();
      return false;
    }
    return true;
  }
};

}  // namespace npaint
