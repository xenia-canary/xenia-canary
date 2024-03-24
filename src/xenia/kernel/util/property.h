/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_PROPERTY_H_
#define XENIA_KERNEL_UTIL_PROPERTY_H_

#include "xenia/base/byte_stream.h"
#include "xenia/kernel/util/xuserdata.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

struct XUSER_PROPERTY {
  xe::be<uint32_t> property_id;
  X_USER_DATA data;
};

struct XUSER_STATS_COLUMN {
  xe::be<uint16_t> column_ordinal;
  X_USER_DATA data;
};

class Property {
 public:
  Property(uint32_t property_id, uint32_t value_size, uint8_t* value_ptr);
  Property(const uint8_t* serialized_data, size_t data_size);
  ~Property();

  bool IsValid() const { return data_ != nullptr; }
  std::vector<uint8_t> Serialize();
  bool Deserialize();

  void Write(Memory* memory, XUSER_PROPERTY* property);
  uint32_t GetSize() const { return value_size_; }

  bool RequiresPointer() const {
    return static_cast<X_USER_DATA_TYPE>(property_id_.type) ==
               X_USER_DATA_TYPE::BINARY ||
           static_cast<X_USER_DATA_TYPE>(property_id_.type) ==
               X_USER_DATA_TYPE::CONTENT ||
           static_cast<X_USER_DATA_TYPE>(property_id_.type) ==
               X_USER_DATA_TYPE::WSTRING;
  }

 private:
  void CreateUserData(uint8_t* data_ptr);

  AttributeKey property_id_ = {};

  uint32_t value_size_ = 0;
  std::vector<uint8_t> value_;

  X_USER_DATA_TYPE data_type_ = X_USER_DATA_TYPE::UNSET;
  std::unique_ptr<UserData> data_ = nullptr;
};

class Filter {
 public:
  enum class ComparationOperator : uint16_t {
    Equals = 0,
    LeftGreater = 1,
    LeftGreaterEqual = 2,
    RightGreater = 3,
    RightGreaterEqual = 4,
  };

  enum class Type : uint32_t {
    Attribute = 0,
    Parameter = 1,
  };

  Filter(uint32_t left_side_id, uint32_t right_side_id, uint32_t op);

 private:
  uint32_t left_side_id_;
  Type left_side_type_;

  uint32_t right_side_id_;
  Type right_side_type_;

  ComparationOperator op_;
};

/*
class Context {
 public:
  Context(uint32_t id);
  ~Context();

  private:
  AttributeKey property_id_ = {};

  uint32_t stringId = 0;
  std::string friendly_name = "";
  uint32_t default_value = 0;
  uint32_t max_value = 0;
};*/

}  // namespace kernel
}  // namespace xe

#endif