# Enumerated Calling Convention — Implementation Progress

## Status: In progress — x86_64 linker fixed, final run test pending

---

## What's done

### All code changes implemented

| File | Change | Status |
|------|--------|--------|
| `tcc.h` | `enum_callconv:1` bit in `FuncAttr`; `EnumCCCallSite`/`EnumCCFunc` structs; `enum_cc_funcs`/`nb_enum_cc_funcs` fields in `TCCState` | Done |
| `tcctok.h` | `TOK_ENUM_CC1`/`TOK_ENUM_CC2` tokens | Done |
| `tccgen.c` | `merge_funcattr` fix; attribute parser cases; `enum_cc_alloc_tables(s1, rdatasec)` (Phase 1); `enum_cc_finalize(s1, textsec, rdatasec)` (Phase 2) | Done |
| `x86_64-gen.c` | Static state (`func_enum_callconv`, `func_enum_cc`, `mem_corruption_sym`); `init_mem_corruption_sym()`; `gfunc_prolog` enum_cc path; `gfunc_epilog` return sequence; `gfunc_call` dispatch | Done |
| `tccmacho.c` | Forward decls; `enum_cc_alloc_tables(s1, rodata_section)` before `collect_sections`; `enum_cc_finalize(s1, text_section, rodata_section)` after `relocate_syms` | Done |

---

## Bugs found and fixed during implementation

### 1. `TOK_main` undefined in x86_64-gen.c
TCC has no `TOK_main` constant. Fixed by using `strcmp(funcname, "main") == 0`.

### 2. `emit_mem_corruption_stub()` overwrote function code
The stub tried to save/restore `ind` and write directly to `text_section`, but the current function's prolog hadn't been emitted yet — both started at `ind=0` — so the stub bytes were overwritten. Fixed: removed the stub entirely, replaced with `init_mem_corruption_sym()` which lazily calls `external_helper_sym("__stack_chk_fail")`. Changed `jae` relocation to `R_X86_64_PLT32`.

### 3. `section_add` called after `collect_sections` → SIGSEGV
Original `enum_cc_finalize` allocated jump-table space in rodata AFTER the linker had computed section sizes and VAs. Writes went to unmapped memory. Fixed by splitting into two phases:
- `enum_cc_alloc_tables()` — allocate space, called **before** `collect_sections`
- `enum_cc_finalize()` — patch values, called **after** `relocate_syms`

### 4. `tcc_state` is NULL when linker calls enum_cc helpers (SIGSEGV at 0x510)
`tccgen.c` defines `USING_GLOBALS`, so `rodata_section` expands to `tcc_state->rodata_section`. But `tcc_exit_state()` sets `tcc_state = NULL` after compilation finishes, before `tcc_output_file` / `macho_output_file` runs. Fixed by changing both functions to accept explicit `Section *` parameters, bypassing the macro entirely:
```c
enum_cc_alloc_tables(s1, rdatasec)
enum_cc_finalize(s1, textsec, rdatasec)
```
Call sites in `tccmacho.c` (no `USING_GLOBALS`) pass `rodata_section` → `s1->rodata_section` correctly.

---

## Test results

| Test | Result |
|------|--------|
| `make` (build tinycc host compiler) | Pass |
| `make x86_64-osx-tcc ONE_SOURCE=yes` (cross-compiler) | Pass |
| ARM64 native test (`./tcc -o test_enum_cc test_enum_cc.c && ./test_enum_cc`) | Pass (ARM64 gen ignores the attribute; standard ARM64 code runs correctly) |
| x86_64 object file generation (`x86_64-osx-tcc -c`) | Pass |
| x86_64 disassembly check (objdump confirms pushq/jmp at call sites, return sequence in epilog) | Pass |
| x86_64 link — 1-function test `add` only | Pass (link exit 0, run exit 0 via Rosetta 2) |
| x86_64 link — full standalone test (add + mul + compute) | **Pending** |

---

## Next step

Run the full standalone test:
```sh
SDK=$(xcrun --show-sdk-path)
./x86_64-osx-tcc -B. -I"$SDK/usr/include" -L"$SDK/usr/lib" -lSystem \
    -o /tmp/test_enum_cc_x86 test_enum_cc_standalone.c
arch -x86_64 /tmp/test_enum_cc_x86
echo "exit: $?"
```
Expected: exit 0.

Then verify with `otool -tv` that no `call`/`ret` appear in enum_callconv functions, and that the jump table lands in `__TEXT,__rodata`.
