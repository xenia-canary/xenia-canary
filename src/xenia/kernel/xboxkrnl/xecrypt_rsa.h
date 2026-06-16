#ifndef XECRYPT_RSA_H_
#define XECRYPT_RSA_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace xecrypt {
namespace bignum {

// Portable 128-bit unsigned helpers using only 64-bit arithmetic
struct u128 {
  uint64_t lo;
  uint64_t hi;
};

static inline u128 u128_from(uint64_t v) { return {v, 0}; }

static inline u128 u128_add(u128 a, uint64_t b) {
  u128 r;
  r.lo = a.lo + b;
  r.hi = a.hi + (r.lo < a.lo ? 1 : 0);
  return r;
}

static inline u128 u128_sub(u128 a, uint64_t b) {
  u128 r;
  r.hi = a.hi - (a.lo < b ? 1 : 0);
  r.lo = a.lo - b;
  return r;
}

static inline u128 u128_shl(u128 v, int s) {
  if (s == 0) {
    return v;
  }
  if (s >= 64) {
    return {0, v.lo << (s - 64)};
  }
  return {v.lo << s, (v.hi << s) | (v.lo >> (64 - s))};
}

static inline u128 u128_shr(u128 v, int s) {
  if (s == 0) {
    return v;
  }
  if (s >= 64) {
    return {v.hi >> (s - 64), 0};
  }
  return {(v.lo >> s) | (v.hi << (64 - s)), v.hi >> s};
}

static inline u128 u128_or64(u128 a, uint64_t b) { return {a.lo | b, a.hi}; }

// Portable 64x64 -> 128 multiply
static inline u128 u128_mul64(uint64_t a, uint64_t b) {
  uint64_t a_lo = a & 0xFFFFFFFF;
  uint64_t a_hi = a >> 32;
  uint64_t b_lo = b & 0xFFFFFFFF;
  uint64_t b_hi = b >> 32;

  uint64_t p0 = a_lo * b_lo;
  uint64_t p1 = a_lo * b_hi;
  uint64_t p2 = a_hi * b_lo;
  uint64_t p3 = a_hi * b_hi;

  uint64_t mid = p1 + (p0 >> 32);
  uint64_t mid_carry = 0;
  uint64_t mid2 = mid + p2;
  if (mid < p1) {
    mid_carry++;
  }
  if (mid2 < mid) {
    mid_carry++;
  }

  u128 r;
  r.lo = (mid2 << 32) | (p0 & 0xFFFFFFFF);
  r.hi = p3 + (mid2 >> 32) + (mid_carry << 32);
  return r;
}

// 128 / 64 -> quotient and remainder
static inline uint64_t u128_div64(u128 num, uint64_t den, uint64_t* rem) {
  if (num.hi == 0) {
    *rem = num.lo % den;
    return num.lo / den;
  }

  uint64_t q = 0;
  uint64_t r = 0;
  for (int i = 127; i >= 0; i--) {
    r = (r << 1);
    if (i >= 64) {
      r |= (num.hi >> (i - 64)) & 1;
    } else {
      r |= (num.lo >> i) & 1;
    }
    if (r >= den) {
      r -= den;
      if (i < 64) {
        q |= (1ULL << i);
      }
    }
  }
  *rem = r;
  return q;
}

// Count leading zeros - portable
static inline int clz64(uint64_t v) {
  if (v == 0) {
    return 64;
  }
  int n = 0;
  if ((v & 0xFFFFFFFF00000000ULL) == 0) {
    n += 32;
    v <<= 32;
  }
  if ((v & 0xFFFF000000000000ULL) == 0) {
    n += 16;
    v <<= 16;
  }
  if ((v & 0xFF00000000000000ULL) == 0) {
    n += 8;
    v <<= 8;
  }
  if ((v & 0xF000000000000000ULL) == 0) {
    n += 4;
    v <<= 4;
  }
  if ((v & 0xC000000000000000ULL) == 0) {
    n += 2;
    v <<= 2;
  }
  if ((v & 0x8000000000000000ULL) == 0) {
    n += 1;
  }
  return n;
}

static inline int clz32(uint32_t v) {
  if (v == 0) {
    return 32;
  }
  int n = 0;
  if ((v & 0xFFFF0000U) == 0) {
    n += 16;
    v <<= 16;
  }
  if ((v & 0xFF000000U) == 0) {
    n += 8;
    v <<= 8;
  }
  if ((v & 0xF0000000U) == 0) {
    n += 4;
    v <<= 4;
  }
  if ((v & 0xC0000000U) == 0) {
    n += 2;
    v <<= 2;
  }
  if ((v & 0x80000000U) == 0) {
    n += 1;
  }
  return n;
}

class BigNum {
 public:
  std::vector<uint64_t> limbs;

  BigNum() = default;

  void trim() {
    while (limbs.size() > 1 && limbs.back() == 0) {
      limbs.pop_back();
    }
  }

  static BigNum from_bytes_be(const uint8_t* data, size_t len) {
    BigNum r;
    size_t n = (len + 7) / 8;
    r.limbs.resize(n, 0);
    for (size_t i = 0; i < len; i++) {
      size_t byte_pos = len - 1 - i;
      r.limbs[byte_pos / 8] |= static_cast<uint64_t>(data[i])
                               << (8 * (byte_pos % 8));
    }
    r.trim();
    return r;
  }

  void to_bytes_be(uint8_t* out, size_t len) const {
    std::memset(out, 0, len);
    for (size_t i = 0; i < len; i++) {
      size_t byte_pos = len - 1 - i;
      size_t li = byte_pos / 8;
      if (li < limbs.size()) {
        out[i] = static_cast<uint8_t>(limbs[li] >> (8 * (byte_pos % 8)));
      }
    }
  }

  static int compare(const BigNum& a, const BigNum& b) {
    size_t an = a.limbs.size(), bn = b.limbs.size();
    size_t n = std::max(an, bn);
    for (size_t i = n; i > 0; i--) {
      uint64_t al = (i - 1 < an) ? a.limbs[i - 1] : 0;
      uint64_t bl = (i - 1 < bn) ? b.limbs[i - 1] : 0;
      if (al < bl) {
        return -1;
      }
      if (al > bl) {
        return 1;
      }
    }
    return 0;
  }

  static BigNum sub(const BigNum& a, const BigNum& b) {
    BigNum r;
    size_t n = a.limbs.size();
    r.limbs.resize(n, 0);
    uint64_t borrow = 0;
    for (size_t i = 0; i < n; i++) {
      uint64_t bl = (i < b.limbs.size()) ? b.limbs[i] : 0;
      u128 diff = u128_sub(u128_sub(u128_from(a.limbs[i]), bl), borrow);
      r.limbs[i] = diff.lo;
      borrow = (diff.hi >> 63) ? 1 : 0;
    }
    r.trim();
    return r;
  }

  static BigNum mul(const BigNum& a, const BigNum& b) {
    size_t an = a.limbs.size(), bn = b.limbs.size();
    BigNum r;
    r.limbs.resize(an + bn, 0);
    for (size_t i = 0; i < an; i++) {
      uint64_t carry = 0;
      for (size_t j = 0; j < bn; j++) {
        u128 prod = u128_mul64(a.limbs[i], b.limbs[j]);
        prod = u128_add(prod, r.limbs[i + j]);
        prod = u128_add(prod, carry);
        r.limbs[i + j] = prod.lo;
        carry = prod.hi;
      }
      r.limbs[i + bn] += carry;
    }
    r.trim();
    return r;
  }

  static BigNum mod(const BigNum& a, const BigNum& m) {
    if (compare(a, m) < 0) {
      return a;
    }

    size_t n = m.limbs.size();
    size_t total = a.limbs.size();

    if (n == 0 || (n == 1 && m.limbs[0] == 0)) {
      return BigNum();
    }

    if (n == 1) {
      uint64_t d = m.limbs[0];
      uint64_t rem = 0;
      for (size_t i = total; i > 0; i--) {
        u128 cur = u128_or64(u128_shl(u128_from(rem), 64), a.limbs[i - 1]);
        u128_div64(cur, d, &rem);
      }
      BigNum r;
      r.limbs = {rem};
      r.trim();
      return r;
    }

    int shift = 0;
    uint64_t top = m.limbs[n - 1];
    if (top != 0) {
      shift = clz64(top);
    }

    BigNum u, v;
    u.limbs.resize(total + 1, 0);
    if (shift > 0) {
      uint64_t carry = 0;
      for (size_t i = 0; i < total; i++) {
        u128 val = u128_or64(u128_shl(u128_from(a.limbs[i]), shift), carry);
        u.limbs[i] = val.lo;
        carry = val.hi;
      }
      u.limbs[total] = carry;
    } else {
      for (size_t i = 0; i < total; i++) {
        u.limbs[i] = a.limbs[i];
      }
      u.limbs[total] = 0;
    }

    v.limbs.resize(n, 0);
    if (shift > 0) {
      uint64_t carry = 0;
      for (size_t i = 0; i < n; i++) {
        u128 val = u128_or64(u128_shl(u128_from(m.limbs[i]), shift), carry);
        v.limbs[i] = val.lo;
        carry = val.hi;
      }
    } else {
      v.limbs = m.limbs;
    }

    uint64_t vn_1 = v.limbs[n - 1];
    uint64_t vn_2 = (n >= 2) ? v.limbs[n - 2] : 0;

    for (size_t j = total; j >= n; j--) {
      u128 num_top =
          u128_or64(u128_shl(u128_from(u.limbs[j]), 64), u.limbs[j - 1]);
      uint64_t rhat_val;
      uint64_t qhat_val = u128_div64(num_top, vn_1, &rhat_val);

      while (true) {
        u128 qv2 = u128_mul64(qhat_val, vn_2);
        u128 rhs = u128_or64(u128_shl(u128_from(rhat_val), 64), u.limbs[j - 2]);
        bool gt = (qv2.hi > rhs.hi) || (qv2.hi == rhs.hi && qv2.lo > rhs.lo);
        if (!gt) {
          break;
        }
        qhat_val--;
        uint64_t old_rhat = rhat_val;
        rhat_val += vn_1;
        if (rhat_val < old_rhat) {
          break;
        }
      }

      uint64_t carry = 0;
      for (size_t i = 0; i < n; i++) {
        u128 prod = u128_mul64(qhat_val, v.limbs[i]);
        prod = u128_add(prod, carry);
        uint64_t prod_lo = prod.lo;
        carry = prod.hi;
        uint64_t u_val = u.limbs[j - n + i];
        u.limbs[j - n + i] = u_val - prod_lo;
        if (u_val < prod_lo) {
          carry++;
        }
      }
      int64_t final_diff =
          static_cast<int64_t>(u.limbs[j]) - static_cast<int64_t>(carry);
      u.limbs[j] = static_cast<uint64_t>(final_diff);

      if (final_diff < 0) {
        uint64_t c = 0;
        for (size_t i = 0; i < n; i++) {
          u128 sum = u128_add(u128_from(u.limbs[j - n + i]), v.limbs[i]);
          sum = u128_add(sum, c);
          u.limbs[j - n + i] = sum.lo;
          c = sum.hi;
        }
        u.limbs[j] += c;
      }
    }

    BigNum r;
    r.limbs.resize(n, 0);
    if (shift > 0) {
      uint64_t carry = 0;
      for (size_t i = n; i > 0; i--) {
        u128 val = u128_or64(u128_shl(u128_from(carry), 64), u.limbs[i - 1]);
        r.limbs[i - 1] = u128_shr(val, shift).lo;
        carry = u.limbs[i - 1] & ((1ULL << shift) - 1);
      }
    } else {
      for (size_t i = 0; i < n; i++) {
        r.limbs[i] = u.limbs[i];
      }
    }

    r.trim();
    return r;
  }

  static BigNum modexp(const BigNum& base, uint32_t exp,
                       const BigNum& mod_val) {
    BigNum result;
    result.limbs = {1};

    if (exp == 0) {
      return mod(result, mod_val);
    }

    int highest_bit = 31 - clz32(exp);
    BigNum b = mod(base, mod_val);

    for (int i = highest_bit; i >= 0; i--) {
      result = mod(mul(result, result), mod_val);
      if ((exp >> i) & 1) {
        result = mod(mul(result, b), mod_val);
      }
    }

    return result;
  }
};

}  // namespace bignum
}  // namespace xecrypt

// Performs RSA public-key encryption/decryption matching
// XeCryptBnQwNeRsaPubCrypt. All buffers use the Xbox 360 layout: big-endian
// uint64 limbs in little-endian limb order.
//
// qw_a:    input data (num_qwords * 8 bytes)
// qw_b:    output buffer (num_qwords * 8 bytes)
// modulus: RSA modulus following the XECRYPT_RSA header (num_qwords * 8 bytes)
// num_qwords: number of 64-bit limbs
// exponent:   public exponent
//
// Returns 1 on success, 0 on failure.
inline uint32_t XeCryptBnQwNeRsaPubCrypt(const uint8_t* qw_a, uint8_t* qw_b,
                                         const uint8_t* modulus,
                                         uint32_t num_qwords,
                                         uint32_t exponent) {
  // Reject keys below 512 bits (8 qwords) to match original BCrypt behavior.
  if (num_qwords < 8) {
    return 0;
  }

  uint32_t modulus_size = num_qwords * 8;

  auto input_be = std::vector<uint8_t>(modulus_size);
  auto mod_be = std::vector<uint8_t>(modulus_size);

  // Reverse qword order to produce big-endian byte arrays
  for (uint32_t i = 0; i < num_qwords; i++) {
    std::memcpy(&input_be[i * 8], &qw_a[(num_qwords - 1 - i) * 8], 8);
    std::memcpy(&mod_be[i * 8], &modulus[(num_qwords - 1 - i) * 8], 8);
  }

  auto base =
      xecrypt::bignum::BigNum::from_bytes_be(input_be.data(), modulus_size);
  auto mod =
      xecrypt::bignum::BigNum::from_bytes_be(mod_be.data(), modulus_size);

  auto result = xecrypt::bignum::BigNum::modexp(base, exponent, mod);

  auto result_be = std::vector<uint8_t>(modulus_size);
  result.to_bytes_be(result_be.data(), modulus_size);

  // Convert back to Xbox format: reverse qword order
  for (uint32_t i = 0; i < num_qwords; i++) {
    std::memcpy(&qw_b[i * 8], &result_be[(num_qwords - 1 - i) * 8], 8);
  }

  return 1;
}

#endif  // XECRYPT_RSA_H_
