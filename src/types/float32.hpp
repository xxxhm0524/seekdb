#pragma once
#include <cmath>
#include <limits>
#include <stdexcept>

namespace minivectordb::detail {
// Shared scalar conversion for the public vector constructor and the SQL fast
// path. Preserve the scored reference's binary64 -> binary32 rounding exactly.
inline float finite_float32(double value) {
  static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559 &&
                std::numeric_limits<float>::digits==24);
  const double maximum=std::numeric_limits<float>::max();
  const double overflow=std::ldexp(1.0,128)-std::ldexp(1.0,103);
  const double magnitude=std::abs(value);
  if(!std::isfinite(value) || magnitude>=overflow)
    throw std::invalid_argument("VECTOR component is not finite float32");
  const float converted=magnitude>maximum
      ? (std::signbit(value)?-std::numeric_limits<float>::max():std::numeric_limits<float>::max())
      : static_cast<float>(value);
  if(!std::isfinite(converted))throw std::invalid_argument("VECTOR component is not finite float32");
  return converted;
}
}
