// NPaintShop core - Photoshop descriptor reader.
//
// Versions 6 and up of .abr keep the real brush names and every setting in a 'desc'
// block holding one Photoshop descriptor: a recursive key/value tree. This reads the
// whole tree into a small variant, because to find the keys that matter it has to be
// able to step over the ones that do not, and skipping a value correctly means knowing
// how to parse it anyway.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rp {

struct DVal {
  enum Type { kNone, kNum, kBool, kText, kDesc, kList, kEnum, kRaw };
  Type type = kNone;
  double num = 0.0;
  bool b = false;
  std::string text;      // kText, and the unit for kNum ("#Prc", "#Pxl", "#Ang")
  std::string enumVal;   // kEnum
  std::map<std::string, DVal> fields;  // kDesc
  std::vector<DVal> items;             // kList

  const DVal* find(const std::string& key) const {
    auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
  }
  // Convenience readers that fall back when a key is missing or the wrong type.
  double numOr(const std::string& key, double fallback) const {
    const DVal* v = find(key);
    return (v && v->type == kNum) ? v->num : fallback;
  }
  bool boolOr(const std::string& key, bool fallback) const {
    const DVal* v = find(key);
    return (v && v->type == kBool) ? v->b : fallback;
  }
  std::string textOr(const std::string& key, const std::string& fallback) const {
    const DVal* v = find(key);
    return (v && v->type == kText) ? v->text : fallback;
  }
};

// Parses one descriptor from `data` at `pos`, advancing it. Returns false on malformed
// input, having consumed an unspecified amount. Nesting is capped to stop a corrupt
// file recursing without end.
bool parseDescriptor(const uint8_t* data, size_t size, size_t& pos, DVal& out);

}  // namespace rp
