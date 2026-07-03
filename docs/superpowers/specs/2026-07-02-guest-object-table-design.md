# guest_object_table revival — design

Date: 2026-07-02
Scope: documentation-only change to `src/xenia/kernel/util/guest_object_table.h`.
No behavior change. (api-scanner revival was considered and dropped from scope.)

## Problem

`guest_object_table.{cc,h}` is a faithful reverse-engineering of the Xbox 360
kernel's internal handle-table routines (`ObpXxx`), operating on guest memory
via `PPCContext`. It was added in commit `6a08208dc` (Oct 2023) alongside the
AllocatePool misalignment work and has zero callers in this fork, upstream
canary, and the AdrianCassar netplay fork (verified 2026-07-02). Its five
function declarations are commented out in the header, so no branch can call
it without editing the header first. Per project policy, dormant code is fixed
in place, not deleted — other branches may wire it later.

## Change

Single file: `src/xenia/kernel/util/guest_object_table.h`.

1. **Doc block** at the top of the `util` namespace covering:
   - What it is: byte-accurate recreation of the 360 kernel's handle table in
     guest memory (guest pool allocations via `xeAllocatePoolTypeWithTag`,
     real `X_KSPINLOCK` locking, big-endian fields, `X_HANDLE_TABLE` layout
     asserted at 0x38 bytes).
   - Provenance: `6a08208dc`, Oct 2023; dormant since; no fork uses it yet.
   - Intended future use: guest-visible handle tables for titles that
     introspect kernel structures — the accuracy counterpart to the host-side
     `util::ObjectTable` (`object_table.h`), which currently owns all real
     handle allocation.
   - Field notes: `unk_36` gates allocation (nonzero = fail), `handle_high_byte`
     tags all handles from this table, `unk_pool_arg_34` is forwarded to pool
     allocations.

2. **Restore the five declarations** currently inside the `/* */` block,
   corrected to match the `.cc` definitions exactly, one-line comment each:
   - `bool GrowHandleTable(uint32_t table_ptr, cpu::ppc::PPCContext* context);`
   - `uint32_t NewObjectHandle(uint32_t table_guest, uint32_t object_guest, cpu::ppc::PPCContext* context);`
   - `uint32_t DestroyObjectHandle(uint32_t table_guest, uint32_t handle, cpu::ppc::PPCContext* context);`
   - `uint32_t LookupHandleUnlocked(X_HANDLE_TABLE* table, guest_handle_t handle, bool reference_object, cpu::ppc::PPCContext* context);`
   - `uint32_t LookupHandle(uint32_t table, uint32_t handle, uint32_t reference_object, cpu::ppc::PPCContext* context);`

   `InitializeNewHandleRange` stays `.cc`-internal (helper, not API).

3. **Include `xenia/kernel/util/shim_utils.h`** to make `cpu::ppc::PPCContext`
   visible — the same pattern sibling kernel headers use for PPCContext in
   signatures (e.g. `xboxkrnl_threading.h`). `PPCContext` is a global-scope
   `typedef struct PPCContext_s`, so a forward declaration would be novel in
   this codebase; reuse the established include instead.

## Not changing

- No edits to `guest_object_table.cc` (it compiles today via source globbing).
- No wiring into `kernel_state`/`xboxkrnl` — that is the future work this
  change enables, gated on a title that needs guest-visible handle tables.
- No unit tests — nothing new is executable; the functions were already
  compiled and remain uncalled.

## Verification

- No local toolchain exists (no VS, no g++/clang). Verification is CI:
  Lint job (`xb lint --all`, pinned clang-format) plus Linux (clang) and
  Windows (MSVC) build jobs compile the header with declarations active.
- Local pre-push checks: added lines ≤ 80 columns, LF endings, no trailing
  NUL bytes (OneDrive Edit quirk), header self-consistency read-through.
- Commit format: `[Kernel] <description>` per Check_commit_message CI rule.

## Risks

- Declaration/definition mismatch would fail both CI builds — caught before
  merge. No runtime risk: nothing calls the functions.
