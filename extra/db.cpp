#include "db.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <sys/resource.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace mini {
namespace {

constexpr size_t missing = std::numeric_limits<size_t>::max();
constexpr size_t ix_allowance = 4 * 1024 * 1024;
constexpr size_t bound_allowance = 2 * 1024 * 1024;
constexpr size_t code_chunk = 8;
constexpr size_t code_word = 4;
constexpr size_t code_alignment = code_chunk * code_word;
constexpr size_t geometric_stride = 128;
constexpr size_t geometric_buckets = 32;
constexpr int32_t outlier_norm = std::numeric_limits<int32_t>::min();
constexpr uint32_t wide_rec = 8 * 1024;

struct RowRef {
  uint64_t Offs;
  uint32_t length;
  uint16_t dense_octets = 0;
  uint8_t preamble_octets = Frame::preamble_extent;

  uint64_t payload_offs() const { return Offs + preamble_octets; }
};
// Each tree Vertex summarizes one sorted Pg of Rec locations. Removing the first
// Ky leaves page boundaries intact, so the tree changes only on splits and merges.
class Rows {
 public:
  using value_type = std::pair<int64_t, RowRef>;

 private:
  static constexpr size_t pg_extent = 128;
  using Page = std::vector<value_type>;
  using Pages = std::map<int64_t, Page>;

 public:
  class const_iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = Rows::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    const_iterator() = default;
    reference operator*() const { return pg->second[posn]; }
    pointer operator->() const { return &**this; }

    const_iterator& operator++() {
      if (++posn == pg->second.size()) {
        ++pg;
        posn = 0;
      }
      return *this;
    }
    const_iterator operator++(int) {
      auto copy = *this;
      ++*this;
      return copy;
    }
    const_iterator& operator--() {
      if (posn == 0) posn = (--pg)->second.size();
      --posn;
      return *this;
    }
    const_iterator operator--(int) {
      auto copy = *this;
      --*this;
      return copy;
    }
    bool operator==(const const_iterator&) const = default;

   private:
    friend class Rows;
    const_iterator(Pages::const_iterator Pg, size_t Posn)
        : pg(Pg), posn(Posn) {}
    Pages::const_iterator pg;
    size_t posn = 0;
  };

  size_t size() const { return size_; }
  const_iterator begin() const { return size_ == 0 ? end() : const_iterator(pages_.begin(), 0); }
  const_iterator end() const { return {pages_.end(), 0}; }
  const_iterator lower_bound(int64_t Ky) const {
    if (size_ == 0) return end();
    auto Pg = std::prev(pages_.upper_bound(Ky));
    const size_t at = Posn(Pg->second, Ky);
    if (at == Pg->second.size()) return {std::next(Pg), 0};
    return {Pg, at};
  }
  const_iterator upper_bound(int64_t Ky) const {
    auto Item = lower_bound(Ky);
    if (Item != end() && Item->first == Ky) ++Item;
    return Item;
  }
  bool range_at_least(const_iterator first, const_iterator Final, size_t count) const {
    while (first.pg != Final.pg) {
      const size_t available = first.pg->second.size() - first.posn;
      if (available >= count) return true;
      count -= available;
      ++first.pg;
      first.posn = 0;
    }
    return Final.posn - first.posn >= count;
  }
  bool contains(int64_t Ky) const {
    const auto Item = lower_bound(Ky);
    return Item != end() && Item->first == Ky;
  }

  void inject_or_assign(int64_t Ky, RowRef Rec) {
    if (pages_.empty()) add_pg(std::numeric_limits<int64_t>::min());
    auto Pg = std::prev(pages_.upper_bound(Ky));
    size_t at = Posn(Pg->second, Ky);
    if (at < Pg->second.size() && Pg->second[at].first == Ky) {
      Pg->second[at].second = Rec;
      return;
    }
    if (Pg->second.size() == pg_extent) {
      if (at == pg_extent && std::next(Pg) == pages_.end()) {
        Pg = add_pg(Ky);
        at = 0;
      } else {
        const size_t middle = pg_extent / 2;
        auto& entries = Pg->second;
        auto Rhs = add_pg(entries[middle].first);
        Rhs->second.insert(Rhs->second.end(), entries.begin() + middle, entries.end());
        entries.resize(middle);
        if (at > middle) {
          Pg = Rhs;
          at -= middle;
        }
      }
    }
    Pg->second.insert(Pg->second.begin() + at, {Ky, Rec});
    ++size_;
  }

  void erase(int64_t Ky) {
    if (size_ == 0) return;
    auto Pg = std::prev(pages_.upper_bound(Ky));
    auto& entries = Pg->second;
    const size_t at = Posn(entries, Ky);
    if (at == entries.size() || entries[at].first != Ky) return;
    entries.erase(entries.begin() + at);
    --size_;
    if (entries.size() >= pg_extent / 4) return;
    const auto Rhs = std::next(Pg);
    if (Rhs != pages_.end() && entries.size() + Rhs->second.size() <= pg_extent) {
      Unify(Pg, Rhs);
    } else if (Pg != pages_.begin()) {
      const auto Lhs = std::prev(Pg);
      if (Lhs->second.size() + entries.size() <= pg_extent) Unify(Lhs, Pg);
    }
  }

 private:
  static size_t Posn(const Page& Pg, int64_t Ky) {
    return std::ranges::lower_bound(Pg, Ky, {}, &value_type::first) - Pg.begin();
  }
  Pages::iterator add_pg(int64_t boundary) {
    Page entries;
    entries.reserve(pg_extent);
    return pages_.emplace(boundary, std::move(entries)).first;
  }
  void Unify(Pages::iterator Lhs, Pages::iterator Rhs) {
    Lhs->second.insert(Lhs->second.end(), Rhs->second.begin(), Rhs->second.end());
    pages_.erase(Rhs);
  }

  Pages pages_;
  size_t size_ = 0;
};

struct ProjectionEntry {
  float pivot;
  const Rows::value_type* Item;
};

struct BoundRow {
  // Before a Relation's Rec directory changes, its Stash is dropped first.
  const Rows::value_type* Item;

  int64_t Ky() const { return Item->first; }
  RowRef location() const { return Item->second; }
};

struct BoundGroups {
  struct Bucket { uint64_t tag; uint16_t Offs; uint16_t count; int32_t radius = 0; };
  size_t Col;
  bool compact_geometry = false;
  std::vector<Bucket> buckets;

  size_t Octets() const {
    return sizeof(*this) + buckets.capacity() * sizeof(Bucket);
  }
  const Bucket* lookup(uint64_t tag) const {
    const auto bucket = std::ranges::lower_bound(buckets, tag, {}, &Bucket::tag);
    return bucket != buckets.end() && bucket->tag == tag ? &*bucket : nullptr;
  }
};

struct BoundCache {
  size_t stride = 0;
  double inverse_stride = 0;
  double max_fault = 0;
  std::vector<BoundRow> Recs;
  std::vector<int32_t> norms;
  std::vector<int8_t> Tags;
  size_t code_offs = 0;
  std::vector<double> origin;
  bool has_outliers = false;
  bool clusters_attempted = false;
  std::unique_ptr<BoundGroups> Clusters;

  void allocate_tags(size_t size) {
    Tags.resize(size + code_alignment - 1);
    const auto address = reinterpret_cast<uintptr_t>(Tags.data());
    code_offs = (code_alignment - address % code_alignment) % code_alignment;
  }

  const int8_t* code_octets() const { return Tags.data() + code_offs; }
  int8_t* code_octets() { return Tags.data() + code_offs; }

  size_t Octets() const {
    return Recs.capacity() * sizeof(BoundRow) + norms.capacity() * sizeof(int32_t) +
           Tags.capacity() + origin.capacity() * sizeof(double) + (Clusters ? Clusters->Octets() : 0);
  }
};

}  // namespace

struct Relation {
  uint32_t id;
  std::string Label;
  std::vector<Column> columns;
  std::map<std::string, size_t, std::less<>> ordinals;
  std::vector<size_t> layout;
  size_t primary = missing;
  size_t vector = missing;
  uint32_t vector_octets = 0;
  Rows Recs;
  std::string ix_label;
  bool ix_active = false;
  std::vector<ProjectionEntry> Ix;
  bool bounds_attempted = false;
  std::unique_ptr<BoundCache> bounds;

  Relation(uint32_t id, std::string Label, std::vector<Column> columns)
      : id(id), Label(std::move(Label)), columns(std::move(columns)) {
    for (size_t i = 0; i < this->columns.size(); ++i) {
      const auto& Col = this->columns[i];
      if (!ordinals.emplace(Col.Label, i).second) {
        throw SqlError("SCHEMA", "duplicate column name");
      }
      if (Col.primary) {
        if (primary != missing || Col.Kind != Type::bigint) {
          throw SqlError("SCHEMA", "primary key must be a single BIGINT column");
        }
        primary = i;
      }
      if (Col.Kind == Type::vector) {
        if (vector != missing || Col.length == 0 ||
            Col.length > (std::numeric_limits<uint32_t>::max() - 32) / sizeof(float)) {
          throw SqlError("SCHEMA", "invalid vector column");
        }
        vector = i;
        vector_octets = Col.length * sizeof(float);
      }
    }
    if (primary == missing) throw SqlError("SCHEMA", "missing primary key");
    if (vector != missing) layout.push_back(vector);
    for (size_t i = 0; i < this->columns.size(); ++i) {
      if (i != primary && i != vector) layout.push_back(i);
    }
  }

  size_t Col(std::string_view Label) const {
    const auto it = ordinals.find(Label);
    if (it == ordinals.end()) throw SqlError("UNKNOWN_COLUMN", "column does not exist");
    return it->second;
  }
};

namespace {

std::string_view payload(Journal& Journal, RowRef Rec) {
  // Because RowRef stores the Frag Offs, it doubles as an undo pointer.
  return Journal.read(Rec.payload_offs(), Rec.length);
}

constexpr uint32_t max_dense_dimensions = 4096;
constexpr uint16_t short_vector = 0x8000;
constexpr uint16_t decimal_vector = 0x4000;
constexpr uint16_t dyadic_vector = short_vector | decimal_vector;
constexpr size_t dense_chunk = 8;
constexpr size_t dense_chunk_octets = 28;
constexpr std::array<double, 3> decimal_scales{100.0, 10000.0, 1000000.0};
constexpr size_t decimal_preamble = sizeof(uint16_t) + sizeof(uint8_t);
constexpr size_t dyadic_preamble = sizeof(uint16_t) + 2 * sizeof(uint8_t);

struct DyadicGrid {
  unsigned Span;
  float Stride;
};

float power_of_two(int Expnt) {
  return std::bit_cast<float>(static_cast<uint32_t>(Expnt + 127) << 23);
}

DyadicGrid pull_dyadic_grid(Reader& Fetcher) {
  const unsigned Span = Fetcher.read<uint8_t>();
  const int Expnt = Fetcher.read<int8_t>();
  if ((Span != 8 && Span != 10 && Span != 12 && Span != 16) ||
      Expnt < -100 || Expnt > 100) throw std::runtime_error("invalid dyadic vector encoding");
  return {Span, power_of_two(Expnt)};
}

int32_t dyadic_code(std::string_view Octets, unsigned Span, size_t Track) {
  uint64_t upper = 0;
  std::memcpy(&upper, Octets.data() + dense_chunk, Span - dense_chunk);
  const unsigned high_bitw = Span - 8;
  const auto high = static_cast<uint32_t>(upper >> (high_bitw * Track)) & ((1u << high_bitw) - 1);
  const auto stored = static_cast<uint8_t>(Octets[Track]) | (high << 8);
  const uint32_t sign = 1u << (Span - 1);
  return static_cast<int32_t>(stored ^ sign) - static_cast<int32_t>(sign);
}

struct DecimalGrid {
  unsigned Span;
  double Stride;
};

DecimalGrid pull_decimal_grid(Reader& Fetcher) {
  const unsigned descriptor = Fetcher.read<uint8_t>();
  const unsigned scale = descriptor >> 2;
  const unsigned Span = descriptor & 3u;
  if (!Span || scale >= decimal_scales.size()) throw std::runtime_error("invalid decimal vector encoding");
  return {Span, 1.0 / decimal_scales[scale]};
}

int32_t signed_decimal_code(std::string_view Octets) {
  uint32_t stored = 0;
  std::memcpy(&stored, Octets.data(), Octets.size());
  const uint32_t sign = 1u << (8 * Octets.size() - 1);
  return static_cast<int32_t>(stored ^ sign) - static_cast<int32_t>(sign);
}

constexpr double decimal_cap = 8388607.5;

bool decimal_code(float input, double scale, int32_t& code) {
  const double scaled = input * scale;
  if (!(std::abs(scaled) < decimal_cap)) return false;
  code = static_cast<int32_t>(scaled + std::copysign(0.5, scaled));
  return std::bit_cast<uint32_t>(static_cast<float>(code * (1.0 / scale))) ==
         std::bit_cast<uint32_t>(input);
}

#if defined(__x86_64__)
template <unsigned Width>
__attribute__((target("avx2")))
bool pack_dyadic_blocks(std::string_view vector, float scale, float Stride, char* Produced, size_t& i) {
  const auto factor = _mm256_set1_ps(scale);
  const auto unit = _mm256_set1_ps(Stride);
  const auto overflow = _mm256_set1_epi32(1 << (Width - 1));
  const auto lo_mask = _mm256_set1_epi32(255);
  constexpr unsigned Bitw = Width - 8;
  const auto shifts = _mm256_setr_epi32(0,Bitw,2*Bitw,3*Bitw,4*Bitw,5*Bitw,6*Bitw,7*Bitw);
  const auto high_mask = _mm256_set1_epi32((1 << Bitw) - 1);
  for (; i + dense_chunk * sizeof(float) <= vector.size(); i += dense_chunk * sizeof(float)) {
    __m256 original;
    std::memcpy(&original, vector.data()+i, sizeof(original));
    const auto code = _mm256_cvttps_epi32(_mm256_mul_ps(original, factor));
    const auto magnitude = _mm256_abs_epi32(code);
    const auto inside = _mm256_cmpgt_epi32(overflow, magnitude);
    const auto restored = _mm256_mul_ps(_mm256_cvtepi32_ps(code), unit);
    const auto same = _mm256_cmpeq_epi32(_mm256_castps_si256(original), _mm256_castps_si256(restored));
    if (static_cast<unsigned>(_mm256_movemask_epi8(_mm256_and_si256(inside,same))) != UINT32_MAX) return false;
    const auto Lo = _mm256_and_si256(code, lo_mask);
    const auto halves = _mm_packus_epi32(_mm256_castsi256_si128(Lo), _mm256_extracti128_si256(Lo, 1));
    const uint64_t Octets = static_cast<uint64_t>(_mm_cvtsi128_si64(_mm_packus_epi16(halves, halves)));
    auto* Chunk = Produced + i / (dense_chunk * sizeof(float)) * Width;
    std::memcpy(Chunk, &Octets, dense_chunk);
    if constexpr (Width == 16) {
      const auto high = _mm256_and_si256(_mm256_srli_epi32(code, 8), high_mask);
      const auto words = _mm_packus_epi32(_mm256_castsi256_si128(high), _mm256_extracti128_si256(high, 1));
      const uint64_t upper = static_cast<uint64_t>(_mm_cvtsi128_si64(_mm_packus_epi16(words,words)));
      std::memcpy(Chunk+dense_chunk, &upper, dense_chunk);
    } else if constexpr (Width != 8) {
      const auto fields = _mm256_sllv_epi32(_mm256_and_si256(_mm256_srli_epi32(code, 8), high_mask), shifts);
      auto all = _mm_or_si128(_mm256_castsi256_si128(fields), _mm256_extracti128_si256(fields, 1));
      all = _mm_or_si128(all, _mm_srli_si128(all, 8));
      all = _mm_or_si128(all, _mm_srli_si128(all, 4));
      const uint32_t upper = static_cast<uint32_t>(_mm_cvtsi128_si32(all));
      std::memcpy(Chunk+dense_chunk, &upper, Width-dense_chunk);
    }
  }
  return true;
}

__attribute__((target("avx2")))
bool decimal_blocks(std::string_view vector, std::span<int32_t> Tags, double scale,
                    int32_t& largest, size_t& i) {
  const auto factor = _mm256_set1_pd(scale);
  const auto Stride = _mm256_set1_pd(1.0 / scale);
  const auto max = _mm256_set1_pd(decimal_cap);
  const auto sign = _mm256_set1_pd(-0.0);
  const auto half = _mm256_set1_pd(0.5);
  auto magnitude = _mm_setzero_si128();
  for (; i + 4 <= Tags.size(); i += 4) {
    __m128 original;
    std::memcpy(&original, vector.data() + i * sizeof(float), sizeof(original));
    const auto scaled = _mm256_mul_pd(_mm256_cvtps_pd(original), factor);
    if (_mm256_movemask_pd(_mm256_cmp_pd(_mm256_andnot_pd(sign, scaled), max, _CMP_LT_OQ)) != 15) return false;
    const auto adjust = _mm256_or_pd(half, _mm256_and_pd(sign, scaled));
    const auto code = _mm256_cvttpd_epi32(_mm256_add_pd(scaled, adjust));
    const auto decoded = _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtepi32_pd(code), Stride));
    const auto same_bitw = _mm_cmpeq_epi32(_mm_castps_si128(original), _mm_castps_si128(decoded));
    if (_mm_movemask_ps(_mm_castsi128_ps(same_bitw)) != 15) return false;
    std::memcpy(Tags.data() + i, &code, sizeof(code));
    magnitude = _mm_max_epi32(magnitude, _mm_abs_epi32(code));
  }
  std::array<int32_t, 4> lanes;
  std::memcpy(lanes.data(), &magnitude, sizeof(magnitude));
  largest = std::ranges::max(lanes);
  return true;
}

__attribute__((target("avx2")))
size_t pack_upper_words(std::string_view vector, char* Produced) {
  size_t Offs = 0;
  for (; Offs + sizeof(__m256i) <= vector.size(); Offs += sizeof(__m256i)) {
    __m256i Vals;
    std::memcpy(&Vals, vector.data() + Offs, sizeof(Vals));
    const auto halves = _mm256_srli_epi32(Vals, 16);
    const auto upper = _mm_packus_epi32(_mm256_castsi256_si128(halves), _mm256_extracti128_si256(halves, 1));
    std::memcpy(Produced + Offs / 2, &upper, sizeof(upper));
  }
  return Offs;
}

__attribute__((target("avx2")))
size_t pack_blocks(std::string_view vector, unsigned highest, std::string& packed) {
  // For the first three byte planes, collect the eight lowest Octets of each.
  // The fourth plane is captured by its sign together with an Expnt delta.
  const auto columns = _mm256_setr_epi8(0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15,
                                        0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15);
  const auto pair_weights = _mm_setr_epi16(1,16,1,16,1,16,1,16);
  const auto pair_octets = _mm_setr_epi8(0,4,8,12,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1);
  const auto expnt_mask = _mm256_set1_epi32(127);
  const auto rec_expnt = _mm256_set1_epi32(highest);
  const auto sign_mask = _mm256_set1_epi32(8);
  const auto escape = _mm256_set1_epi32(7);
  size_t at = 0;
  for (; at + 32 <= vector.size(); at += 32) {
    __m256i Bitw;
    std::memcpy(&Bitw, vector.data() + at, sizeof(Bitw));
    const auto exponents = _mm256_and_si256(_mm256_srli_epi32(Bitw, 24), expnt_mask);
    const auto deltas = _mm256_min_epi32(_mm256_sub_epi32(rec_expnt, exponents), escape);
    const auto signs = _mm256_and_si256(_mm256_srli_epi32(Bitw, 28), sign_mask);
    const auto Tags = _mm256_or_si256(deltas, signs);
    const auto nibbles = _mm_packus_epi32(_mm256_castsi256_si128(Tags), _mm256_extracti128_si256(Tags, 1));
    const uint32_t Preamble = static_cast<uint32_t>(_mm_cvtsi128_si32(
        _mm_shuffle_epi8(_mm_madd_epi16(nibbles, pair_weights), pair_octets)));

    const auto planes = _mm256_shuffle_epi8(Bitw, columns);
    const auto Lhs = _mm256_castsi256_si128(planes);
    const auto Rhs = _mm256_extracti128_si256(planes, 1);
    const auto Lo = _mm_unpacklo_epi32(Lhs, Rhs);
    const auto mid = _mm_unpackhi_epi32(Lhs, Rhs);
    auto* to = packed.data() + 3 + (at / 32) * dense_chunk_octets;
    std::memcpy(to, &Preamble, sizeof(Preamble));
    std::memcpy(to + sizeof(Preamble), &Lo, sizeof(Lo));
    std::memcpy(to + sizeof(Preamble) + sizeof(Lo), &mid, sizeof(uint64_t));

    unsigned special = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(deltas, escape))));
    if (special) {
      std::array<uint32_t, dense_chunk> Vals;
      std::memcpy(Vals.data(), &exponents, sizeof(exponents));
      do {
        const auto Track = std::countr_zero(special);
        packed.push_back(static_cast<char>(Vals[Track]));
        special &= special - 1;
      } while (special);
    }
  }
  return at / sizeof(float);
}
#endif

template <unsigned Width>
bool pack_dyadic(std::string_view vector, int highest, size_t previous_extent, std::string& packed) {
  const size_t blocks = (vector.size() / sizeof(float) + dense_chunk - 1) / dense_chunk;
  const size_t size = dyadic_preamble + blocks * Width;
  const int Expnt = highest - Width + 2;
  if (size >= previous_extent || Expnt < -100 || Expnt > 100) return false;
  const float Stride = power_of_two(Expnt);
  const float scale = power_of_two(-Expnt);
  packed.resize(size);
  char* Produced = packed.data()+dyadic_preamble;
  size_t i = 0;
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2") && !pack_dyadic_blocks<Width>(vector, scale, Stride, Produced, i)) return false;
#endif
  constexpr auto Cap = 1 << (Width-1);
  constexpr auto high_mask = (1u << (Width-8))-1;
  for (; i < vector.size();) {
    auto* Chunk = Produced + i / (dense_chunk * sizeof(float)) * Width;
    std::fill_n(Chunk, Width, '\0');
    uint64_t high = 0;
    for (size_t Track = 0; Track < dense_chunk && i < vector.size(); ++Track, i+=sizeof(float)) {
      const float original = load<float>(vector.substr(i));
      const float scaled = original * scale;
      if (!(std::abs(scaled) < Cap)) return false;
      const int32_t code = static_cast<int32_t>(scaled);
      if (std::bit_cast<uint32_t>(code*Stride) != std::bit_cast<uint32_t>(original)) return false;
      Chunk[Track] = static_cast<char>(code);
      high |= uint64_t((static_cast<uint32_t>(code) >> 8) & high_mask) << ((Width-8)*Track);
    }
    std::memcpy(Chunk + dense_chunk, &high, Width-dense_chunk);
  }
  const auto Preamble = static_cast<uint16_t>(size | dyadic_vector);
  std::memcpy(packed.data(), &Preamble, sizeof(Preamble));
  packed[2] = static_cast<char>(Width);
  packed[3] = static_cast<char>(Expnt);
  return true;
}

bool on_grid(uint32_t Bitw, int Expnt) {
  const uint32_t magnitude = Bitw & 0x7fffffffu;
  if (magnitude == 0) return Bitw == 0;
  const int binary_expnt = static_cast<int>(magnitude >> 23);
  const uint32_t digits = (magnitude & 0x7fffffu) | (binary_expnt ? 0x800000u : 0u);
  return std::max(1, binary_expnt) - 150 + std::countr_zero(digits) >= Expnt;
}

// A leading word holds the prefix size and the encoding choice. Scalar columns
// keep their original byte layout, and raw float32 storage is always available.
bool pack_decimal(std::string_view vector, size_t previous_extent, std::string& packed) {
  std::array<int32_t, max_dense_dimensions> scratch;
  const auto Tags = std::span(scratch).first(vector.size() / sizeof(float));
  for (unsigned scale = 0; scale < decimal_scales.size(); ++scale) {
    size_t i = 0;
    int32_t largest = 0;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2") && !decimal_blocks(vector, Tags, decimal_scales[scale], largest, i)) continue;
#endif
    for (; i < Tags.size(); ++i) {
      if (!decimal_code(load<float>(vector.substr(i * sizeof(float))), decimal_scales[scale], Tags[i])) break;
      largest = std::max(largest, std::abs(Tags[i]));
    }
    if (i != Tags.size()) continue;
    const unsigned Span = largest <= 127 ? 1 : largest <= 32767 ? 2 : 3;
    const size_t size = decimal_preamble + Span * Tags.size();
    if (size >= previous_extent) return false;
    packed.clear();
    store(packed, static_cast<uint16_t>(size | decimal_vector));
    store(packed, static_cast<uint8_t>((scale << 2) | Span));
    packed.resize(size);
    const auto push2_tags = [&]<size_t Width>() {
      auto* Produced = packed.data() + decimal_preamble;
      for (int32_t code : Tags) {
        std::memcpy(Produced, &code, Width);
        Produced += Width;
      }
    };
    if (Span == 1) push2_tags.template operator()<1>();
    else if (Span == 2) push2_tags.template operator()<2>();
    else push2_tags.template operator()<3>();
    return true;
  }
  return false;
}

std::string_view pack_rec(std::string_view Rec, uint32_t vector_octets, std::string& packed,
                          bool decimal, bool dyadic) {
  const size_t Dim = vector_octets / sizeof(float);
  if (Dim < 16 || Dim > max_dense_dimensions) return {};
  uint32_t lo_bitw = 0;
  uint32_t maximum = 0;
  for (size_t i = 0; i < Dim; ++i) {
    uint32_t Bitw;
    std::memcpy(&Bitw, Rec.data() + i * sizeof(Bitw), sizeof(Bitw));
    lo_bitw |= Bitw;
    maximum = std::max(maximum, Bitw & 0x7fffffffu);
  }

  const bool short_form = (lo_bitw & 0xffffu) == 0;
  const size_t blocks = (Dim + dense_chunk - 1) / dense_chunk;
  const size_t previous_extent = 3 + blocks * dense_chunk_octets;
  if (dyadic) {
    const auto vector = Rec.substr(0, vector_octets);
    const int Expnt = static_cast<int>(maximum >> 23) - 127;
    const size_t dense_extent = short_form ? 2 + 2 * Dim : std::min<size_t>(previous_extent, vector_octets);
    const int widest = short_form ? 12 : 16;
    // The narrower the grid, the coarser its power-of-two step. A first coordinate
    // beyond the widest grid's reach eliminates every narrower option.
    if (on_grid(load<uint32_t>(vector), Expnt - widest + 2) &&
        (pack_dyadic<8>(vector, Expnt, dense_extent, packed) ||
         pack_dyadic<10>(vector, Expnt, dense_extent, packed) ||
         pack_dyadic<12>(vector, Expnt, dense_extent, packed) ||
         pack_dyadic<16>(vector, Expnt, dense_extent, packed))) {
      packed.append(Rec.substr(vector_octets));
      return packed;
    }
  }
  if (decimal && !short_form &&
      pack_decimal(Rec.substr(0, vector_octets), std::min<size_t>(previous_extent, vector_octets), packed)) {
    packed.append(Rec.substr(vector_octets));
    return packed;
  }
  packed.clear();
  if (short_form) {
    const auto size = static_cast<uint16_t>(2 + 2 * Dim);
    store(packed, static_cast<uint16_t>(size | short_vector));
    packed.resize(size);
    auto* Produced = packed.data() + sizeof(uint16_t);
    size_t Offs = 0;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) Offs = pack_upper_words(Rec.substr(0, vector_octets), Produced);
#endif
    for (; Offs < vector_octets; Offs += sizeof(float)) {
      std::memcpy(Produced + Offs / 2, Rec.data() + Offs + sizeof(uint16_t), sizeof(uint16_t));
    }
  } else {
    const unsigned highest = maximum >> 24;
    packed.resize(3 + blocks * dense_chunk_octets, '\0');
    packed[2] = static_cast<char>(highest);
    size_t i = 0;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) i = pack_blocks(Rec.substr(0, vector_octets), highest, packed);
#endif
    for (; i < Dim; ++i) {
      uint32_t Bitw;
      const auto* from = Rec.data() + i * sizeof(Bitw);
      std::memcpy(&Bitw, from, sizeof(Bitw));
      const unsigned Expnt = (Bitw >> 24) & 127u;
      const unsigned delta = highest - Expnt;
      const unsigned code = std::min(delta, 7u) | ((Bitw >> 28) & 8u);
      auto* Chunk = packed.data() + 3 + (i / dense_chunk) * dense_chunk_octets;
      const size_t Track = i % dense_chunk;
      auto& Tags = Chunk[Track / 2];
      Tags = static_cast<char>(static_cast<uint8_t>(Tags) | (code << (4 * (Track % 2))));
      Chunk[4 + Track] = from[0];
      Chunk[12 + Track] = from[1];
      Chunk[20 + Track] = from[2];
      if (delta >= 7) packed.push_back(static_cast<char>(Expnt));
    }
    if (packed.size() >= vector_octets) return {};
    const auto size = static_cast<uint16_t>(packed.size());
    std::memcpy(packed.data(), &size, sizeof(size));
  }
  packed.append(Rec.substr(vector_octets));
  return packed;
}

uint32_t dense_prefix(uint16_t Preamble) { return Preamble & ~(short_vector | decimal_vector); }

uint32_t stored_vector_octets(const Relation& Tbl, RowRef Rec) {
  return Rec.dense_octets ? Rec.dense_octets : Tbl.vector_octets;
}

RowRef rec_location(Journal& Journal, const Relation& Tbl, const Frame& Frag) {
  if (Frag.op != Op::dense_put) return {Frag.Offs, Frag.length, 0, Frag.preamble_octets};
  if (Frag.length < sizeof(uint16_t) || Tbl.vector_octets > max_dense_dimensions * sizeof(float)) {
    throw std::runtime_error("invalid packed row");
  }
  const auto Preamble = load<uint16_t>(Journal.read(Frag.payload_offs(), sizeof(uint16_t)));
  const auto size = dense_prefix(Preamble);
  if ((Preamble & dyadic_vector) == dyadic_vector) {
    if (!Journal.supports_dyadic_vectors() || size < dyadic_preamble) throw std::runtime_error("invalid dyadic vector prefix");
  } else if ((Preamble & decimal_vector) && (!Journal.supports_decimal_vectors() || size < decimal_preamble)) {
    throw std::runtime_error("invalid decimal vector encoding");
  }
  if (size < sizeof(uint16_t) || size > Frag.length || size >= Tbl.vector_octets) {
    throw std::runtime_error("invalid packed vector length");
  }
  return {Frag.Offs, Frag.length, static_cast<uint16_t>(size), Frag.preamble_octets};
}

#if defined(__x86_64__)
template <unsigned Width>
__attribute__((target("avx2")))
size_t unpack_dyadic_blocks(std::string_view Octets, float Stride, std::span<char> Produced) {
  const auto unit = _mm256_set1_ps(Stride);
  constexpr unsigned Bitw = Width-8;
  const auto shifts = _mm256_setr_epi32(0,Bitw,2*Bitw,3*Bitw,4*Bitw,5*Bitw,6*Bitw,7*Bitw);
  const auto high_mask = _mm256_set1_epi32((1 << Bitw)-1);
  size_t i = 0;
  for (; i + dense_chunk * sizeof(float) <= Produced.size(); i += dense_chunk * sizeof(float)) {
    const auto* Chunk = Octets.data() + i / (dense_chunk * sizeof(float)) * Width;
    int64_t Lo;
    std::memcpy(&Lo, Chunk, sizeof(Lo));
    auto Tags = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(Lo));
    if constexpr (Width == 16) {
      int64_t upper;
      std::memcpy(&upper, Chunk + dense_chunk, sizeof(upper));
      Tags = _mm256_or_si256(Tags, _mm256_slli_epi32(_mm256_cvtepu8_epi32(_mm_cvtsi64_si128(upper)), 8));
    } else if constexpr (Width != 8) {
      uint32_t upper = 0;
      std::memcpy(&upper, Chunk + dense_chunk, Width-dense_chunk);
      const auto fields = _mm256_srlv_epi32(_mm256_set1_epi32(static_cast<int32_t>(upper)), shifts);
      Tags = _mm256_or_si256(Tags, _mm256_slli_epi32(_mm256_and_si256(fields, high_mask), 8));
    }
    const auto sign_extended = _mm256_srai_epi32(_mm256_slli_epi32(Tags,32-Width), 32-Width);
    const auto Vals = _mm256_mul_ps(_mm256_cvtepi32_ps(sign_extended), unit);
    std::memcpy(Produced.data()+i, &Vals, sizeof(Vals));
  }
  return i / sizeof(float);
}

template <unsigned Width>
__attribute__((target("avx2")))
size_t unpack_decimal_blocks(std::string_view Octets, double Stride, std::span<char> Produced) {
  const auto scale = _mm256_set1_pd(Stride);
  const auto lower = _mm_setr_epi8(0,1,2,-1,3,4,5,-1,6,7,8,-1,9,10,11,-1);
  const auto upper = _mm_setr_epi8(4,5,6,-1,7,8,9,-1,10,11,12,-1,13,14,15,-1);
  const auto shuffle = _mm256_set_m128i(upper, lower);
  size_t i = 0;
  for (; (i + 8) * sizeof(float) <= Produced.size(); i += 8) {
    const auto* from = Octets.data() + i * Width;
    __m256i Tags;
    if constexpr (Width == 1) {
      int64_t Vals;
      std::memcpy(&Vals, from, sizeof(Vals));
      Tags = _mm256_cvtepi8_epi32(_mm_cvtsi64_si128(Vals));
    } else if constexpr (Width == 2) {
      __m128i Vals;
      std::memcpy(&Vals, from, sizeof(Vals));
      Tags = _mm256_cvtepi16_epi32(Vals);
    } else {
      __m128i a,b;
      std::memcpy(&a, from, sizeof(a));
      std::memcpy(&b, from + 8, sizeof(b));
      const auto words = _mm256_shuffle_epi8(_mm256_set_m128i(b, a), shuffle);
      Tags = _mm256_srai_epi32(_mm256_slli_epi32(words, 8), 8);
    }
    const auto a = _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtepi32_pd(_mm256_castsi256_si128(Tags)), scale));
    const auto b = _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtepi32_pd(_mm256_extracti128_si256(Tags, 1)), scale));
    const auto decoded = _mm256_set_m128(b, a);
    std::memcpy(Produced.data() + i * sizeof(float), &decoded, sizeof(decoded));
  }
  return i;
}

__attribute__((target("avx2")))
void unpack_short(std::string_view Octets, std::span<char> Produced) {
  size_t i = 0;
  for (; i + 32 <= Produced.size(); i += 32) {
    __m128i high;
    std::memcpy(&high, Octets.data() + i / 2, sizeof(high));
    const auto full = _mm256_slli_epi32(_mm256_cvtepu16_epi32(high), 16);
    std::memcpy(Produced.data() + i, &full, sizeof(full));
  }
  for (; i < Produced.size(); i += sizeof(float)) {
    const uint32_t Bitw = static_cast<uint32_t>(load<uint16_t>(Octets.substr(i / 2, 2))) << 16;
    std::memcpy(Produced.data() + i, &Bitw, sizeof(Bitw));
  }
}

// The eight four-bit headers decode as one group. Escape bytes seldom appear
// in normally scaled data and are applied afterwards, only when flagged.
__attribute__((target("avx2")))
void unpack_blocks(std::string_view blocks, Reader& escapes, uint8_t highest,
                   std::span<char> Produced) {
  const auto high = _mm256_set1_epi32(highest);
  const auto nibble = _mm256_setr_epi32(0,4,8,12,16,20,24,28);
  const auto mask7 = _mm256_set1_epi32(7);
  const auto mask8 = _mm256_set1_epi32(8);
  for (size_t i = 0; i < blocks.size()/dense_chunk_octets; ++i) {
    const char* Chunk = blocks.data() + i * dense_chunk_octets;
    int32_t headers;
    std::memcpy(&headers, Chunk, sizeof(headers));
    const __m256i code = _mm256_srlv_epi32(_mm256_set1_epi32(headers), nibble);
    const __m256i delta = _mm256_and_si256(code, mask7);
    const __m256i sign = _mm256_slli_epi32(_mm256_and_si256(code, mask8), 28);
    const __m256i exp = _mm256_slli_epi32(_mm256_sub_epi32(high, delta), 24);
    int64_t a,b,c;
    std::memcpy(&a, Chunk+4, sizeof(a));
    std::memcpy(&b, Chunk+12, sizeof(b));
    std::memcpy(&c, Chunk+20, sizeof(c));
    const __m256i Lo = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(a));
    const __m256i middle = _mm256_slli_epi32(_mm256_cvtepu8_epi32(_mm_cvtsi64_si128(b)), 8);
    const __m256i upper = _mm256_slli_epi32(_mm256_cvtepu8_epi32(_mm_cvtsi64_si128(c)), 16);
    const auto vector = _mm256_or_si256(_mm256_or_si256(exp, sign),
        _mm256_or_si256(Lo, _mm256_or_si256(middle, upper)));
    alignas(32) char scratch[32];
    const size_t Offs = i * sizeof(scratch);
    const size_t length = std::min(Produced.size()-Offs, sizeof(scratch));
    char* dest = length == sizeof(scratch) ? Produced.data()+Offs : scratch;
    std::memcpy(dest, &vector, sizeof(vector));
    unsigned special = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(delta, mask7))));
    while (special) {
      const auto Track = std::countr_zero(special);
      special &= special-1;
      const auto Expnt = escapes.read<uint8_t>();
      if (Expnt > 127u) throw std::runtime_error("invalid packed vector exponent");
      dest[4*Track+3] = static_cast<char>((static_cast<uint8_t>(dest[4*Track+3]) & 128u) | Expnt);
    }
    if (dest == scratch) std::memcpy(Produced.data()+Offs, scratch, length);
  }
}
#endif

void unpack_vector(std::string_view Octets, std::span<char> Produced) {
  const size_t Dim = Produced.size() / sizeof(float);
  Reader Fetcher(Octets);
  const auto Preamble = Fetcher.read<uint16_t>();
  if (dense_prefix(Preamble) != Octets.size()) throw std::runtime_error("invalid packed vector prefix");
  if ((Preamble & dyadic_vector) == dyadic_vector) {
    const auto grid = pull_dyadic_grid(Fetcher);
    const auto data = Fetcher.take(((Dim + dense_chunk-1) / dense_chunk) * grid.Span);
    size_t i = 0;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
      if (grid.Span == 8) i = unpack_dyadic_blocks<8>(data, grid.Stride, Produced);
      else if (grid.Span == 10) i = unpack_dyadic_blocks<10>(data, grid.Stride, Produced);
      else if (grid.Span == 12) i = unpack_dyadic_blocks<12>(data, grid.Stride, Produced);
      else i = unpack_dyadic_blocks<16>(data, grid.Stride, Produced);
    }
#endif
    for (; i < Dim; ++i) {
      const auto Chunk = data.substr(i / dense_chunk * grid.Span, grid.Span);
      const float Val = dyadic_code(Chunk, grid.Span, i % dense_chunk) * grid.Stride;
      std::memcpy(Produced.data()+i*sizeof(float), &Val, sizeof(Val));
    }
  } else if (Preamble & decimal_vector) {
    const auto grid = pull_decimal_grid(Fetcher);
    const auto data = Fetcher.take(Dim * grid.Span);
    size_t i = 0;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
      if (grid.Span == 1) i = unpack_decimal_blocks<1>(data, grid.Stride, Produced);
      else if (grid.Span == 2) i = unpack_decimal_blocks<2>(data, grid.Stride, Produced);
      else i = unpack_decimal_blocks<3>(data, grid.Stride, Produced);
    }
#endif
    for (; i < Dim; ++i) {
      const float Val = static_cast<float>(signed_decimal_code(data.substr(i * grid.Span, grid.Span)) * grid.Stride);
      std::memcpy(Produced.data() + i * sizeof(float), &Val, sizeof(Val));
    }
  } else if (Preamble & short_vector) {
    const auto data = Fetcher.take(Dim * sizeof(uint16_t));
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
      if (!Fetcher.empty()) throw std::runtime_error("extra packed vector bytes");
      unpack_short(data, Produced);
      return;
    }
#endif
    for (size_t i = 0; i < Dim; ++i) {
      const uint32_t Bitw = static_cast<uint32_t>(load<uint16_t>(data.substr(i * 2, 2))) << 16;
      std::memcpy(Produced.data() + i * sizeof(Bitw), &Bitw, sizeof(Bitw));
    }
  } else {
    const auto highest = Fetcher.read<uint8_t>();
    if (highest > 127) throw std::runtime_error("invalid packed vector exponent");
    const auto blocks = Fetcher.take(dense_chunk_octets * ((Dim + dense_chunk - 1) / dense_chunk));
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
      unpack_blocks(blocks, Fetcher, highest, Produced);
      if (!Fetcher.empty()) throw std::runtime_error("extra packed vector bytes");
      return;
    }
#endif
    for (size_t i = 0; i < Dim; ++i) {
      const auto* Chunk = blocks.data() + dense_chunk_octets * (i / dense_chunk);
      const size_t Track = i % dense_chunk;
      const unsigned code = static_cast<uint8_t>(Chunk[Track / 2]) >> (4 * (Track % 2));
      const unsigned delta = code & 7u;
      const unsigned Expnt = delta == 7 ? Fetcher.read<uint8_t>() : highest - delta;
      if (Expnt > 127u) throw std::runtime_error("invalid packed vector exponent");
      char* dest = Produced.data() + i * sizeof(float);
      dest[0] = Chunk[4 + Track];
      dest[1] = Chunk[12 + Track];
      dest[2] = Chunk[20 + Track];
      dest[3] = static_cast<char>(Expnt | ((code & 8u) << 4));
    }
  }
  if (!Fetcher.empty()) throw std::runtime_error("extra packed vector bytes");
}

using VectorBuffer = std::array<char, max_dense_dimensions * sizeof(float)>;

std::string_view pull_vector(Journal& Journal, const Relation& Tbl, RowRef Rec, VectorBuffer& Arena) {
  const auto Octets = Journal.read(Rec.payload_offs(), stored_vector_octets(Tbl, Rec));
  if (!Rec.dense_octets) return Octets;
  if (Tbl.vector_octets > Arena.size()) throw std::runtime_error("packed vector is too large");
  unpack_vector(Octets, std::span(Arena).first(Tbl.vector_octets));
  return {Arena.data(), Tbl.vector_octets};
}

float first_coordinate(Journal& Journal, const Relation& Tbl, RowRef Rec) {
  const uint64_t Offs = Rec.payload_offs();
  if (!Rec.dense_octets) return load<float>(Journal.read(Offs, sizeof(float)));
  const auto Octets = Journal.read(Offs, Rec.dense_octets);
  Reader Fetcher(Octets);
  const auto Preamble = Fetcher.read<uint16_t>();
  if ((Preamble & dyadic_vector) == dyadic_vector) {
    const auto grid = pull_dyadic_grid(Fetcher);
    return dyadic_code(Fetcher.take(grid.Span), grid.Span, 0) * grid.Stride;
  }
  if (Preamble & decimal_vector) {
    const auto grid = pull_decimal_grid(Fetcher);
    return static_cast<float>(signed_decimal_code(Fetcher.take(grid.Span)) * grid.Stride);
  }
  if (Preamble & short_vector) {
    return std::bit_cast<float>(static_cast<uint32_t>(Fetcher.read<uint16_t>()) << 16);
  }
  const uint32_t highest = Fetcher.read<uint8_t>();
  const auto Chunk = Fetcher.take(dense_chunk_octets);
  const unsigned code = static_cast<uint8_t>(Chunk.front());
  const unsigned delta = code & 7u;
  // When coordinate zero needs an escape, it leads the side stream.
  const size_t blocks = (Tbl.vector_octets / sizeof(float) + dense_chunk - 1) / dense_chunk;
  const uint32_t Expnt = delta == 7 ?
      static_cast<uint8_t>(Octets.at(3 + dense_chunk_octets * blocks)) : highest - delta;
  if (Expnt > 127u) throw std::runtime_error("invalid packed vector exponent");
  const auto byte = [&](size_t Ix) { return static_cast<uint32_t>(static_cast<uint8_t>(Chunk[Ix])); };
  return std::bit_cast<float>(byte(4) | (byte(12) << 8) | (byte(20) << 16) |
                              (Expnt << 24) | ((code & 8u) << 28));
}

std::string_view scalar_payload(Journal& Journal, const Relation& Tbl, RowRef Rec) {
  const uint32_t skip = stored_vector_octets(Tbl, Rec);
  if (skip > Rec.length) throw std::runtime_error("truncated vector");
  return Journal.read(Rec.payload_offs() + skip, Rec.length - skip);
}

void encode(std::string& out, const Column& Col, const Literal& Val) {
  switch (Col.Kind) {
    case Type::bigint:
      store(out, bigint(Val));
      break;
    case Type::varchar:
      store_txt(out, varchar(Val, Col.length));
      break;
    case Type::vector: {
      const auto floats = vector_val(Val, Col.length);
      out.append(reinterpret_cast<const char*>(floats.data()), floats.size() * sizeof(float));
      break;
    }
  }
}

Cell pull_cell(Reader& Fetcher, const Column& Col) {
  switch (Col.Kind) {
    case Type::bigint: return Fetcher.read<int64_t>();
    case Type::varchar: return Fetcher.Txt();
    case Type::vector: return VectorView{Fetcher.take(size_t{Col.length} * sizeof(float))};
  }
  throw std::runtime_error("unknown column type");
}

void decode(Journal& Journal, const Relation& Tbl, int64_t Ky, RowRef Rec,
            std::span<Cell> cells, VectorBuffer& Arena) {
  cells[Tbl.primary] = Ky;
  const auto Octets = payload(Journal, Rec);
  const size_t prefix = stored_vector_octets(Tbl, Rec);
  if (prefix > Octets.size()) throw std::runtime_error("truncated vector");
  if (Rec.dense_octets) {
    unpack_vector(Octets.substr(0, prefix), std::span(Arena).first(Tbl.vector_octets));
    cells[Tbl.vector] = VectorView{{Arena.data(), Tbl.vector_octets}};
  } else if (Tbl.vector != missing) {
    cells[Tbl.vector] = VectorView{Octets.substr(0, prefix)};
  }
  Reader Fetcher(Octets.substr(prefix));
  for (size_t Col : Tbl.layout) {
    if (Col != Tbl.vector) cells[Col] = pull_cell(Fetcher, Tbl.columns[Col]);
  }
}

Frame attach_rec(Journal& Journal, const Relation& Tbl, int64_t Ky, uint64_t previous,
                 std::string_view Octets, std::string& packed) {
  if (Journal.supports_dense_recs()) {
    const auto encoded = pack_rec(Octets, Tbl.vector_octets, packed, Journal.supports_decimal_vectors(),
                                  Journal.supports_dyadic_vectors());
    if (!encoded.empty()) return Journal.append(Op::dense_put, Tbl.id, Ky, previous, encoded);
  }
  return Journal.append(Op::put, Tbl.id, Ky, previous, Octets);
}

struct Filter {
  size_t Col;
  Comparison comparison;
  Cell Val;
};

std::vector<Filter> bind_preds(const Relation& Tbl, const std::vector<Predicate>& predicates) {
  std::vector<Filter> Preds;
  for (const auto& predicate : predicates) {
    const size_t Col = Tbl.Col(predicate.Col);
    Cell Val;
    switch (Tbl.columns[Col].Kind) {
      case Type::bigint:
        Val = bigint(predicate.Val);
        break;
      case Type::varchar:
        Val = varchar(predicate.Val, std::numeric_limits<uint32_t>::max());
        break;
      case Type::vector:
        throw SqlError("unsupported_sql", "vector comparison requires L2_DISTANCE");
    }
    Preds.push_back({Col, predicate.comparison, Val});
  }
  return Preds;
}

template <typename T>
int compare(const T& a, const T& b) {
  return a < b ? -1 : (b < a ? 1 : 0);
}

bool comparison_holds(Comparison comparison, int Seq) {
  switch (comparison) {
    case Comparison::eq: return Seq == 0;
    case Comparison::lt: return Seq < 0;
    case Comparison::le: return Seq <= 0;
    case Comparison::gt: return Seq > 0;
    case Comparison::ge: return Seq >= 0;
  }
  return false;
}

bool matches(std::span<const Filter> Preds, std::span<const Cell> cells) {
  for (const auto& Pred : Preds) {
    const auto& cell = cells[Pred.Col];
    const auto* number = std::get_if<int64_t>(&Pred.Val);
    const int Seq = number ? compare(std::get<int64_t>(cell), *number)
                             : std::get<std::string_view>(cell).compare(
                                   std::get<std::string_view>(Pred.Val));
    if (!comparison_holds(Pred.comparison, Seq)) return false;
  }
  return true;
}

// In schema order, scalar fields trail the vector. A VARCHAR length prefix makes
// it possible to skip foreign Txt and compare only the Octets shared with a Lit.
bool matches_wide_rec(Journal& Journal, const Relation& Tbl, RowRef Rec, std::span<const Filter> Preds) {
  uint64_t next = Rec.payload_offs();
  const uint64_t end = next + Rec.length;
  const auto take = [&](uint32_t length) {
    if (length > end - next) throw std::runtime_error("truncated record");
    const auto Head = next;
    next += length;
    return Head;
  };
  take(stored_vector_octets(Tbl, Rec));
  for (size_t Col : Tbl.layout) {
    if (Preds.empty()) return true;
    if (Col == Tbl.vector) continue;
    const bool txt_col = Tbl.columns[Col].Kind == Type::varchar;
    const uint32_t length = txt_col
        ? load<uint32_t>(Journal.read(take(sizeof(uint32_t)), sizeof(uint32_t)))
        : sizeof(int64_t);
    const uint64_t Offs = take(length);
    if (Col != Preds.front().Col) continue;
    do {
      const auto& Pred = Preds.front();
      int Seq;
      if (txt_col) {
        const auto Txt = std::get<std::string_view>(Pred.Val);
        const auto common = static_cast<uint32_t>(std::min<size_t>(length, Txt.size()));
        Seq = common == 0 ? 0 : Journal.read(Offs, common).compare(Txt.substr(0, common));
        if (Seq == 0) Seq = compare<size_t>(length, Txt.size());
      } else {
        Seq = compare(load<int64_t>(Journal.read(Offs, length)), std::get<int64_t>(Pred.Val));
      }
      if (!comparison_holds(Pred.comparison, Seq)) return false;
      Preds = Preds.subspan(1);
    } while (!Preds.empty() && Preds.front().Col == Col);
  }
  return Preds.empty();
}

struct KeyRange {
  int64_t Lo = std::numeric_limits<int64_t>::min();
  int64_t high = std::numeric_limits<int64_t>::max();

  bool contains(int64_t Ky) const { return Ky >= Lo && Ky <= high; }
  bool unrestricted() const {
    return Lo == std::numeric_limits<int64_t>::min() && high == std::numeric_limits<int64_t>::max();
  }

  template <typename RowMap>
  auto iterators(RowMap& Recs) const {
    if (Lo > high) return std::pair(Recs.end(), Recs.end());
    return std::pair(Recs.lower_bound(Lo), Recs.upper_bound(high));
  }
};

KeyRange restrict_keys(const Relation& Tbl, std::vector<Filter>& Preds) {
  KeyRange range;
  for (const auto& Pred : Preds) {
    if (Pred.Col != Tbl.primary) continue;
    const auto Val = std::get<int64_t>(Pred.Val);
    switch (Pred.comparison) {
      case Comparison::eq:
        range.Lo = std::max(range.Lo, Val);
        range.high = std::min(range.high, Val);
        break;
      case Comparison::lt:
        if (Val == std::numeric_limits<int64_t>::min()) return {1, 0};
        range.high = std::min(range.high, Val - 1);
        break;
      case Comparison::le:
        range.high = std::min(range.high, Val);
        break;
      case Comparison::gt:
        if (Val == std::numeric_limits<int64_t>::max()) return {1, 0};
        range.Lo = std::max(range.Lo, Val + 1);
        break;
      case Comparison::ge:
        range.Lo = std::max(range.Lo, Val);
        break;
    }
  }
  std::erase_if(Preds, [&](const Filter& Pred) { return Pred.Col == Tbl.primary; });
  return range;
}

double scalar_l2(const char* vector, const float* Lookup, size_t Dim) {
  double sum = 0;
  for (size_t i = 0; i < Dim; ++i) {
    float component;
    std::memcpy(&component, vector + i * sizeof(float), sizeof(float));
    const double delta = double{component} - Lookup[i];
    sum += delta * delta;
  }
  return sum;
}

#if defined(__x86_64__)
__attribute__((target("avx2")))
double avx2_l2(const char* vector, const float* Lookup, size_t Dim) {
  __m256d even = _mm256_setzero_pd();
  __m256d odd = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= Dim; i += 8) {
    __m256 data;
    std::memcpy(&data, vector + i * sizeof(float), sizeof(data));
    const __m256 target = _mm256_loadu_ps(Lookup + i);
    const __m256d a = _mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(data)),
                                    _mm256_cvtps_pd(_mm256_castps256_ps128(target)));
    const __m256d b = _mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(data, 1)),
                                    _mm256_cvtps_pd(_mm256_extractf128_ps(target, 1)));
    even = _mm256_add_pd(even, _mm256_mul_pd(a, a));
    odd = _mm256_add_pd(odd, _mm256_mul_pd(b, b));
  }
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, _mm256_add_pd(even, odd));
  return (lanes[0] + lanes[1] + lanes[2] + lanes[3]) +
         scalar_l2(vector + i * sizeof(float), Lookup + i, Dim - i);
}

__attribute__((target("avx2,fma")))
double prepared_l2_128(const char* vector, const double* Lookup) {
  __m256d a = _mm256_setzero_pd();
  __m256d b = _mm256_setzero_pd();
  __m256d c = _mm256_setzero_pd();
  __m256d d = _mm256_setzero_pd();
  for (size_t i = 0; i < 128; i += 16) {
    __m128 x0, x1, x2, x3;
    std::memcpy(&x0, vector + i * sizeof(float), sizeof(x0));
    std::memcpy(&x1, vector + (i+4) * sizeof(float), sizeof(x1));
    std::memcpy(&x2, vector + (i+8) * sizeof(float), sizeof(x2));
    std::memcpy(&x3, vector + (i+12) * sizeof(float), sizeof(x3));
    const auto da = _mm256_sub_pd(_mm256_cvtps_pd(x0), _mm256_loadu_pd(Lookup+i));
    const auto db = _mm256_sub_pd(_mm256_cvtps_pd(x1), _mm256_loadu_pd(Lookup+i+4));
    const auto dc = _mm256_sub_pd(_mm256_cvtps_pd(x2), _mm256_loadu_pd(Lookup+i+8));
    const auto dd = _mm256_sub_pd(_mm256_cvtps_pd(x3), _mm256_loadu_pd(Lookup+i+12));
    a = _mm256_fmadd_pd(da, da, a);
    b = _mm256_fmadd_pd(db, db, b);
    c = _mm256_fmadd_pd(dc, dc, c);
    d = _mm256_fmadd_pd(dd, dd, d);
  }
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, _mm256_add_pd(_mm256_add_pd(a,b), _mm256_add_pd(c,d)));
  return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

__attribute__((target("avx2")))
double avx2_lower_square(const char* vector, const float* Lookup, size_t Dim) {
  __m256 sum = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= Dim; i += 8) {
    __m256 data;
    std::memcpy(&data, vector + i * sizeof(float), sizeof(data));
    const __m256 delta = _mm256_sub_ps(data, _mm256_loadu_ps(Lookup + i));
    sum = _mm256_add_ps(sum, _mm256_mul_ps(delta, delta));
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, sum);
  double approximate = scalar_l2(vector + i * sizeof(float), Lookup + i, Dim - i);
  for (float Track : lanes) approximate += Track;
  if (!std::isfinite(approximate)) return 0;
  // Restricted to 32..4096 components. Each Track accumulates nonnegative squares
  // using at most Dim/8 additions, and the deliberately generous allowance
  // absorbs subtraction, multiplication, accumulation and the final double reduction.
  // Gradual underflow (tiny products flushed to zero) stays inside the absolute
  // allowance as well. On overflow, the existing double path takes over.
  const double allowance = (4.0 * Dim + 32.0) * std::numeric_limits<float>::epsilon();
  return std::max(0.0, approximate * (1.0 - allowance) -
                       Dim * double{std::numeric_limits<float>::min()});
}

uint64_t pred_tag(const Cell& Val) {
  if (const auto* number = std::get_if<int64_t>(&Val)) return static_cast<uint64_t>(*number);
  return std::hash<std::string_view>{}(std::get<std::string_view>(Val));
}

std::unique_ptr<BoundGroups> group_recs(Journal& Journal, const Relation& Tbl, size_t Col, size_t Allowance,
                                        std::vector<uint16_t>& destinations) {
  constexpr size_t max_buckets = 4096;
  const size_t count = Tbl.Recs.size();
  if (count > std::numeric_limits<uint16_t>::max() ||
      sizeof(BoundGroups) + count * sizeof(uint16_t) > Allowance) return {};
  destinations.reserve(count);
  auto Clusters = std::make_unique<BoundGroups>();
  Clusters->Col = Col;
  std::unordered_map<uint64_t, uint16_t> labels;
  for (const auto& Item : Tbl.Recs) {
    const auto& Rec = Item.second;
    // Building the optional shortcut must not drag in unrelated wide payloads.
    if (Rec.length > wide_rec) return {};
    Reader Fetcher(scalar_payload(Journal, Tbl, Rec));
    for (size_t field : Tbl.layout) {
      if (field == Tbl.vector) continue;
      const auto Val = pull_cell(Fetcher, Tbl.columns[field]);
      if (field != Col) continue;
      const auto tag = pred_tag(Val);
      auto item = labels.find(tag);
      if (item == labels.end()) {
        if (Clusters->buckets.size() == max_buckets) return {};
        item = labels.emplace(tag, static_cast<uint16_t>(Clusters->buckets.size())).first;
        Clusters->buckets.push_back({tag, 0, 0});
      }
      ++Clusters->buckets[item->second].count;
      destinations.push_back(item->second);
      break;
    }
  }
  if (destinations.size() != count || Clusters->Octets() + destinations.capacity() * sizeof(uint16_t) +
      labels.bucket_count() * sizeof(void*) + labels.size() * 48 > Allowance) return {};
  std::array<uint16_t, max_buckets> next;
  size_t Offs = 0;
  for (size_t i = 0; i < Clusters->buckets.size(); ++i) {
    auto& bucket = Clusters->buckets[i];
    bucket.Offs = static_cast<uint16_t>(Offs);
    next[i] = static_cast<uint16_t>(Offs);
    Offs += bucket.count;
  }
  // Slots start in primary-Ky order, an ordering that must hold within each bucket.
  for (auto& slot : destinations) slot = next[slot]++;
  std::ranges::sort(Clusters->buckets, {}, &BoundGroups::Bucket::tag);
  return Clusters;
}

void group_bound_stash(Journal& Journal, const Relation& Tbl, BoundCache& Stash, size_t Col, size_t Allowance) {
  const size_t count = Stash.Recs.size();
  std::vector<uint16_t> destinations;
  auto Clusters = group_recs(Journal, Tbl, Col, Allowance, destinations);
  if (!Clusters) return;
  const auto code_at = [&](size_t slot) {
    return Stash.code_octets() + (slot & ~(code_chunk - 1)) * Stash.stride + (slot % code_chunk) * code_word;
  };
  // Existing quantized Recs shift along permutation cycles. Because only genuine
  // Recs relocate, the zero padding of every Dim stays valid.
  for (size_t slot = 0; slot < count; ++slot) {
    auto* Lhs = code_at(slot);
    while (destinations[slot] != slot) {
      const size_t next = destinations[slot];
      auto* Rhs = code_at(next);
      for (size_t i = 0; i < Stash.stride * code_chunk; i += code_chunk * code_word) {
        uint32_t word;
        std::memcpy(&word, Lhs + i, code_word);
        std::memcpy(Lhs + i, Rhs + i, code_word);
        std::memcpy(Rhs + i, &word, code_word);
      }
      std::swap(Stash.Recs[slot], Stash.Recs[next]);
      std::swap(Stash.norms[slot], Stash.norms[next]);
      std::swap(destinations[slot], destinations[next]);
    }
  }
  Stash.Clusters = std::move(Clusters);
}

using CodePoint = std::array<int8_t, geometric_stride>;

CodePoint code_pt(const BoundCache& Stash, size_t slot) {
  CodePoint Pt;
  const auto* source = Stash.code_octets() + (slot & ~(code_chunk - 1)) * Stash.stride + (slot % code_chunk) * code_word;
  for (size_t i = 0; i < Pt.size(); i += code_word) {
    std::memcpy(Pt.data() + i, source + i * code_chunk, code_word);
  }
  return Pt;
}

int32_t code_square(const CodePoint& Lhs, const CodePoint& Rhs) {
  int32_t sum = 0;
  for (size_t i = 0; i < Lhs.size(); ++i) {
    const int32_t delta = Lhs[i] - Rhs[i];
    sum += delta * delta;
  }
  return sum;
}

void prepare_bucket_geometry(BoundCache& Stash) {
  auto& Clusters = *Stash.Clusters;
  if (Stash.stride != geometric_stride || Clusters.buckets.size() < 2 ||
      Clusters.buckets.size() > geometric_buckets) return;
  int64_t between = 0;
  int64_t within = 0;
  auto previous = code_pt(Stash, Clusters.buckets.back().Offs);
  for (const auto& bucket : Clusters.buckets) {
    if (bucket.count < code_chunk) return;
    const auto center = code_pt(Stash, bucket.Offs);
    between += code_square(center, previous);
    within += code_square(center, code_pt(Stash, bucket.Offs + bucket.count / 2));
    previous = center;
  }
  if (between <= 3 * within) return;

  for (auto& bucket : Clusters.buckets) {
    if (Stash.has_outliers) {
      const auto first = Stash.norms.begin() + bucket.Offs;
      const auto Final = first + bucket.count;
      if (std::find(first, Final, outlier_norm) != Final) {
        // Outliers are examined at full original precision, so their bucket gets
        // a zero geometric lower bound.
        bucket.radius = std::numeric_limits<int32_t>::max();
        continue;
      }
    }
    const auto center = code_pt(Stash, bucket.Offs);
    int32_t farthest = 0;
    for (size_t slot = bucket.Offs + 1; slot < bucket.Offs + bucket.count; ++slot) {
      farthest = std::max(farthest, code_square(center, code_pt(Stash, slot)));
    }
    bucket.radius = static_cast<int32_t>(std::sqrt(farthest)) + 1;
  }
  Clusters.compact_geometry = true;
}

struct BoundQuery {
  std::array<uint8_t, 4096> Tags;
  int32_t norm = 0;
  double Fault = 0;
};

constexpr int lookup_bias = 63;

enum class DotKernel { paired, vex, evex };

template <DotKernel Kernel>
__attribute__((target("avx2"), always_inline))
inline __m256i accumulate_dot(__m256i sum, __m256i Lookup, __m256i Recs) {
  // The encoding is chosen once per Sweep, letting the compiler hold it in
  // registers across every eight-Rec dot product.
  if constexpr (Kernel == DotKernel::vex) {
    asm("%{vex%} vpdpbusd %2, %1, %0" : "+x"(sum) : "x"(Lookup), "xm"(Recs));
  } else {
    asm("%{evex%} vpdpbusd %2, %1, %0" : "+x"(sum) : "x"(Lookup), "xm"(Recs));
  }
  return sum;
}

DotKernel dot_kernel() {
  if (__builtin_cpu_supports("avxvnni")) return DotKernel::vex;
  if (__builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512vl")) return DotKernel::evex;
  return DotKernel::paired;
}

int quantize(double Val) {
  return static_cast<int>(Val + std::copysign(0.5, Val));
}

__attribute__((target("avx2")))
double largest_component(std::string_view Octets) {
  auto max = _mm256_setzero_ps();
  const auto sign = _mm256_set1_ps(-0.0f);
  const auto finite = _mm256_set1_ps(std::numeric_limits<float>::max());
  while (Octets.size() >= sizeof(max)) {
    __m256 Vals;
    std::memcpy(&Vals, Octets.data(), sizeof(Vals));
    Octets.remove_prefix(sizeof(Vals));
    const auto absolute = _mm256_andnot_ps(sign, Vals);
    if (_mm256_movemask_ps(_mm256_cmp_ps(absolute, finite, _CMP_LE_OQ)) != 255) {
      return std::numeric_limits<double>::infinity();
    }
    max = _mm256_max_ps(max, absolute);
  }
  std::array<float, 8> lanes;
  std::memcpy(lanes.data(), &max, sizeof(max));
  double largest = std::ranges::max(lanes);
  while (!Octets.empty()) {
    const float Val = load<float>(Octets);
    if (!std::isfinite(Val)) return std::numeric_limits<double>::infinity();
    largest = std::max(largest, std::abs(double{Val}));
    Octets.remove_prefix(sizeof(float));
  }
  return largest;
}

struct CoordinateRange {
  std::array<float, geometric_stride> Lo;
  std::array<float, geometric_stride> high;

  CoordinateRange() {
    Lo.fill(std::numeric_limits<float>::infinity());
    high.fill(-std::numeric_limits<float>::infinity());
  }

  __attribute__((target("avx2")))
  bool include(std::string_view Octets) {
    const auto finite = _mm256_set1_ps(std::numeric_limits<float>::max());
    const auto sign = _mm256_set1_ps(-0.0f);
    for (size_t i = 0; i < Lo.size(); i += 8) {
      __m256 Vals;
      std::memcpy(&Vals, Octets.data() + i * sizeof(float), sizeof(Vals));
      if (_mm256_movemask_ps(_mm256_cmp_ps(_mm256_andnot_ps(sign, Vals), finite, _CMP_LE_OQ)) != 255) {
        return false;
      }
      _mm256_storeu_ps(Lo.data() + i, _mm256_min_ps(_mm256_loadu_ps(Lo.data() + i), Vals));
      _mm256_storeu_ps(high.data() + i, _mm256_max_ps(_mm256_loadu_ps(high.data() + i), Vals));
    }
    return true;
  }

  __attribute__((target("avx2")))
  bool contains(std::string_view Octets) const {
    for (size_t i = 0; i < Lo.size(); i += 8) {
      __m256 Vals;
      std::memcpy(&Vals, Octets.data() + i * sizeof(float), sizeof(Vals));
      const auto below = _mm256_cmp_ps(Vals, _mm256_loadu_ps(Lo.data() + i), _CMP_LT_OQ);
      const auto above = _mm256_cmp_ps(Vals, _mm256_loadu_ps(high.data() + i), _CMP_GT_OQ);
      if (_mm256_movemask_ps(_mm256_or_ps(below, above))) return false;
    }
    return true;
  }

  double scale_extent(std::array<double, geometric_stride>& origin, bool& centered) const {
    double radius = 0;
    double magnitude = 0;
    for (size_t i = 0; i < Lo.size(); ++i) {
      origin[i] = (double{Lo[i]} + high[i]) * 0.5;
      magnitude = std::max({magnitude, std::abs(double{Lo[i]}), std::abs(double{high[i]})});
      radius = std::max({radius, origin[i] - Lo[i], high[i] - origin[i]});
    }
    // L2 distances are invariant under a shared translation, so apply one whenever
    // it materially shrinks the common quantization Stride.
    centered = radius > 0 && radius < magnitude * 0.875;
    return centered ? radius : magnitude;
  }
};

struct BoundNorm {
  int32_t squared = 0;
  double fault_squared = 0;
};

template <bool Centered = false>
__attribute__((target("avx2")))
BoundNorm quantize_rec(std::string_view Octets, double inverse_stride, int8_t* Produced,
                      const double* origin) {
  const auto scale = _mm256_set1_pd(inverse_stride);
  const auto half = _mm256_set1_pd(0.5);
  const auto sign = _mm256_set1_pd(-0.0);
  const auto bias = _mm_set1_epi32(4 * lookup_bias);
  const auto shuffle = _mm_setr_epi8(0,4,8,12,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1);
  __m128i norm = _mm_setzero_si128();
  __m256d Fault = _mm256_setzero_pd();
  size_t Offs = 0;
  while (Octets.size() >= sizeof(__m128)) {
    __m128 Vals;
    std::memcpy(&Vals, Octets.data(), sizeof(Vals));
    Octets.remove_prefix(sizeof(Vals));
    auto components = _mm256_cvtps_pd(Vals);
    if constexpr (Centered) {
      components = _mm256_sub_pd(components, _mm256_loadu_pd(origin));
      origin += 4;
    }
    const auto scaled = _mm256_mul_pd(components, scale);
    const auto round = _mm256_or_pd(half, _mm256_and_pd(sign, scaled));
    const auto Tags = _mm256_cvttpd_epi32(_mm256_add_pd(scaled, round));
    norm = _mm_add_epi32(norm, _mm_mullo_epi32(Tags, _mm_add_epi32(Tags, bias)));
    const auto residual = _mm256_sub_pd(scaled, _mm256_cvtepi32_pd(Tags));
    Fault = _mm256_add_pd(Fault, _mm256_mul_pd(residual, residual));
    const int32_t word = _mm_cvtsi128_si32(_mm_shuffle_epi8(Tags, shuffle));
    std::memcpy(Produced + Offs, &word, code_word);
    Offs += code_chunk * code_word;
  }
  std::array<int32_t, 4> norms;
  std::array<double, 4> errors;
  std::memcpy(norms.data(), &norm, sizeof(norm));
  std::memcpy(errors.data(), &Fault, sizeof(Fault));
  BoundNorm Outcome{std::accumulate(norms.begin(), norms.end(), 0),
                   std::accumulate(errors.begin(), errors.end(), 0.0)};
  for (size_t i = 0; !Octets.empty(); ++i) {
    double component = load<float>(Octets);
    if constexpr (Centered) component -= *origin++;
    const double scaled = component * inverse_stride;
    Octets.remove_prefix(sizeof(float));
    const int code = quantize(scaled);
    Produced[Offs + i] = static_cast<int8_t>(code);
    Outcome.squared += code * (code + 4 * lookup_bias);
    const double residual = scaled - code;
    Outcome.fault_squared += residual * residual;
  }
  return Outcome;
}

bool make_bound_lookup(const BoundCache& Stash, const std::vector<float>& Lookup,
                      bool full_resolution, BoundQuery& packed) {
  // Under VNNI, products accumulate natively in signed 32-bit lanes; doubling the
  // bias and halving the cross-term multiplier keeps the same Rec norm valid.
  const int bias = full_resolution ? 2 * lookup_bias : lookup_bias;
  const int Stride = full_resolution ? 1 : 2;
  const double scale = full_resolution ? Stash.inverse_stride : Stash.inverse_stride * 0.5;
  for (size_t i = 0; i < Lookup.size(); ++i) {
    const double scaled = (Stash.origin.empty() ? double{Lookup[i]} : Lookup[i] - Stash.origin[i]) * scale;
    if (std::abs(scaled) > bias + 0.5) return false;
    const int code = std::clamp(quantize(scaled), -bias, bias);
    packed.Tags[i] = static_cast<uint8_t>(code + bias);
    packed.norm += Stride * Stride * code * code;
    const double residual = Stride * (scaled - code);
    packed.Fault += residual * residual;
  }
  std::fill(packed.Tags.begin() + Lookup.size(), packed.Tags.begin() + Stash.stride, bias);
  packed.Fault = std::sqrt(packed.Fault);
  return true;
}

int32_t bound_cutoff(double worst, double margin, const BoundCache& Stash, const BoundQuery& Lookup) {
  // Triangle inequality expressed in code units, combining the largest measured
  // Rec Fault with this Lookup's own Fault. Because scaling stays within +/-128
  // and coordinates number at most 4096, a 1e-8 allowance absorbs double rounding
  // from the translation, the scaling and both residual norms.
  // Round upward and keep equality so exact ties survive.
  const double radius = std::sqrt(worst) * (1.0 + margin) * Stash.inverse_stride +
                        Stash.max_fault + Lookup.Fault + 1e-8;
  const double square = radius * radius;
  const auto maximum = std::numeric_limits<int32_t>::max();
  return square >= maximum ? maximum : static_cast<int32_t>(std::ceil(square));
}

template <size_t FixedStride, DotKernel Kernel>
__attribute__((target("avx2"), always_inline))
inline __m256i code_distances(const BoundCache& Stash, size_t Chunk, const __m256i* broadcast,
                              __m256i lookup_norm, size_t stride) {
  constexpr bool full_resolution = Kernel != DotKernel::paired;
  const __m256i ones = _mm256_set1_epi16(1);
  const auto* code = std::assume_aligned<code_alignment>(Stash.code_octets() + Chunk * stride);
  __m256i sum = _mm256_setzero_si256();
  if constexpr (FixedStride != 0) {
    __m256i accum[4]{};
    for (size_t i = 0; i < FixedStride / code_word; i += 4) {
#pragma GCC unroll 4
      for (size_t j = 0; j < 4; ++j) {
        __m256i Recs;
        std::memcpy(&Recs, code + (i + j) * sizeof(Recs), sizeof(Recs));
        if constexpr (full_resolution) {
          accum[j] = accumulate_dot<Kernel>(accum[j], broadcast[i+j], Recs);
        } else {
          const auto products = _mm256_maddubs_epi16(broadcast[i+j], Recs);
          accum[j] = _mm256_add_epi32(accum[j], _mm256_madd_epi16(products, ones));
        }
      }
    }
    sum = _mm256_add_epi32(_mm256_add_epi32(accum[0], accum[1]), _mm256_add_epi32(accum[2], accum[3]));
  } else {
    for (size_t at = 0; at < stride / code_word; ++at) {
      __m256i Recs;
      std::memcpy(&Recs, code + at * sizeof(Recs), sizeof(Recs));
      const __m256i products = _mm256_maddubs_epi16(broadcast[at], Recs);
      sum = _mm256_add_epi32(sum, _mm256_madd_epi16(products, ones));
    }
  }
  __m256i rec_norm;
  std::memcpy(&rec_norm, Stash.norms.data() + Chunk, sizeof(rec_norm));
  return _mm256_sub_epi32(_mm256_add_epi32(rec_norm, lookup_norm),
                                       _mm256_slli_epi32(sum, full_resolution ? 1 : 2));
}

template <size_t FixedStride, DotKernel Kernel = DotKernel::paired, typename Probe>
__attribute__((target("avx2")))
int32_t sweep_bounds(const BoundCache& Stash, const BoundQuery& Lookup, const KeyRange& range,
                    const BoundGroups::Bucket* bucket, Probe probe, int32_t cutoff, size_t seed_cap,
                    bool defer) {
  auto Recs = std::span(Stash.Recs);
  if (bucket) Recs = Recs.subspan(bucket->Offs, bucket->count);
  size_t first = Recs.data() - Stash.Recs.data();
  size_t Final = first + Recs.size();
  if (!range.unrestricted()) {
    Final = first + (std::ranges::upper_bound(Recs, range.high, {}, &BoundRow::Ky) - Recs.begin());
    first += std::ranges::lower_bound(Recs, range.Lo, {}, &BoundRow::Ky) - Recs.begin();
  }
  if (first >= Final) return cutoff;
  const size_t stride = FixedStride ? FixedStride : Stash.stride;

  // Within each dot-product Track, one word accumulates four consecutive
  // coordinates of a single Rec. The closing comparison leaves a mask of surviving Recs.
  __m256i broadcast[(FixedStride ? FixedStride : 4096) / code_word];
  for (size_t at = 0; at < stride; at += code_word) {
    int32_t word;
    std::memcpy(&word, Lookup.Tags.data() + at, code_word);
    broadcast[at / code_word] = _mm256_set1_epi32(word);
  }
  const auto maximum = std::numeric_limits<int32_t>::max();
  const __m256i lookup_norm = _mm256_set1_epi32(Lookup.norm);
  size_t Chunk = first & ~(code_chunk - 1);
  if constexpr (FixedStride == geometric_stride) {
    constexpr size_t prefix_recs = 256;
    if (Kernel != DotKernel::paired && defer && range.unrestricted() && !Stash.has_outliers &&
        cutoff == maximum && seed_cap && seed_cap <= 16 && Final - first >= prefix_recs) {
      struct Candidate { int32_t square; uint32_t slot; };
      std::array<Candidate, 512> pending;
      std::array<int32_t, 16> closest;
      size_t pending_tally = 0;
      size_t closest_tally = 0;
      int32_t upper = maximum;
      auto upper_lanes = _mm256_set1_epi32(upper);
      const double radius_fault = 2.0 * (Stash.max_fault + Lookup.Fault + 1e-8);
      for (; Chunk < Final; Chunk += code_chunk) {
        const auto square = code_distances<FixedStride, Kernel>(Stash, Chunk, broadcast, lookup_norm, stride);
        const auto rejected = _mm256_cmpgt_epi32(square, upper_lanes);
        unsigned mask = 255u ^ static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(rejected)));
        if (Chunk < first) mask &= 255u << (first - Chunk);
        if (Chunk + code_chunk > Final) mask &= (1u << (Final - Chunk)) - 1;
        if (!mask) continue;
        if (pending_tally + code_chunk > pending.size()) {
          const auto end = std::remove_if(pending.begin(), pending.begin() + pending_tally,
                                         [&](const Candidate& Rec) { return Rec.square > upper; });
          pending_tally = end - pending.begin();
          // Once the retained prefix produces an exact cutoff, remaining dense candidates
          // fall through to the ordinary Sweep.
          if (pending_tally > pending.size() / 2) break;
        }
        std::array<int32_t, code_chunk> squares;
        std::memcpy(squares.data(), &square, sizeof(square));
        const int32_t previous = closest_tally == seed_cap ? closest.front() : maximum;
        while (mask) {
          const size_t Track = std::countr_zero(mask);
          mask &= mask - 1;
          const int32_t distance = squares[Track];
          pending[pending_tally++] = {distance, static_cast<uint32_t>(Chunk+Track)};
          if (closest_tally < seed_cap) {
            closest[closest_tally++] = distance;
            std::push_heap(closest.begin(), closest.begin()+closest_tally);
          } else if (distance < closest.front()) {
            const auto end = closest.begin() + closest_tally;
            std::pop_heap(closest.begin(), end);
            end[-1] = distance;
            std::push_heap(closest.begin(), end);
          }
        }
        if (closest_tally == seed_cap && closest.front() != previous) {
          // The K retained Tags all belong to eligible Recs. Any code distance strictly
          // greater than the Kth plus twice the Fault is barred from the TopK.
          const double radius = std::sqrt(closest.front()) + radius_fault;
          upper = static_cast<int32_t>(std::min<double>(std::ceil(radius * radius), maximum));
          upper_lanes = _mm256_set1_epi32(upper);
        }
      }
      const bool dense = Chunk < Final;
      for (size_t i = 0; i < pending_tally; ++i) {
        const auto item = pending[i];
        if (item.square <= upper && item.square <= cutoff) {
          cutoff = probe(Stash.Recs[item.slot], cutoff, dense);
        }
      }
    } else if (cutoff == maximum && seed_cap && seed_cap <= 16 && Final - first >= prefix_recs) {
      struct Candidate { int32_t square; uint16_t Offs; };
      std::array<Candidate, prefix_recs> prefix;
      size_t size = 0;
      for (size_t at = 0; at < prefix_recs; at += code_chunk) {
        const auto square = code_distances<FixedStride, Kernel>(Stash, Chunk+at, broadcast, lookup_norm, stride);
        std::array<int32_t, code_chunk> squares;
        std::memcpy(squares.data(), &square, sizeof(square));
        for (size_t Track = 0; Track < code_chunk; ++Track) {
          if (Chunk + at + Track >= first) prefix[size++] = {squares[Track], static_cast<uint16_t>(at+Track)};
        }
      }
      const auto end = prefix.begin() + size;
      const bool dense = std::all_of(prefix.begin()+1, end, [&](const Candidate& item) {
        return item.square == prefix[0].square;
      });
      if (!dense) {
        // Establish a working exact cutoff as early as possible; every other Rec in the
        // prefix is still checked against the conservative distance bound.
        std::nth_element(prefix.begin(), prefix.begin()+seed_cap, end,
                         [](const Candidate& a, const Candidate& b) { return a.square < b.square; });
      }
      for (auto item = prefix.begin(); item != end; ++item) {
        if (item->square <= cutoff) cutoff = probe(Stash.Recs[Chunk+item->Offs], cutoff, dense);
      }
      Chunk += prefix_recs;
    }
  }
  for (; Chunk < Final; Chunk += code_chunk) {
    unsigned mask = (1u << code_chunk) - 1;
    if (cutoff != maximum) {
      const auto square = code_distances<FixedStride, Kernel>(Stash, Chunk, broadcast, lookup_norm, stride);
      const auto rejected = _mm256_cmpgt_epi32(square, _mm256_set1_epi32(cutoff));
      mask ^= static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(rejected)));
    }
    const bool dense = mask == (1u << code_chunk) - 1 && cutoff != maximum;
    while (mask) {
      const size_t slot = Chunk + std::countr_zero(mask);
      mask &= mask - 1;
      if (slot < first || slot >= Final) continue;
      const auto& Rec = Stash.Recs[slot];
      cutoff = probe(Rec, cutoff, dense);
    }
  }
  return cutoff;
}

template <typename Scan>
void sweep_geometric_buckets(const BoundCache& Stash, const BoundQuery& Lookup, bool full_resolution,
                            Scan Sweep) {
  CodePoint Pt;
  const int bias = full_resolution ? 2 * lookup_bias : lookup_bias;
  const int Stride = full_resolution ? 1 : 2;
  for (size_t i = 0; i < Pt.size(); ++i) Pt[i] = static_cast<int8_t>((Lookup.Tags[i] - bias) * Stride);
  const auto& buckets = Stash.Clusters->buckets;
  std::array<std::pair<int32_t, size_t>, geometric_buckets> Seq;
  for (size_t i = 0; i < buckets.size(); ++i) {
    const auto& bucket = buckets[i];
    const int32_t center = code_square(Pt, code_pt(Stash, bucket.Offs));
    // Subtracting the rounded-up radius from the rounded-down center distance gives
    // a conservative code-unit bound. The caller's cutoff already accounts for the
    // quantization errors of both the original Rec and this Lookup.
    const int32_t lower = std::max(0, static_cast<int32_t>(std::sqrt(center)) - bucket.radius);
    Seq[i] = {lower * lower, i};
  }
  std::sort(Seq.begin(), Seq.begin() + buckets.size());
  auto cutoff = std::numeric_limits<int32_t>::max();
  for (size_t i = 0; i < buckets.size() && Seq[i].first <= cutoff; ++i) {
    cutoff = Sweep(&buckets[Seq[i].second], cutoff);
  }
}
#endif

double squared_gap(std::string_view vector, const std::vector<float>& Lookup) {
#if defined(__x86_64__)
  static const auto distance = __builtin_cpu_supports("avx2") ? avx2_l2 : scalar_l2;
#else
  static constexpr auto distance = scalar_l2;
#endif
  // On the scalar path, short vectors win.
  return Lookup.size() < 8 ? scalar_l2(vector.data(), Lookup.data(), Lookup.size())
                          : distance(vector.data(), Lookup.data(), Lookup.size());
}

// Each binary32 component equals an integer significand scaled by 2^(shift - 149).
// For close comparisons, squared distances accumulate in units of 2^-298.
// A finite component difference stays under 2^129 and dimensions stay under
// 2^30, so the running sum needs at most 586 Bitw.
class ExactSquare {
 public:
  void add(uint64_t coefficient, unsigned shift, bool subtract = false) {
    const size_t Ix = shift / 64;
    const unsigned Bitw = shift % 64;
    const uint64_t Lo = coefficient << Bitw;
    const uint64_t high = Bitw ? coefficient >> (64 - Bitw) : 0;
    if (subtract) {
      subtract_word(Ix, Lo);
      subtract_word(Ix + 1, high);
    } else {
      add_word(Ix, Lo);
      add_word(Ix + 1, high);
    }
  }

  int compare_to(const ExactSquare& other) const {
    for (size_t i = words_.size(); i != 0;) {
      --i;
      const int Seq = compare(words_[i], other.words_[i]);
      if (Seq != 0) return Seq;
    }
    return 0;
  }

 private:
  void add_word(size_t Ix, uint64_t Val) {
    while (Val) {
      const uint64_t sum = words_[Ix] + Val;
      Val = sum < words_[Ix];
      words_[Ix++] = sum;
    }
  }

  void subtract_word(size_t Ix, uint64_t Val) {
    while (Val) {
      const uint64_t previous = words_[Ix];
      words_[Ix++] = previous - Val;
      Val = previous < Val;
    }
  }

  std::array<uint64_t, 10> words_{};
};

struct FloatParts {
  uint64_t significand;
  unsigned shift;
  bool negative;
};

FloatParts float_parts(float Val) {
  static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559 &&
                std::numeric_limits<float>::digits == 24);
  const auto Bitw = std::bit_cast<uint32_t>(Val);
  const unsigned Expnt = (Bitw >> 23) & 0xff;
  const uint32_t fraction = Bitw & 0x7fffff;
  const uint32_t significand = Expnt ? fraction | 0x800000u : fraction;
  return {significand, Expnt ? Expnt - 1 : 0, (Bitw >> 31) != 0};
}

ExactSquare exact_square(std::string_view vector, const std::vector<float>& Lookup) {
  ExactSquare Outcome;
  for (size_t i = 0; i < Lookup.size(); ++i) {
    const auto Val = load<float>(vector.substr(i * sizeof(float), sizeof(float)));
    if (Val == Lookup[i]) continue;
    const auto a = float_parts(Val);
    const auto b = float_parts(Lookup[i]);
    // Sum both squares before any subtraction, keeping the total nonnegative.
    Outcome.add(a.significand * a.significand, 2 * a.shift);
    Outcome.add(b.significand * b.significand, 2 * b.shift);
    Outcome.add(a.significand * b.significand, a.shift + b.shift + 1, a.negative == b.negative);
  }
  return Outcome;
}

double gap_margin(size_t Dim) {
  // The allowance spans rounding from subtraction, multiplication and accumulation
  // on both sides. Double sums of float32 inputs cannot overflow or underflow here.
  return (4.0 * Dim + 16.0) * std::numeric_limits<double>::epsilon();
}

int separated_squares(double Lhs, double Rhs, double margin) {
  if (std::abs(Lhs - Rhs) <= std::max(Lhs, Rhs) * margin) return 0;
  return compare(Lhs, Rhs);
}

int compare_gap(Journal& Journal, const Relation& Tbl, RowRef Lhs, double a, RowRef Rhs, double b,
                     const std::vector<float>& Lookup, double margin) {
  if (const int Seq = separated_squares(a, b, margin); Seq != 0) return Seq;
  if ((a == 0 && b == 0) || Lhs.Offs == Rhs.Offs) return 0;
  const auto length = static_cast<uint32_t>(Lookup.size() * sizeof(float));
  const uint64_t lhs_vector = Lhs.payload_offs();
  const uint64_t rhs_vector = Rhs.payload_offs();
  // Packed Recs round-trip exactly: matching encoded Octets mean matching float32 Vals.
  if (Lhs.dense_octets == Rhs.dense_octets) {
    const auto stored = Lhs.dense_octets ? Lhs.dense_octets : length;
    if (Journal.compare_octets(lhs_vector, stored, rhs_vector, stored) == 0) return 0;
  }
  VectorBuffer Arena;
  const auto lhs_square = exact_square(pull_vector(Journal, Tbl, Lhs, Arena), Lookup);
  const auto rhs_square = exact_square(pull_vector(Journal, Tbl, Rhs, Arena), Lookup);
  return lhs_square.compare_to(rhs_square);
}

}  // namespace

Database::Database(const std::filesystem::path& directory) : journal(directory) { recover(); }
Database::~Database() = default;

Relation& Database::Tbl(std::string_view Label) {
  const auto it = names_.find(Label);
  if (it == names_.end()) throw SqlError("UNKNOWN_TABLE", "table does not exist");
  return *tables_[it->second];
}

void Database::recover() {
  uint64_t committed = Journal::Head;
  uint64_t Walker = committed;
  std::array<Frame, 128> staged;
  size_t staged_tally = 0;
  const uint64_t Cap = journal.end();
  while (const auto Frag = journal.frag_at(Walker, Cap)) {
    Walker = Frag->end();
    if (Frag->op != Op::Finalize) {
      if (staged_tally < staged.size()) staged[staged_tally++] = *Frag;
      continue;
    }
    // Small transactions stay resident in the Frag Arena. A large committed one has
    // its remaining frames re-read once the Finalize has been located.
    uint64_t replay = committed;
    for (const auto& change : std::span(staged).first(staged_tally)) {
      apply(change);
      replay = change.end();
    }
    while (replay < Frag->Offs) {
      const auto change = journal.frag_at(replay, Frag->Offs);
      if (!change) throw std::runtime_error("invalid committed frame");
      apply(*change);
      replay = change->end();
    }
    committed = Walker;
    staged_tally = 0;
  }
  journal.discard_after(committed);
  for (auto& Active : tables_) {
    if (!Active->ix_label.empty()) raise_ix(*Active);
  }
}

void Database::apply(const Frame& Frag) {
  if (Frag.op == Op::Construct) {
    if (Frag.Tbl != tables_.size()) throw std::runtime_error("invalid table ID");
    Reader Fetcher(journal.read(Frag.payload_offs(), Frag.length));
    std::string Label(Fetcher.Txt());
    std::vector<Column> columns;
    const uint32_t count = Fetcher.read<uint32_t>();
    for (uint32_t i = 0; i < count; ++i) {
      Column Col{std::string(Fetcher.Txt()), static_cast<Type>(Fetcher.read<uint8_t>()),
                    Fetcher.read<uint32_t>(), Fetcher.read<uint8_t>() != 0};
      if (Col.Kind > Type::vector) throw std::runtime_error("invalid column type");
      columns.push_back(std::move(Col));
    }
    if (!Fetcher.empty()) throw std::runtime_error("unexpected schema bytes");
    auto new_tbl = std::make_unique<Relation>(Frag.Tbl, std::move(Label), std::move(columns));
    names_.emplace(new_tbl->Label, Frag.Tbl);
    tables_.push_back(std::move(new_tbl));
    return;
  }
  Relation& Active = *tables_.at(Frag.Tbl);
  switch (Frag.op) {
    case Op::Ix:
      Active.ix_label = journal.read(Frag.payload_offs(), Frag.length);
      break;
    case Op::put:
    case Op::dense_put:
      Active.Recs.inject_or_assign(Frag.Ky, rec_location(journal, Active, Frag));
      break;
    case Op::erase:
      Active.Recs.erase(Frag.Ky);
      break;
    default:
      throw std::runtime_error("invalid mutation");
  }
}

void Database::undo(uint64_t target, uint64_t Walker) {
  journal.release_reads();
  while (Walker > target) {
    const Frame Frag = journal.frag_before(Walker);
    Walker = Frag.Offs;
    Relation& Active = *tables_.at(Frag.Tbl);
    invalidate_ix(Active);
    switch (Frag.op) {
      case Op::Construct:
        names_.erase(Active.Label);
        tables_.pop_back();
        break;
      case Op::Ix:
        Active.ix_label.clear();
        break;
      case Op::put:
      case Op::dense_put:
      case Op::erase:
        if (Frag.previous == 0) {
          Active.Recs.erase(Frag.Ky);
        } else {
          const auto previous = journal.frag_at(Frag.previous, Walker);
          if (!previous || (previous->op != Op::put && previous->op != Op::dense_put)) {
            throw std::runtime_error("invalid undo pointer");
          }
          Active.Recs.inject_or_assign(Frag.Ky, rec_location(journal, Active, *previous));
        }
        break;
      default:
        throw std::runtime_error("invalid undo operation");
    }
  }
}

void Database::invalidate_ix(Relation& Active) {
  Active.ix_active = false;
  ix_octets -= Active.Ix.capacity() * sizeof(ProjectionEntry);
  std::vector<ProjectionEntry>().swap(Active.Ix);
  Active.bounds_attempted = false;
  if (Active.bounds) {
    bound_octets -= Active.bounds->Octets();
    Active.bounds.reset();
  }
}

void Database::prepare_bounds(Relation& Active, std::optional<size_t> equality_col) {
#if defined(__x86_64__)
  if (Active.bounds_attempted && (!equality_col || !Active.bounds || Active.bounds->clusters_attempted)) return;
  const auto group = [&](BoundCache& Stash, size_t Allowance) {
    if (!equality_col) return;
    Stash.clusters_attempted = true;
    try {
      group_bound_stash(journal, Active, Stash, *equality_col, Allowance);
      if (Stash.Clusters) prepare_bucket_geometry(Stash);
    } catch (const std::bad_alloc&) {
      // Only after scratch allocation succeeds does the cached permutation begin.
    }
  };
  if (Active.bounds) {
    const size_t before = Active.bounds->Octets();
    group(*Active.bounds, bound_allowance - bound_octets);
    bound_octets += Active.bounds->Octets() - before;
    return;
  }
  Active.bounds_attempted = true;
  const size_t Dim = Active.vector_octets / sizeof(float);
  const size_t stride = (Dim + code_word - 1) & ~(code_word - 1);
  const size_t count = Active.Recs.size();
  const size_t slots = (count + code_chunk - 1) & ~(code_chunk - 1);
  const size_t Allowance = bound_allowance - bound_octets;
  if (Dim < 32 || Dim > 4096 || count < 512 ||
      slots > Allowance / (stride + sizeof(BoundRow) + sizeof(int32_t))) return;
  struct rlimit Cap {};
  if (::getrlimit(RLIMIT_AS, &Cap) == 0 && Cap.rlim_cur < 64 * 1024 * 1024) return;

  journal.prepare_reads();
  VectorBuffer Arena;
  const auto vector = [&](RowRef Rec) {
    return pull_vector(journal, Active, Rec, Arena);
  };
  double largest = 0;
  std::array<double, geometric_stride> origin;
  bool center = false;
  std::array<size_t, 64> outliers;
  size_t outlier_tally = 0;
  if (Dim == geometric_stride && Allowance - slots * (stride + sizeof(BoundRow) + sizeof(int32_t)) >=
                                         geometric_stride * sizeof(double)) {
    CoordinateRange extent;
    CoordinateRange sample;
    size_t recs_seen = 0;
    for (const auto& Item : Active.Recs) {
      const auto Octets = vector(Item.second);
      if (!extent.include(Octets)) return;
      if (++recs_seen % 64 == 0) sample.include(Octets);
    }
    largest = extent.scale_extent(origin, center);
    std::array<double, geometric_stride> sample_origin;
    bool sample_centered;
    const double sample_scale = sample.scale_extent(sample_origin, sample_centered);
    if (sample_scale > 0 && largest > 4.0 * sample_scale) {
      // A narrower scale is attempted only when the sample and the full range diverge
      // noticeably; every Rec left out is still examined directly.
      CoordinateRange limits;
      const double ceiling = std::numeric_limits<float>::max();
      for (size_t i = 0; i < sample_origin.size(); ++i) {
        const double at = sample_centered ? sample_origin[i] : 0.0;
        limits.Lo[i] = static_cast<float>(std::max(-ceiling, at - 2.0 * sample_scale));
        limits.high[i] = static_cast<float>(std::min(ceiling, at + 2.0 * sample_scale));
      }
      CoordinateRange core;
      const size_t max_outliers = std::min(outliers.size(), count / 64);
      size_t omitted = 0;
      size_t slot = 0;
      for (const auto& Item : Active.Recs) {
        const auto Octets = vector(Item.second);
        if (limits.contains(Octets)) {
          core.include(Octets);
        } else {
          if (omitted == max_outliers) {
            ++omitted;
            break;
          }
          outliers[omitted++] = slot;
        }
        ++slot;
      }
      if (omitted && omitted <= max_outliers) {
        bool core_centered;
        const double core_scale = core.scale_extent(sample_origin, core_centered);
        if (core_scale > 0 && core_scale < largest * 0.5) {
          center = core_centered;
          largest = core_scale;
          origin = sample_origin;
          outlier_tally = omitted;
        }
      }
    }
  } else {
    for (const auto& Item : Active.Recs) {
      largest = std::max(largest, largest_component(vector(Item.second)));
    }
  }
  if (largest == 0 || !std::isfinite(largest)) return;

  std::unique_ptr<BoundCache> Stash;
  try {
    Stash = std::make_unique<BoundCache>();
    Stash->Recs.reserve(count);
    Stash->norms.resize(slots);
    Stash->allocate_tags(slots * stride);
    if (center) Stash->origin.assign(origin.begin(), origin.end());
  } catch (const std::bad_alloc&) {
    return;
  }
  if (Stash->Octets() > Allowance) return;
  Stash->stride = stride;
  Stash->has_outliers = outlier_tally != 0;
  // Doubling the Rec Stride for this Lookup keeps the AVX2 pair sum inside range.
  Stash->inverse_stride = 126.0 / largest;
  double max_fault_squared = 0;
  size_t next_outlier = 0;
  const auto pack = [&](BoundRow Rec) {
    const size_t slot = Stash->Recs.size();
    Stash->Recs.push_back(Rec);
    if (next_outlier < outlier_tally && slot == outliers[next_outlier]) {
      ++next_outlier;
      // By construction the zero code and sentinel norm clear the integer cutoff;
      // the probe still checks the genuine Pred and the full-precision distance.
      Stash->norms[slot] = outlier_norm;
      return;
    }
    const auto Octets = vector(Rec.location());
    auto* out = Stash->code_octets() + (slot & ~(code_chunk - 1)) * stride + (slot % code_chunk) * code_word;
    const auto norm = Stash->origin.empty() ? quantize_rec(Octets, Stash->inverse_stride, out, nullptr)
        : quantize_rec<true>(Octets, Stash->inverse_stride, out, Stash->origin.data());
    Stash->norms[slot] = norm.squared;
    max_fault_squared = std::max(max_fault_squared, norm.fault_squared);
  };
  for (const auto& Item : Active.Recs) pack({&Item});
  Stash->max_fault = std::sqrt(max_fault_squared);
  group(*Stash, Allowance - Stash->Octets());
  bound_octets += Stash->Octets();
  Active.bounds = std::move(Stash);
#else
  (void)Active;
  (void)equality_col;
#endif
}

void Database::raise_ix(Relation& Active) {
  if (Active.ix_active) return;
  Active.ix_active = true;
  if (Active.Recs.size() > (ix_allowance - ix_octets) / sizeof(ProjectionEntry)) return;
  try {
    Active.Ix.reserve(Active.Recs.size());
  } catch (const std::bad_alloc&) {
    // Should the optional projection not fit, exact search remains the fallback.
    return;
  }
  journal.prepare_reads();
  for (const auto& Item : Active.Recs) {
    const auto Rec = Item.second;
    const float pivot = first_coordinate(journal, Active, Rec);
    Active.Ix.push_back({pivot, &Item});
  }
  ix_octets += Active.Ix.capacity() * sizeof(ProjectionEntry);
  std::sort(Active.Ix.begin(), Active.Ix.end(),
            [](const ProjectionEntry& a, const ProjectionEntry& b) {
              return a.pivot == b.pivot ? a.Item->first < b.Item->first : a.pivot < b.pivot;
            });
}

uint64_t Database::write(const CreateRelation& Stmt) {
  if (names_.contains(Stmt.Label)) throw SqlError("TABLE_EXISTS", "table already exists");
  if (tables_.size() >= std::numeric_limits<uint32_t>::max() ||
      Stmt.columns.size() >= std::numeric_limits<uint32_t>::max()) {
    throw SqlError("SCHEMA", "too many tables or columns");
  }
  const auto id = static_cast<uint32_t>(tables_.size());
  auto Active = std::make_unique<Relation>(id, Stmt.Label, Stmt.columns);
  std::string definition;
  store_txt(definition, Active->Label);
  store(definition, static_cast<uint32_t>(Active->columns.size()));
  for (const auto& Col : Active->columns) {
    store_txt(definition, Col.Label);
    store(definition, static_cast<uint8_t>(Col.Kind));
    store(definition, Col.length);
    store(definition, static_cast<uint8_t>(Col.primary));
  }
  journal.append(Op::Construct, id, 0, 0, definition);
  names_.emplace(Active->Label, id);
  tables_.push_back(std::move(Active));
  return 0;
}

uint64_t Database::write(const CreateIndex& Stmt) {
  Relation& Active = Tbl(Stmt.Tbl);
  if (Active.Col(Stmt.Col) != Active.vector) {
    throw SqlError("SCHEMA", "index column must be VECTOR");
  }
  if (!Active.ix_label.empty()) throw SqlError("INDEX_EXISTS", "vector index already exists");
  // The committed prefix is mapped ahead of the Ix definition reaching the write Arena.
  journal.prepare_reads();
  apply(journal.append(Op::Ix, Active.id, 0, 0, Stmt.Label));
  raise_ix(Active);
  return 0;
}

uint64_t Database::write(const Insert& Stmt) {
  Relation& Active = Tbl(Stmt.Tbl);
  const size_t col_tally = Active.columns.size();
  std::vector<size_t> positions(col_tally, missing);
  if (Stmt.columns.empty()) {
    std::iota(positions.begin(), positions.end(), size_t{0});
  } else {
    if (Stmt.columns.size() != col_tally) throw SqlError("VALUE", "all columns are required");
    for (size_t i = 0; i < col_tally; ++i) {
      auto& Posn = positions[Active.Col(Stmt.columns[i])];
      if (Posn != missing) throw SqlError("SCHEMA", "duplicate insert column");
      Posn = i;
    }
  }

  invalidate_ix(Active);
  std::string Octets, packed;
  for (const auto& Rec : Stmt.Recs) {
    if (Rec.size() != col_tally) throw SqlError("VALUE", "incorrect number of values");
    const int64_t Ky = bigint(Rec[positions[Active.primary]]);
    if (Active.Recs.contains(Ky)) throw SqlError("duplicate_key", "primary key already exists");
    Octets.clear();
    for (size_t Col : Active.layout) {
      encode(Octets, Active.columns[Col], Rec[positions[Col]]);
    }
    apply(attach_rec(journal, Active, Ky, 0, Octets, packed));
  }
  return Stmt.Recs.size();
}

uint64_t Database::write(const Update& Stmt) {
  Relation& Active = Tbl(Stmt.Tbl);
  auto Preds = bind_preds(Active, Stmt.Pred);
  const KeyRange range = restrict_keys(Active, Preds);
  std::vector<std::optional<std::string>> replacements(Active.columns.size());
  for (const auto& [Label, Val] : Stmt.assignments) {
    const size_t Col = Active.Col(Label);
    if (Col == Active.primary) throw SqlError("unsupported_sql", "cannot update primary key");
    if (replacements[Col]) throw SqlError("SCHEMA", "duplicate SET column");
    encode(replacements[Col].emplace(), Active.columns[Col], Val);
  }

  invalidate_ix(Active);
  journal.prepare_reads();
  const auto [first, Final] = range.iterators(Active.Recs);
  uint64_t affected = 0;
  std::string Octets, packed, original;
  VectorBuffer Arena;
  std::vector<Cell> cells(Active.columns.size());
  for (auto Rec = first; Rec != Final; ++Rec) {
    if (!Preds.empty()) {
      decode(journal, Active, Rec->first, Rec->second, cells, Arena);
      if (!matches(Preds, cells)) continue;
    }
    auto old = payload(journal, Rec->second);
    if (Rec->second.dense_octets) {
      const auto vector = pull_vector(journal, Active, Rec->second, Arena);
      original.assign(vector);
      original.append(scalar_payload(journal, Active, Rec->second));
      old = original;
    }
    Octets.clear();
    Reader Fetcher(old);
    for (size_t Col : Active.layout) {
      const auto before = old.substr(old.size() - Fetcher.remaining());
      pull_cell(Fetcher, Active.columns[Col]);
      if (replacements[Col]) {
        Octets += *replacements[Col];
      } else {
        Octets += before.substr(0, before.size() - Fetcher.remaining());
      }
    }
    ++affected;
    if (Octets != old) {
      apply(attach_rec(journal, Active, Rec->first, Rec->second.Offs, Octets, packed));
    }
  }
  return affected;
}

uint64_t Database::write(const Delete& Stmt) {
  Relation& Active = Tbl(Stmt.Tbl);
  auto Preds = bind_preds(Active, Stmt.Pred);
  const KeyRange range = restrict_keys(Active, Preds);
  invalidate_ix(Active);
  journal.prepare_reads();
  uint64_t affected = 0;
  std::vector<Cell> cells(Active.columns.size());
  VectorBuffer Arena;
  auto Rec = Active.Recs.lower_bound(range.Lo);
  while (Rec != Active.Recs.end() && range.contains(Rec->first)) {
    const auto [Ky, location] = *Rec;
    if (!Preds.empty()) {
      decode(journal, Active, Ky, location, cells, Arena);
      if (!matches(Preds, cells)) {
        ++Rec;
        continue;
      }
    }
    apply(journal.append(Op::erase, Active.id, Ky, location.Offs));
    ++affected;
    // Since erasing may shift entries or Unify pages, resumption goes by Ky.
    Rec = Active.Recs.upper_bound(Ky);
  }
  return affected;
}

void Database::transact(Transaction operation) {
  journal.release_reads();
  if (operation == Transaction::begin) {
    if (transaction_) throw SqlError("TRANSACTION", "transaction already active");
    transaction_ = journal.end();
    return;
  }
  if (!transaction_) throw SqlError("TRANSACTION", "no active transaction");
  if (operation == Transaction::Revert) {
    close();
  } else {
    if (*transaction_ != journal.end()) journal.Finalize();
    transaction_.reset();
  }
}

void Database::close() {
  if (!transaction_) return;
  undo(*transaction_, journal.end());
  journal.discard_after(*transaction_);
  transaction_.reset();
}

void Database::execute(const Statement& Stmt, SearchMode mode, ResultSink& sink) {
  std::visit([&](const auto& command) {
    using Command = std::decay_t<decltype(command)>;
    if constexpr (std::is_same_v<Command, Select>) {
      Project(command, mode, sink);
    } else if constexpr (std::is_same_v<Command, Transaction>) {
      transact(command);
      sink.ok(0);
    } else {
      journal.release_reads();
      const uint64_t mark = journal.end();
      uint64_t affected;
      try {
        affected = write(command);
      } catch (const SqlError&) {
        undo(mark, journal.end());
        journal.discard_after(mark);
        throw;
      }
      if (!transaction_ && mark != journal.end()) journal.Finalize();
      sink.ok(affected);
    }
  }, Stmt);
}

namespace {

struct BoundExpression {
  size_t Col;
  size_t Lookup;
};

struct OrderKey {
  BoundExpression Expr;
  bool descending;
};

struct Selection {
  int64_t Ky;
  RowRef Rec;
};

struct Neighbor {
  Selection selection;
  double squared;
};

struct TextPosition {
  uint64_t Offs;
  uint32_t length;
};

using SortValue = std::variant<int64_t, TextPosition, double>;
struct OrderedRow {
  Selection selection;
  std::vector<SortValue> Vals;
};

template <typename Entry, typename Less>
void collect(std::vector<Entry>& Recs, Entry Item, size_t Cap, bool bounded, Less less) {
  if (!bounded) {
    Recs.push_back(std::move(Item));
  } else if (Recs.size() < Cap) {
    Recs.push_back(std::move(Item));
    std::push_heap(Recs.begin(), Recs.end(), less);
  } else if (less(Item, Recs.front())) {
    std::pop_heap(Recs.begin(), Recs.end(), less);
    Recs.back() = std::move(Item);
    std::push_heap(Recs.begin(), Recs.end(), less);
  }
}

}  // namespace

void Database::Project(const Select& Stmt, SearchMode mode, ResultSink& sink) {
  Relation& Active = Tbl(Stmt.Tbl);
  auto Preds = bind_preds(Active, Stmt.Pred);
  const KeyRange range = restrict_keys(Active, Preds);
  std::ranges::sort(Preds, {}, &Filter::Col);

  std::vector<std::vector<float>> queries;
  std::string_view first_lookup_txt;
  const auto bind = [&](const Expression& Expr) {
    const size_t Col = Active.Col(Expr.Col);
    if (!Expr.Lookup) return BoundExpression{Col, missing};
    if (Col != Active.vector) throw SqlError("VALUE", "L2_DISTANCE requires a VECTOR column");
    const auto lookup_txt = txt_view(Expr.Lookup->Txt);
    if (!queries.empty() && lookup_txt == first_lookup_txt) return BoundExpression{Col, 0};
    auto Val = vector_val(*Expr.Lookup, Active.columns[Col].length);
    if (queries.empty()) first_lookup_txt = lookup_txt;
    const auto existing = std::find(queries.begin(), queries.end(), Val);
    const size_t Ix = static_cast<size_t>(existing - queries.begin());
    if (existing == queries.end()) queries.push_back(std::move(Val));
    return BoundExpression{Col, Ix};
  };

  std::vector<BoundExpression> projections;
  std::vector<std::string> names;
  if (Stmt.star) {
    for (size_t i = 0; i < Active.columns.size(); ++i) {
      projections.push_back({i, missing});
      names.push_back(Active.columns[i].Label);
    }
  } else {
    for (const auto& Expr : Stmt.projections) {
      projections.push_back(bind(Expr));
      names.push_back(!Expr.alias.empty() ? Expr.alias :
                      Expr.Lookup ? "l2_distance" : Expr.Col);
    }
  }
  std::vector<OrderKey> Seq;
  for (const auto& ordering : Stmt.Seq) {
    const auto& Expr = ordering.Ky;
    const auto alias = std::find_if(Stmt.projections.begin(), Stmt.projections.end(),
                                    [&](const Expression& projection) {
      return !Expr.Lookup && !projection.alias.empty() && projection.alias == Expr.Col;
    });
    const BoundExpression bound = alias == Stmt.projections.end()
        ? bind(Expr) : projections[alias - Stmt.projections.begin()];
    if (bound.Lookup == missing && bound.Col == Active.vector) {
      throw SqlError("unsupported_sql", "cannot order by a vector value");
    }
    Seq.push_back({bound, ordering.descending});
  }

  const size_t Cap = std::min<uint64_t>(Active.Recs.size(),
      Stmt.Cap.value_or(Active.Recs.size()));
  if (Cap == 0) {
    sink.enter_recs(names, 0);
    sink.end_recs();
    return;
  }

  journal.prepare_reads();
  const auto [first, Final] = range.iterators(Active.Recs);
  std::vector<Cell> decoded(Active.columns.size());
  std::vector<Cell> Produced(projections.size());
  VectorBuffer Arena;
  const bool project_columns = std::any_of(projections.begin(), projections.end(),
      [&](const BoundExpression& projection) {
        return projection.Lookup == missing && projection.Col != Active.primary;
      });

  const auto accepted = [&](int64_t Ky, RowRef Rec) {
    if (Preds.empty()) return true;
    decode(journal, Active, Ky, Rec, decoded, Arena);
    return matches(Preds, decoded);
  };
  const auto included = [&](int64_t Ky, RowRef Rec) {
    if (Preds.empty()) return true;
    if (Rec.dense_octets || Rec.length > wide_rec) return matches_wide_rec(journal, Active, Rec, Preds);
    return accepted(Ky, Rec);
  };
  const auto distance = [&](std::string_view Octets, size_t Lookup) {
    return squared_gap(Octets.substr(0, Active.vector_octets), queries[Lookup]);
  };
  const auto vector_payload = [&](RowRef Rec) {
    return pull_vector(journal, Active, Rec, Arena);
  };
  const auto emit = [&](Selection selection, size_t known_lookup = missing, double known_square = 0) {
    std::string_view Octets;
    decoded[Active.primary] = selection.Ky;
    if (project_columns) {
      decode(journal, Active, selection.Ky, selection.Rec, decoded, Arena);
      if (Active.vector != missing) Octets = std::get<VectorView>(decoded[Active.vector]).Octets;
    }
    for (size_t i = 0; i < projections.size(); ++i) {
      const auto& Expr = projections[i];
      if (Expr.Lookup == missing) {
        Produced[i] = decoded[Expr.Col];
      } else {
        if (Expr.Lookup != known_lookup && Octets.empty()) {
          Octets = vector_payload(selection.Rec);
        }
        const double square = Expr.Lookup == known_lookup
            ? known_square : distance(Octets, Expr.Lookup);
        Produced[i] = std::sqrt(square);
      }
    }
    sink.Rec(Produced);
  };

  const bool primary_seq = !Seq.empty() && Seq.front().Expr.Col == Active.primary;
  if (Seq.empty() || primary_seq) {
    const auto traverse = [&](auto visit) {
      if (primary_seq && Seq.front().descending) {
        for (auto Rec = std::make_reverse_iterator(Final);
             Rec != std::make_reverse_iterator(first); ++Rec) {
          if (!visit(*Rec)) break;
        }
      } else {
        for (auto Rec = first; Rec != Final; ++Rec) {
          if (!visit(*Rec)) break;
        }
      }
    };
    size_t count = 0;
    if (Preds.empty() && first == Active.Recs.begin() && Final == Active.Recs.end()) {
      count = Cap;
    } else {
      traverse([&](const auto& Rec) {
        count += included(Rec.first, Rec.second);
        return count < Cap;
      });
    }
    sink.enter_recs(names, count);
    if (count != 0) {
      size_t sent = 0;
      traverse([&](const auto& Rec) {
        if (included(Rec.first, Rec.second)) {
          emit({Rec.first, Rec.second});
          ++sent;
        }
        return sent < count;
      });
    }
  } else if (Seq.size() == 1 && Seq.front().Expr.Lookup != missing) {
    const size_t Lookup = Seq.front().Expr.Lookup;
    const bool descending = Seq.front().descending;
    const double margin = gap_margin(queries[Lookup].size());
    const auto better = [&](const Neighbor& a, const Neighbor& b) {
      const int cmp = compare_gap(journal, Active, a.selection.Rec, a.squared, b.selection.Rec,
                                       b.squared, queries[Lookup], margin);
      if (cmp == 0) return a.selection.Ky < b.selection.Ky;
      return descending ? cmp > 0 : cmp < 0;
    };
    std::vector<Neighbor> nearest;
    const bool bounded = Stmt.Cap.has_value();
    if (bounded) nearest.reserve(std::min<size_t>(Cap, 1024));
#if defined(__x86_64__)
    const bool float_bound = bounded && !descending && queries[Lookup].size() >= 32 &&
                             queries[Lookup].size() <= 4096 && __builtin_cpu_supports("avx2");
    const bool prepared = float_bound && queries[Lookup].size() == 128 && __builtin_cpu_supports("fma");
    std::array<double, 128> prepared_lookup;
    if (prepared) std::copy(queries[Lookup].begin(), queries[Lookup].end(), prepared_lookup.begin());
#endif
    const auto probe = [&](int64_t Ky, RowRef Rec, bool pred_known = false,
                           bool quantized = false, bool dense = false) {
      if (pred_known || included(Ky, Rec)) {
        const auto Octets = vector_payload(Rec);
#if defined(__x86_64__)
        if (float_bound && (!prepared || !quantized || dense) && nearest.size() == Cap &&
            separated_squares(avx2_lower_square(Octets.data(), queries[Lookup].data(),
                                                queries[Lookup].size()),
                              nearest.front().squared, margin) > 0) return;
        if (prepared && quantized) {
          collect(nearest, Neighbor{{Ky, Rec}, prepared_l2_128(Octets.data(), prepared_lookup.data())}, Cap, bounded, better);
          return;
        }
#endif
        collect(nearest, Neighbor{{Ky, Rec}, distance(Octets, Lookup)}, Cap, bounded, better);
      }
    };
    if (mode == SearchMode::ann && bounded && !descending && !Active.ix_label.empty()) {
      raise_ix(Active);
    }
    bool used_bounds = false;
#if defined(__x86_64__)
    if (mode == SearchMode::exact && float_bound && Cap <= 64 && first != Final && !transaction_) {
      const Filter* equality = Preds.size() == 1 && Preds.front().comparison == Comparison::eq
          ? &Preds.front() : nullptr;
      prepare_bounds(Active, equality ? std::optional(equality->Col) : std::nullopt);
      BoundQuery packed;
      static const auto accelerated = dot_kernel();
      const auto kernel = Active.bounds && Active.bounds->stride == 128 ? accelerated : DotKernel::paired;
      if (Active.bounds && make_bound_lookup(*Active.bounds, queries[Lookup], kernel != DotKernel::paired, packed)) {
        auto& Stash = *Active.bounds;
        const bool grouped = equality && Stash.Clusters && Stash.Clusters->Col == equality->Col;
        // BIGINT tags keep every bit intact; text fingerprints still need the original predicate.
        const bool pred_known = grouped && std::holds_alternative<int64_t>(equality->Val);
        double previous_worst = -1;
        const auto visit = [&](const BoundRow& Item, int32_t cutoff, bool dense) {
          probe(Item.Ky(), Item.location(), pred_known, true, dense);
          if (nearest.size() == Cap && nearest.front().squared != previous_worst) {
            previous_worst = nearest.front().squared;
            return bound_cutoff(previous_worst, margin, Stash, packed);
          }
          return cutoff;
        };
        const auto* bucket = grouped ? Stash.Clusters->lookup(pred_tag(equality->Val)) : nullptr;
        const bool sweep_range_by_group = Stash.Clusters && !range.unrestricted() && Preds.empty() &&
            Stash.Clusters->buckets.size() <= geometric_buckets &&
            Active.Recs.range_at_least(first, Final, 512);
        if (grouped || !Stash.Clusters || range.unrestricted() || sweep_range_by_group) {
          used_bounds = true;
          if (!grouped || bucket) {
            const size_t seed = grouped || Preds.empty() ? Cap : 0;
            // With geometric buckets, the Sweep already narrows to the closest Clusters.
            const bool defer = (pred_known || Preds.empty()) &&
                (!Stash.Clusters || !Stash.Clusters->compact_geometry);
            const auto Sweep = [&](const BoundGroups::Bucket* target, int32_t cutoff) {
              if (kernel == DotKernel::vex) return sweep_bounds<128, DotKernel::vex>(Stash, packed, range, target, visit, cutoff, seed, defer);
              if (kernel == DotKernel::evex) return sweep_bounds<128, DotKernel::evex>(Stash, packed, range, target, visit, cutoff, seed, defer);
              if (Stash.stride == 128) return sweep_bounds<128>(Stash, packed, range, target, visit, cutoff, seed, defer);
              return sweep_bounds<0>(Stash, packed, range, target, visit, cutoff, 0, false);
            };
            if (Preds.empty() && Stash.Clusters && Stash.Clusters->compact_geometry) {
              sweep_geometric_buckets(Stash, packed, kernel != DotKernel::paired, Sweep);
            } else if (sweep_range_by_group) {
              auto cutoff = std::numeric_limits<int32_t>::max();
              for (const auto& group : Stash.Clusters->buckets) cutoff = Sweep(&group, cutoff);
            } else {
              Sweep(bucket, std::numeric_limits<int32_t>::max());
            }
          }
        }
      }
    }
#endif
    if (!used_bounds && mode == SearchMode::ann && bounded && !descending && !Active.Ix.empty()) {
      // Projecting on the first coordinate gives a lower bound for the full L2 distance.
      // Expanding outward in order of that bound, ties are visited before any stop, so
      // indexed and exact searches agree on primary-Ky order.
      const double pivot = queries[Lookup].front();
      const auto Cleave = std::lower_bound(Active.Ix.begin(), Active.Ix.end(), pivot,
          [](const ProjectionEntry& Item, double Val) { return Item.pivot < Val; });
      size_t Lhs = Cleave - Active.Ix.begin();
      size_t Rhs = Lhs;
      while (Lhs || Rhs < Active.Ix.size()) {
        const double lhs_delta = Lhs ? pivot - Active.Ix[Lhs - 1].pivot
                                       : std::numeric_limits<double>::infinity();
        const double rhs_delta = Rhs < Active.Ix.size() ? Active.Ix[Rhs].pivot - pivot
                                                               : std::numeric_limits<double>::infinity();
        const double delta = std::min(lhs_delta, rhs_delta);
        if (nearest.size() == Cap &&
            separated_squares(delta * delta, nearest.front().squared, margin) > 0) {
          break;
        }
        const auto& Item = lhs_delta < rhs_delta ? Active.Ix[--Lhs] : Active.Ix[Rhs++];
        const auto& [Ky, Rec] = *Item.Item;
        if (range.contains(Ky)) probe(Ky, Rec);
      }
    } else if (!used_bounds) {
      for (auto Rec = first; Rec != Final; ++Rec) probe(Rec->first, Rec->second);
    }
    std::sort(nearest.begin(), nearest.end(), better);
    sink.enter_recs(names, nearest.size());
    for (const auto& Rec : nearest) emit(Rec.selection, Lookup, Rec.squared);
  } else {
    const auto better = [&](const OrderedRow& a, const OrderedRow& b) {
      for (size_t i = 0; i < Seq.size(); ++i) {
        const auto Lookup = Seq[i].Expr.Lookup;
        int cmp;
        if (Lookup != missing) {
          cmp = compare_gap(journal, Active, a.selection.Rec, std::get<double>(a.Vals[i]),
                                 b.selection.Rec, std::get<double>(b.Vals[i]),
                                 queries[Lookup], gap_margin(queries[Lookup].size()));
        } else if (const auto* number = std::get_if<int64_t>(&a.Vals[i])) {
          cmp = compare(*number, std::get<int64_t>(b.Vals[i]));
        } else {
          const auto Lhs = std::get<TextPosition>(a.Vals[i]);
          const auto Rhs = std::get<TextPosition>(b.Vals[i]);
          cmp = journal.compare_octets(Lhs.Offs, Lhs.length, Rhs.Offs, Rhs.length);
        }
        if (cmp != 0) return Seq[i].descending ? cmp > 0 : cmp < 0;
      }
      return a.selection.Ky < b.selection.Ky;
    };
    std::vector<OrderedRow> sorted;
    for (auto Rec = first; Rec != Final; ++Rec) {
      decode(journal, Active, Rec->first, Rec->second, decoded, Arena);
      if (!matches(Preds, decoded)) continue;
      OrderedRow Item{{Rec->first, Rec->second}, {}};
      Item.Vals.reserve(Seq.size());
      for (const auto& Ky : Seq) {
        const auto& Expr = Ky.Expr;
        if (Expr.Lookup != missing) {
          Item.Vals.emplace_back(distance(std::get<VectorView>(decoded[Active.vector]).Octets, Expr.Lookup));
        } else if (const auto* number = std::get_if<int64_t>(&decoded[Expr.Col])) {
          Item.Vals.emplace_back(*number);
        } else {
          const auto Txt = std::get<std::string_view>(decoded[Expr.Col]);
          const auto Octets = payload(journal, Rec->second);
          const auto relative = static_cast<uint64_t>(Txt.data() - Octets.data());
          Item.Vals.emplace_back(TextPosition{Rec->second.payload_offs() + relative,
                                                 static_cast<uint32_t>(Txt.size())});
        }
      }
      collect(sorted, std::move(Item), Cap, Stmt.Cap.has_value(), better);
    }
    std::sort(sorted.begin(), sorted.end(), better);
    sink.enter_recs(names, sorted.size());
    for (const auto& Rec : sorted) emit(Rec.selection);
  }
  sink.end_recs();
}

}  // namespace mini
