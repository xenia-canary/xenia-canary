/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"

#include <atomic>
#include <cstddef>

#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_range.h"
#include "xenia/kernel/xboxkrnl/guest_object_type.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

// Whether every byte of a guest range can be read, and whether it can also be
// written.
bool IsGuestRangeReadable(uint32_t address, uint32_t length) {
  return util::IsGuestRangeReadable(kernel_memory(), address, length);
}

bool IsGuestRangeWritable(uint32_t address, uint32_t length) {
  return util::IsGuestRangeWritable(kernel_memory(), address, length);
}

// The ten X_OBJECT_TYPEs xenia defines itself sit together at the start of
// the kernel's guest globals, from ExThreadObjectType to
// ObSymbolicLinkObjectType; four of them have no host type of their own.
constexpr uint32_t kXeniaObjectTypesBegin =
    static_cast<uint32_t>(offsetof(KernelGuestGlobals, ExThreadObjectType));
constexpr uint32_t kXeniaObjectTypesEnd = static_cast<uint32_t>(
    offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType) +
    sizeof(X_OBJECT_TYPE));
static_assert(kXeniaObjectTypesEnd - kXeniaObjectTypesBegin ==
                  10 * sizeof(X_OBJECT_TYPE),
              "xenia's object types are no longer one block");

// An object type a title defined itself, as read from its guest memory.
struct TitleObjectType {
  uint32_t type_ptr;
  uint32_t close_proc;
  uint32_t delete_proc;
};

// Reads a title-defined type recorded by ObOpenObjectByPointer. Dispatch
// objects created directly in guest memory need not have an Object Manager
// header; closing them must not speculate about the bytes before their body.
bool ReadTitleObjectType(const XObject& object, TitleObjectType* out_type) {
  const uint32_t type_ptr = object.guest_object_type();
  const auto* type =
      IsGuestRangeReadable(type_ptr, sizeof(X_OBJECT_TYPE))
          ? kernel_memory()->TranslateVirtual<const X_OBJECT_TYPE*>(type_ptr)
          : nullptr;
  const uint32_t globals = kernel_state()->GetKernelGuestGlobals();
  if (!IsTitleObjectType(
          type_ptr, type != nullptr, type ? uint32_t(type->allocate_proc) : 0,
          globals + kXeniaObjectTypesBegin, globals + kXeniaObjectTypesEnd)) {
    return false;
  }
  *out_type = {type_ptr, type->close_proc, type->delete_proc};
  return true;
}

bool HasTitleObjectType(const XObject& object) {
  TitleObjectType type;
  return ReadTitleObjectType(object, &type);
}

// Whether a guest address can be the entry of a guest procedure: aligned,
// readable, and inside the code of a module the processor has loaded. A
// header read in front of an object ObCreateObject did not make can name a
// "type" whose procedures are anything at all.
bool IsGuestCodeAddress(uint32_t address) {
  return address && !(address & 3) && IsGuestRangeReadable(address, 4) &&
         kernel_state()->processor()->LookupModule(address);
}

// Said once per run, for the first close procedure that is not guest code.
void NoteCloseProcedureRefused(const TitleObjectType& type) {
  static std::atomic<bool> noted{false};
  if (!noted.exchange(true)) {
    XELOGW(
        "Object type {:08X} names a close procedure {:08X} that is not guest "
        "code, so it is not run",
        type.type_ptr, type.close_proc);
  }
}

// xenia keeps no pointer count for a guest object, so it never learns when the
// last reference to one goes, and a title-defined type's delete procedure (and
// its free procedure after it) never runs. Said once per run, for the first
// such type that is closed.
void NoteDeleteProcedureNotRun(const TitleObjectType& type) {
  static std::atomic<bool> noted{false};
  if (type.delete_proc && !noted.exchange(true)) {
    XELOGD(
        "Object type {:08X} has a delete procedure {:08X}, which is not run: "
        "no pointer count is kept for guest objects",
        type.type_ptr, type.delete_proc);
  }
}

// A guest object that xenia wraps carries its wrapper's handle in its dispatch
// header (XObject::GetNativeObject). Once the last reference on one of its
// handles has gone, the header is kept, pointed at a handle the object still
// has, or given back an empty wait list, as DecideStashedHandle says. Callers
// hold the global lock across the release and this, so that no kernel call
// on the object can resolve the closed handle in between.
void ForgetClosedHandle(XObject* object, X_HANDLE handle) {
  auto global_lock = global_critical_region::AcquireDirect();
  const uint32_t guest_object = object->guest_object();
  if (!IsGuestRangeWritable(guest_object, sizeof(X_DISPATCH_HEADER))) {
    return;
  }
  auto* header =
      kernel_memory()->TranslateVirtual<X_DISPATCH_HEADER*>(guest_object);
  const bool header_has_stash = header->wait_list.flink_ptr == kXObjSignature;
  const uint32_t stashed_handle = header->wait_list.blink_ptr;
  bool stashed_handle_wraps_same_object = false;
  if (header_has_stash && stashed_handle == handle) {
    auto holder = kernel_state()->object_table()->LookupObject<XObject>(
        stashed_handle, true);
    stashed_handle_wraps_same_object =
        holder && holder->guest_object() == guest_object;
  }
  const auto decision =
      DecideStashedHandle(handle, object->handles(), header_has_stash,
                          stashed_handle, stashed_handle_wraps_same_object);
  switch (decision.action) {
    case StashedHandleAction::kKeep:
      break;
    case StashedHandleAction::kRepoint:
      header->wait_list.blink_ptr = decision.handle;
      break;
    case StashedHandleAction::kReset: {
      const uint32_t wait_list =
          guest_object +
          static_cast<uint32_t>(offsetof(X_DISPATCH_HEADER, wait_list));
      header->wait_list.flink_ptr = wait_list;
      header->wait_list.blink_ptr = wait_list;
    } break;
  }
}

}  // namespace

void xeObSplitName(X_ANSI_STRING input_string,
                   X_ANSI_STRING* leading_path_component,
                   X_ANSI_STRING* remaining_path_components,
                   PPCContext* context) {
  xe::FatalError("xeObSplitName unimplemented!");
}

uint32_t xeObHashObjectName(X_ANSI_STRING* ElementName, PPCContext* context) {
  uint8_t* current_character_ptr =
      context->TranslateVirtual(ElementName->pointer);
  uint32_t result = 0;
  uint8_t* name_span_end = &current_character_ptr[ElementName->length];
  while (current_character_ptr < name_span_end) {
    uint32_t current_character = *current_character_ptr++;
    if (current_character < 0x80) {
      result = (current_character | 0x20) + (result >> 1) + 3 * result;
    }
  }
  return result % 0xD;
}

uint32_t xeObCreateObject(X_OBJECT_TYPE* object_factory,
                          X_OBJECT_ATTRIBUTES* optional_attributes,
                          uint32_t object_size_without_headers,
                          uint32_t* out_object, cpu::ppc::PPCContext* context) {
  unsigned int resulting_header_flags = 0;
  *out_object = 0;
  unsigned int poolarg = 0;

  auto get_flags_and_poolarg_for_process_type = [&resulting_header_flags,
                                                 &poolarg, context]() {
    uint32_t process_type = xboxkrnl::xeKeGetCurrentProcessType(context);
    if (process_type == X_PROCTYPE_TITLE) {
      poolarg = 1;
      resulting_header_flags = OBJECT_HEADER_IS_TITLE_OBJECT;
    } else {
      poolarg = 2;
    }
  };
  if (!optional_attributes) {
    get_flags_and_poolarg_for_process_type();
  }

  else if ((optional_attributes->attributes & 0x1000) == 0) {
    if ((optional_attributes->attributes & 0x2000) != 0) {
      poolarg = 2;
    } else {
      get_flags_and_poolarg_for_process_type();
    }
  } else {
    poolarg = 1;
    resulting_header_flags = OBJECT_HEADER_IS_TITLE_OBJECT;
  }
  uint32_t desired_object_path_ptr;

  if (!optional_attributes ||
      (desired_object_path_ptr = optional_attributes->name_ptr) == 0) {
    /*
        object has no name provided, just allocate an object with a basic header
    */
    uint64_t allocate_args[] = {
        object_size_without_headers + sizeof(X_OBJECT_HEADER),
        object_factory->pool_tag, poolarg};
    context->processor->Execute(
        context->thread_state, object_factory->allocate_proc, allocate_args, 3);

    uint32_t allocation = static_cast<uint32_t>(context->r[3]);

    if (allocation) {
      X_OBJECT_HEADER* new_object_header =
          context->TranslateVirtual<X_OBJECT_HEADER*>(allocation);
      new_object_header->pointer_count = 1;
      new_object_header->handle_count = 0;
      new_object_header->object_type_ptr =
          context->HostToGuestVirtual(object_factory);
      new_object_header->flags = resulting_header_flags;

      *out_object = allocation + sizeof(X_OBJECT_HEADER);
      return X_STATUS_SUCCESS;
    }
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }
  /*
    iterate through all path components until we obtain the final one, which is
    the objects actual name
  */
  X_ANSI_STRING trailing_path_component;
  X_ANSI_STRING remaining_path;
  X_ANSI_STRING loaded_object_name;
  loaded_object_name =
      *context->TranslateVirtual<X_ANSI_STRING*>(desired_object_path_ptr);
  trailing_path_component.pointer = 0;
  trailing_path_component.length = 0;
  remaining_path = loaded_object_name;
  while (remaining_path.length) {
    xeObSplitName(remaining_path, &trailing_path_component, &remaining_path,
                  context);
    if (remaining_path.length) {
      if (*context->TranslateVirtual<char*>(remaining_path.pointer) == '\\') {
        return X_STATUS_OBJECT_NAME_INVALID;
      }
    }
  }
  if (!trailing_path_component.length) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  // the object and its name are all created in a single allocation

  unsigned int aligned_object_size =
      xe::align<uint32_t>(object_size_without_headers, 4);
  {
    uint64_t allocate_args[] = {
        trailing_path_component.length + aligned_object_size +
            sizeof(X_OBJECT_HEADER_NAME_INFO) + sizeof(X_OBJECT_HEADER),
        object_factory->pool_tag, poolarg};

    context->processor->Execute(
        context->thread_state, object_factory->allocate_proc, allocate_args, 3);
  }
  uint32_t named_object_allocation = static_cast<uint32_t>(context->r[3]);
  if (!named_object_allocation) {
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }

  X_OBJECT_HEADER_NAME_INFO* nameinfo =
      context->TranslateVirtual<X_OBJECT_HEADER_NAME_INFO*>(
          named_object_allocation);
  nameinfo->next_in_directory = 0;
  nameinfo->object_directory = 0;

  X_OBJECT_HEADER* header_for_named_object =
      reinterpret_cast<X_OBJECT_HEADER*>(nameinfo + 1);

  char* name_string_memory_for_named_object = &reinterpret_cast<char*>(
      header_for_named_object + 1)[aligned_object_size];
  nameinfo->name.pointer =
      context->HostToGuestVirtual(name_string_memory_for_named_object);
  nameinfo->name.length = trailing_path_component.length;
  nameinfo->name.maximum_length = trailing_path_component.length;

  memcpy(name_string_memory_for_named_object,
         context->TranslateVirtual<char*>(trailing_path_component.pointer),
         trailing_path_component.length);

  header_for_named_object->pointer_count = 1;
  header_for_named_object->handle_count = 0;
  header_for_named_object->object_type_ptr =
      context->HostToGuestVirtual(object_factory);
  header_for_named_object->flags =
      resulting_header_flags & 0xFFFE | OBJECT_HEADER_FLAG_NAMED_OBJECT;
  *out_object = context->HostToGuestVirtual(&header_for_named_object[1]);
  return X_STATUS_SUCCESS;
}
dword_result_t ObOpenObjectByName_entry(lpunknown_t obj_attributes_ptr,
                                        lpunknown_t object_type_ptr,
                                        dword_t unk, lpdword_t handle_ptr) {
  // r3 = ptr to info?
  //   +0 = -4
  //   +4 = name ptr
  //   +8 = 0
  // r4 = ExEventObjectType | ExSemaphoreObjectType | ExTimerObjectType
  // r5 = 0
  // r6 = out_ptr (handle?)

  if (!obj_attributes_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto obj_attributes = kernel_memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
      obj_attributes_ptr);
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_memory(),
                                               obj_attributes->name_ptr);

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->GetObjectByName(name, &handle);
  if (XSUCCEEDED(result)) {
    *handle_ptr = handle;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByName, kNone, kImplemented);
// chrispy: investigate this, pretty certain it does not properly emulate the
// original
dword_result_t ObOpenObjectByPointer_entry(lpvoid_t object_ptr,
                                           lpdword_t out_handle_ptr) {
  *out_handle_ptr = 0;
  auto global_lock = global_critical_region::AcquireDirect();
  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), object_ptr, X_OBJECT_TYPES::UndefinedObject, true);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  object->SetNativeType();

  if (HasTitleObjectType(*object)) {
    const X_STATUS result = kernel_state()->object_table()->OpenGuestHandle(
        object->handle(), object.get());
    if (XFAILED(result)) {
      return result;
    }
  } else {
    // Existing kernel types retain a handle for each open.
    object->RetainHandle();
  }
  *out_handle_ptr = object->handle();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByPointer, kNone, kImplemented);

dword_result_t ObLookupThreadByThreadId_entry(dword_t thread_id,
                                              lpdword_t out_object_ptr) {
  auto thread = kernel_state()->GetThreadByID(thread_id);
  if (!thread) {
    *out_object_ptr = 0;
    return X_STATUS_INVALID_PARAMETER;
  }

  // Retain the object. Will be released in ObDereferenceObject.
  thread->RetainHandle();
  *out_object_ptr = thread->guest_object();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObLookupThreadByThreadId, kNone, kImplemented);

dword_result_t ObLookupAnyThreadByThreadId_entry(dword_t thread_id,
                                                 lpdword_t out_object_ptr) {
  return ObLookupThreadByThreadId_entry(thread_id, out_object_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(ObLookupAnyThreadByThreadId, kNone, kImplemented);

dword_result_t ObReferenceObjectByHandle_entry(dword_t handle,
                                               dword_t object_type_ptr,
                                               lpdword_t out_object_ptr) {
  // chrispy: gotta preinit this to 0, kernel is expected to do that
  *out_object_ptr = 0;

  auto object = kernel_state()->object_table()->LookupObject<XObject>(handle);
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }
  if (HasTitleObjectType(*object) &&
      !kernel_state()->object_table()->IsGuestHandleOpen(handle,
                                                         object.get())) {
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t native_ptr = object->guest_object();

  if (object_type_ptr) {
    const auto& object_types =
        kernel_state()->host_object_type_enum_to_guest_object_type_ptr_;
    const auto host_type = object_types.find(object->type());
    const uint32_t host_type_ptr =
        host_type != object_types.end() ? host_type->second : 0;
    // A recorded guest type takes precedence over the wrapper's host type.
    switch (CheckObjectType(object_type_ptr, host_type_ptr,
                            object->guest_object_type())) {
      case ObjectTypeCheck::kHostTypeMatches:
        break;
      case ObjectTypeCheck::kGuestTypeMatches:
        break;
      case ObjectTypeCheck::kMismatch:
        return X_STATUS_OBJECT_TYPE_MISMATCH;
      case ObjectTypeCheck::kUnknown:
        assert_unhandled_case(object->type());
        native_ptr = 0xDEADF00D;
        break;
    }
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  object->RetainHandle();

  assert_not_zero(native_ptr);

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = native_ptr;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByHandle, kNone, kImplemented);

dword_result_t ObReferenceObjectByName_entry(pointer_t<X_ANSI_STRING> name,
                                             dword_t attributes,
                                             dword_t object_type_ptr,
                                             lpvoid_t parse_context,
                                             lpdword_t out_object_ptr,
                                             const ppc_context_t& ctx) {
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;

  char* name_str = ctx.TranslateVirtual<char*>(name->pointer);
  X_STATUS result =
      kernel_state()->object_table()->GetObjectByName(name_str, &handle);
  if (XSUCCEEDED(result)) {
    return ObReferenceObjectByHandle_entry(handle, object_type_ptr,
                                           out_object_ptr);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByName, kNone, kImplemented);

void xeObDereferenceObject(PPCContext* context, uint32_t native_ptr) {
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (native_ptr == 0xDEADF00D) {
    return;
  }
  if (!native_ptr) {
    XELOGE("Null native ptr in ObDereferenceObject!");
    return;
  }

  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_memory()->TranslateVirtual(native_ptr));
  if (object) {
    if (HasTitleObjectType(*object)) {
      // A reference taken with ObReferenceObjectByHandle counts against the
      // handle, so for an object of a title-defined type it can be what drops
      // the last of it, after the title's NtClose.
      auto global_lock = global_critical_region::AcquireDirect();
      if (!object->handles().empty()) {
        const X_HANDLE handle = object->handle();
        object->ReleaseHandle();
        ForgetClosedHandle(object.get(), handle);
      }
    } else {
      object->ReleaseHandle();
    }
  } else {
    if (native_ptr) {
      XELOGW("Unregistered guest object provided to ObDereferenceObject {:08X}",
             native_ptr);
    }
  }
  return;
}

void ObDereferenceObject_entry(dword_t native_ptr, const ppc_context_t& ctx) {
  xeObDereferenceObject(ctx, native_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(ObDereferenceObject, kNone, kImplemented);

void ObReferenceObject_entry(dword_t native_ptr) {
  // Check if a dummy value from ObReferenceObjectByHandle.
  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_memory()->TranslateVirtual(native_ptr));
  if (object) {
    object->RetainHandle();
  } else {
    if (native_ptr) {
      XELOGW("Unregistered guest object provided to ObReferenceObject {:08X}",
             native_ptr.value());
    }
  }
  return;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObject, kNone, kImplemented);

dword_result_t ObCreateSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr,
                                          pointer_t<X_ANSI_STRING> target_ptr) {
  auto path = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiPath(kernel_memory(), path_ptr));
  auto target = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiPath(kernel_memory(), target_ptr));

  if (xe::utf8::starts_with(path, "\\??\\")) {
    path = path.substr(4);  // Strip the full qualifier
  }

  if (xe::utf8::starts_with(path, "\\System??\\")) {
    path = path.substr(10);  // Strip the full qualifier
  }

  // 4D5307DC expects success.
  if (kernel_state()->file_system()->FindSymbolicLink(path, target)) {
    return X_STATUS_SUCCESS;
  }

  if (!kernel_state()->file_system()->RegisterSymbolicLink(path, target)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateSymbolicLink, kNone, kImplemented);

dword_result_t ObDeleteSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr) {
  auto path = util::TranslateAnsiPath(kernel_memory(), path_ptr);
  if (!kernel_state()->file_system()->UnregisterSymbolicLink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObDeleteSymbolicLink, kNone, kImplemented);

dword_result_t NtDuplicateObject_entry(dword_t handle, lpdword_t new_handle_ptr,
                                       dword_t options) {
  // NOTE: new_handle_ptr can be zero to just close a handle.
  // NOTE: this function seems to be used to get the current thread handle
  //       (passed handle=-2).
  // This function actually just creates a new handle to the same object.
  // Most games use it to get real handles to the current thread or whatever.

  X_HANDLE new_handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->DuplicateHandle(handle, &new_handle);

  if (new_handle_ptr) {
    *new_handle_ptr = new_handle;
  }

  if (options == 1 /* DUPLICATE_CLOSE_SOURCE */) {
    NtClose(handle);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtDuplicateObject, kNone, kImplemented);

namespace {

// Run a title type's close procedure on the calling guest thread, when present.
uint32_t CloseTitleObjectHandle(uint32_t handle, XObject* object,
                                const TitleObjectType& title_type,
                                PPCContext* context) {
  auto* object_table = kernel_state()->object_table();
  // Claim before executing guest code so reentrant closes cannot claim it.
  int32_t handle_count = 0;
  uint64_t generation = 0;
  const X_STATUS claim_result =
      object_table->ClaimGuestClose(handle, object, &handle_count, &generation);
  if (XFAILED(claim_result)) {
    return claim_result;
  }
  switch (PlanCloseProcedure(title_type.close_proc,
                             IsGuestCodeAddress(title_type.close_proc),
                             handle_count)) {
    case CloseProcedureAction::kNone:
      break;
    case CloseProcedureAction::kRun: {
      if (!context) {
        XELOGD(
            "NtClose({:08X}) on a host thread: close procedure {:08X} of type "
            "{:08X} is not run",
            handle, title_type.close_proc, title_type.type_ptr);
        break;
      }
      XELOGD(
          "NtClose({:08X}): close procedure {:08X}({:08X}, {}) of type {:08X}",
          handle, title_type.close_proc, object->guest_object(), handle_count,
          title_type.type_ptr);
      uint64_t args[] = {object->guest_object(),
                         static_cast<uint32_t>(handle_count)};
      context->processor->Execute(context->thread_state, title_type.close_proc,
                                  args, xe::countof(args));
    } break;
    case CloseProcedureAction::kRefuse:
      NoteCloseProcedureRefused(title_type);
      break;
  }
  NoteDeleteProcedureNotRun(title_type);

  // xenia counts a title's ObDereferenceObject against the handle the object
  // was found by, so the close procedure may have released it already.
  auto global_lock = global_critical_region::AcquireDirect();
  object_table->ReleaseHandleOf(handle, object, generation);
  ForgetClosedHandle(object, handle);
  return X_STATUS_SUCCESS;
}

}  // namespace

// A host close releases the handle even when no guest thread can run its hook.
uint32_t NtClose(uint32_t handle) {
  auto* object_table = kernel_state()->object_table();
  {
    auto object = object_table->LookupObject<XObject>(handle);
    TitleObjectType title_type;
    if (object && ReadTitleObjectType(*object, &title_type)) {
      XThread* thread =
          XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
      return CloseTitleObjectHandle(
          handle, object.get(), title_type,
          thread ? thread->thread_state()->context() : nullptr);
    }
  }
  return object_table->ReleaseHandle(handle);
}

dword_result_t NtClose_entry(dword_t handle) { return NtClose(handle); }
DECLARE_XBOXKRNL_EXPORT1(NtClose, kNone, kImplemented);

dword_result_t ObCreateObject_entry(
    pointer_t<X_OBJECT_TYPE> object_factory,
    pointer_t<X_OBJECT_ATTRIBUTES> optional_attributes,
    dword_t object_size_sans_headers, lpdword_t out_object,
    const ppc_context_t& context) {
  uint32_t out_object_tmp = 0;

  uint32_t result =
      xeObCreateObject(object_factory, optional_attributes,
                       object_size_sans_headers, &out_object_tmp, context);
  *out_object = out_object_tmp;
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateObject, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ob);
