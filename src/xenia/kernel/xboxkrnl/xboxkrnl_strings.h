/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_STRINGS_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_STRINGS_H_

#include "xenia/kernel/util/shim_utils.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

enum FormatState {
  FS_Invalid = 0,
  FS_Unknown,
  FS_Start,
  FS_Flags,
  FS_Width,
  FS_PrecisionStart,
  FS_Precision,
  FS_Size,
  FS_Type,
  FS_End,
};

enum FormatFlags {
  FF_LeftJustify = 1 << 0,
  FF_AddLeadingZeros = 1 << 1,
  FF_AddPositive = 1 << 2,
  FF_AddPositiveAsSpace = 1 << 3,
  FF_AddNegative = 1 << 4,
  FF_AddPrefix = 1 << 5,
  FF_IsShort = 1 << 6,
  FF_IsLong = 1 << 7,
  FF_IsLongLong = 1 << 8,
  FF_IsWide = 1 << 9,
  FF_IsSigned = 1 << 10,
  FF_ForceLeadingZero = 1 << 11,
  FF_InvertWide = 1 << 12,
};

enum ArgumentSize {
  AS_Default = 0,
  AS_Short,
  AS_Long,
  AS_LongLong,
};

class FormatData {
 public:
  virtual uint16_t get() = 0;
  virtual uint16_t peek(int32_t offset) = 0;
  virtual void skip(int32_t count) = 0;
  virtual bool put(uint16_t c) = 0;
};

class ArgList {
 public:
  virtual uint32_t get32() = 0;
  virtual uint64_t get64() = 0;
};

// Making the assumption that the Xbox 360's implementation of the
// printf-functions matches what is described on MSDN's documentation for the
// Windows CRT:
//
// "Format Specification Syntax: printf and wprintf Functions"
// https://msdn.microsoft.com/en-us/library/56e442dc.aspx

std::string format_double(double value, int32_t precision, uint16_t c,
                          uint32_t flags);

int32_t format_core(PPCContext* ppc_context, FormatData& data, ArgList& args,
                    const bool wide);

class StackArgList : public ArgList {
 public:
  StackArgList(PPCContext* ppc_context, int32_t index)
      : ppc_context(ppc_context), index_(index) {}

  uint32_t get32() { return (uint32_t)get64(); }

  uint64_t get64() { return util::get_arg_64(ppc_context, index_++); }

 private:
  PPCContext* ppc_context;
  int32_t index_;
};

class ArrayArgList : public ArgList {
 public:
  ArrayArgList(PPCContext* ppc_context, uint32_t arg_ptr)
      : ppc_context(ppc_context), arg_ptr_(arg_ptr), index_(0) {}

  uint32_t get32() { return (uint32_t)get64(); }

  uint64_t get64() {
    return xe::load_and_swap<uint64_t>(
        ppc_context->TranslateVirtual(arg_ptr_ + (8 * index_++)));
  }

 private:
  PPCContext* ppc_context;
  uint32_t arg_ptr_;
  int32_t index_;
};

class StringFormatData : public FormatData {
 public:
  StringFormatData(const uint8_t* input) : input_(input) {}

  uint16_t get() {
    uint16_t result = *input_;
    if (result) {
      input_++;
    }
    return result;
  }

  uint16_t peek(int32_t offset) { return input_[offset]; }

  void skip(int32_t count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  bool put(uint16_t c) {
    if (c >= 0x100) {
      return false;
    }
    output_.push_back(char(c));
    return true;
  }

  const std::string& str() const { return output_; }

 private:
  const uint8_t* input_;
  std::string output_;
};

class WideStringFormatData : public FormatData {
 public:
  WideStringFormatData(const uint16_t* input) : input_(input) {}

  uint16_t get() {
    uint16_t result = *input_;
    if (result) {
      input_++;
    }
    return xe::byte_swap(result);
  }

  uint16_t peek(int32_t offset) { return xe::byte_swap(input_[offset]); }

  void skip(int32_t count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  bool put(uint16_t c) {
    output_.push_back(char16_t(c));
    return true;
  }

  const std::u16string& wstr() const { return output_; }

 private:
  const uint16_t* input_;
  std::u16string output_;
};

class WideCountFormatData : public FormatData {
 public:
  WideCountFormatData(const uint16_t* input) : input_(input), count_(0) {}

  uint16_t get() {
    uint16_t result = *input_;
    if (result) {
      input_++;
    }
    return xe::byte_swap(result);
  }

  uint16_t peek(int32_t offset) { return xe::byte_swap(input_[offset]); }

  void skip(int32_t count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  bool put(uint16_t c) {
    ++count_;
    return true;
  }

  const int32_t count() const { return count_; }

 private:
  const uint16_t* input_;
  int32_t count_;
};

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif
