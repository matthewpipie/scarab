/* Copyright 2024 Litz Lab
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : packed_pin_inst.h
 * Description  : Exact, sparse storage for a ctype_pin_inst that is kept per static PC.
 *
 * A ctype_pin_inst is 1071 B, most of it worst-case arrays: 24 + 8 register slots with
 * values (never set by the trace frontends), 8 load and 8 store addresses and 8 load values.
 * Tables that keep one per PC (over a million PCs in a Google sim) therefore hold ~1 KB of
 * mostly-constant bytes per entry. A Packed_Pin_Inst keeps only the 8-byte words that differ
 * from a fixed template image, so unpack() reproduces the packed struct byte for byte.
 ***************************************************************************************/

#ifndef __PACKED_PIN_INST_H__
#define __PACKED_PIN_INST_H__

#include <cstdint>
#include <cstring>
#include <memory>

#include "ctype_pin_inst.h"

class Packed_Pin_Inst {
 public:
  static constexpr size_t kBytes = sizeof(ctype_pin_inst);
  static constexpr size_t kWords = (kBytes + 7) / 8;
  static constexpr size_t kMaskWords = (kWords + 63) / 64;

  // A template image: kWords words whose first kBytes bytes are a ctype_pin_inst (the rest zero).
  struct Template {
    uint64_t w[kWords] = {};
    void set(const ctype_pin_inst& inst) {
      w[kWords - 1] = 0;
      memcpy(w, &inst, kBytes);
    }
  };

  // Store `in`, except that bytes [overlay_lo, overlay_hi) are taken from the template instead.
  void pack(const ctype_pin_inst& in, const Template& tmpl, size_t overlay_lo = 0, size_t overlay_hi = 0) {
    uint64_t buf[kWords];
    buf[kWords - 1] = 0;
    memcpy(buf, &in, kBytes);
    if (overlay_hi > overlay_lo)
      memcpy(reinterpret_cast<char*>(buf) + overlay_lo, reinterpret_cast<const char*>(tmpl.w) + overlay_lo,
             overlay_hi - overlay_lo);
    uint64_t mask[kMaskWords] = {};
    uint64_t diff[kWords];
    size_t n = 0;
    for (size_t i = 0; i < kWords; i++) {
      if (buf[i] != tmpl.w[i]) {
        mask[i / 64] |= uint64_t{1} << (i % 64);
        diff[n++] = buf[i];
      }
    }
    if (n != count_)
      words_.reset(n ? new uint64_t[n] : nullptr);
    count_ = n;
    memcpy(mask_, mask, sizeof mask_);
    if (n)
      memcpy(words_.get(), diff, n * sizeof(uint64_t));
  }

  void unpack(ctype_pin_inst* out, const Template& tmpl) const {
    char* dst = reinterpret_cast<char*>(out);
    memcpy(dst, tmpl.w, kBytes);
    size_t k = 0;
    for (size_t m = 0; m < kMaskWords; m++) {
      for (uint64_t bits = mask_[m]; bits; bits &= bits - 1) {
        size_t i = m * 64 + __builtin_ctzll(bits);
        size_t off = i * 8;
        memcpy(dst + off, &words_[k++], off + 8 <= kBytes ? 8 : kBytes - off);
      }
    }
  }

  // Bytes [off, off + len) of the packed struct.
  void read(size_t off, void* dst, size_t len, const Template& tmpl) const {
    char* d = static_cast<char*>(dst);
    while (len) {
      size_t i = off / 8, b = off % 8, n = len < 8 - b ? len : 8 - b;
      uint64_t word = word_at(i, tmpl);
      memcpy(d, reinterpret_cast<const char*>(&word) + b, n);
      d += n;
      off += n;
      len -= n;
    }
  }

  template <class T>
  T field(size_t off, const Template& tmpl) const {
    T v;
    read(off, &v, sizeof v, tmpl);
    return v;
  }

  size_t heap_words() const { return count_; }

 private:
  uint64_t word_at(size_t i, const Template& tmpl) const {
    uint64_t bit = uint64_t{1} << (i % 64);
    if (!(mask_[i / 64] & bit))
      return tmpl.w[i];
    size_t rank = __builtin_popcountll(mask_[i / 64] & (bit - 1));
    for (size_t m = 0; m < i / 64; m++)
      rank += __builtin_popcountll(mask_[m]);
    return words_[rank];
  }

  uint64_t mask_[kMaskWords] = {};
  size_t count_ = 0;
  std::unique_ptr<uint64_t[]> words_;
};

#endif /* #ifndef __PACKED_PIN_INST_H__ */
