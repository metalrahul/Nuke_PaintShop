#include "Descriptor.h"

#include <cstring>

namespace rp {
namespace {

constexpr int kMaxDepth = 24;
constexpr uint32_t kMaxItems = 1u << 20;  // a sane ceiling for list/field counts

struct Reader {
  const uint8_t* d;
  size_t n, p = 0;
  bool ok = true;

  bool has(size_t k) const { return ok && p + k <= n; }
  uint8_t u8() { if (!has(1)) { ok = false; return 0; } return d[p++]; }
  uint32_t u32() {
    if (!has(4)) { ok = false; return 0; }
    const uint32_t v = (uint32_t(d[p]) << 24) | (uint32_t(d[p + 1]) << 16) |
                       (uint32_t(d[p + 2]) << 8) | d[p + 3];
    p += 4;
    return v;
  }
  int32_t i32() { return int32_t(u32()); }
  double f64() {
    if (!has(8)) { ok = false; return 0.0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | d[p + i];
    p += 8;
    double out;
    std::memcpy(&out, &v, 8);
    return out;
  }
  std::string fixed(size_t k) {
    if (!has(k)) { ok = false; return std::string(); }
    std::string s(reinterpret_cast<const char*>(d + p), k);
    p += k;
    return s;
  }
  // A key: a length, then either that many bytes or, when the length is zero, a
  // four-character code.
  std::string key() {
    const uint32_t len = u32();
    return fixed(len == 0 ? 4 : (len > 1024 ? (ok = false, 0) : len));
  }
  // A UTF-16BE string with a character count. Only the Latin-1 range is kept, which
  // covers brush names in practice and keeps the rest of the code byte-oriented.
  std::string ucs2() {
    const uint32_t count = u32();
    if (count > kMaxItems) { ok = false; return std::string(); }
    std::string s;
    s.reserve(count);
    for (uint32_t i = 0; i < count && ok; ++i) {
      if (!has(2)) { ok = false; break; }
      const unsigned ch = (unsigned(d[p]) << 8) | d[p + 1];
      p += 2;
      if (ch == 0) continue;
      if (ch < 0x80) s += char(ch);
      else if (ch < 0x800) { s += char(0xC0 | (ch >> 6)); s += char(0x80 | (ch & 0x3F)); }
      else { s += char(0xE0 | (ch >> 12)); s += char(0x80 | ((ch >> 6) & 0x3F));
             s += char(0x80 | (ch & 0x3F)); }
    }
    return s;
  }
};

bool readValue(Reader& r, const std::string& type, DVal& out, int depth);

bool readDescriptor(Reader& r, DVal& out, int depth) {
  if (depth > kMaxDepth) return r.ok = false;
  out.type = DVal::kDesc;
  r.ucs2();   // the descriptor's own name, which nothing here needs
  r.key();    // its class id
  const uint32_t count = r.u32();
  if (!r.ok || count > kMaxItems) return r.ok = false;
  for (uint32_t i = 0; i < count; ++i) {
    const std::string k = r.key();
    const std::string t = r.fixed(4);
    if (!r.ok) return false;
    DVal v;
    if (!readValue(r, t, v, depth + 1)) return false;
    out.fields.emplace(k, std::move(v));
  }
  return r.ok;
}

// A reference is a list of typed items; only its length matters here, so each item is
// stepped over according to its own shape.
bool skipReference(Reader& r, int depth) {
  const uint32_t count = r.u32();
  if (!r.ok || count > kMaxItems) return r.ok = false;
  for (uint32_t i = 0; i < count && r.ok; ++i) {
    const std::string t = r.fixed(4);
    if (t == "name") { r.ucs2(); r.key(); r.ucs2(); }
    else if (t == "Clss") { r.ucs2(); r.key(); }
    else if (t == "Enmr") { r.ucs2(); r.key(); r.key(); r.key(); }
    else if (t == "Idnt" || t == "indx" || t == "rele") {
      if (t != "Idnt") { r.ucs2(); r.key(); }
      r.i32();
    } else if (t == "prop") { r.ucs2(); r.key(); r.key(); }
    else return r.ok = false;
    (void)depth;
  }
  return r.ok;
}

bool readValue(Reader& r, const std::string& type, DVal& out, int depth) {
  if (depth > kMaxDepth) return r.ok = false;
  if (type == "Objc" || type == "GlbO") return readDescriptor(r, out, depth);
  if (type == "VlLs") {
    out.type = DVal::kList;
    const uint32_t count = r.u32();
    if (!r.ok || count > kMaxItems) return r.ok = false;
    out.items.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const std::string t = r.fixed(4);
      if (!r.ok || !readValue(r, t, out.items[i], depth + 1)) return false;
    }
    return r.ok;
  }
  if (type == "doub") { out.type = DVal::kNum; out.num = r.f64(); return r.ok; }
  if (type == "UntF") {
    out.type = DVal::kNum;
    out.text = r.fixed(4);  // the unit: #Prc, #Pxl, #Ang, ...
    out.num = r.f64();
    return r.ok;
  }
  if (type == "TEXT") { out.type = DVal::kText; out.text = r.ucs2(); return r.ok; }
  if (type == "enum") {
    out.type = DVal::kEnum;
    out.text = r.key();      // the enum's type
    out.enumVal = r.key();   // and its value
    return r.ok;
  }
  if (type == "long") { out.type = DVal::kNum; out.num = r.i32(); return r.ok; }
  if (type == "comp") {
    out.type = DVal::kNum;
    const uint32_t hi = r.u32(), lo = r.u32();
    out.num = double((uint64_t(hi) << 32) | lo);
    return r.ok;
  }
  if (type == "bool") { out.type = DVal::kBool; out.b = r.u8() != 0; return r.ok; }
  if (type == "type" || type == "GlbC") { r.ucs2(); r.key(); out.type = DVal::kRaw; return r.ok; }
  if (type == "alis" || type == "tdta") {
    const uint32_t len = r.u32();
    if (!r.has(len)) return r.ok = false;
    r.p += len;
    out.type = DVal::kRaw;
    return r.ok;
  }
  if (type == "obj ") { out.type = DVal::kRaw; return skipReference(r, depth); }
  if (type == "ObAr" || type == "UnFl") {
    // Object/unit-float arrays: rare, and nothing here reads them. Unknown layout, so
    // stop rather than guess and walk off into the rest of the file.
    return r.ok = false;
  }
  return r.ok = false;  // an OSType this reader does not know
}

}  // namespace

bool parseDescriptor(const uint8_t* data, size_t size, size_t& pos, DVal& out) {
  if (!data || pos > size) return false;
  Reader r{data, size, pos};
  const bool ok = readDescriptor(r, out, 0);
  pos = r.p;
  return ok;
}

}  // namespace rp
