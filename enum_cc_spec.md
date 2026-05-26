# Enumerated Calling Convention

Rather than using classic call/ret, we will use a separate calling convention that pops an enum off the call stack which selects a 'safe' return address from RO memory.

This will be tested in a modified version of tinycc for x86_64 for testability on my Mac Mini.


## The convention itself.

See here the standard fashion of calling.

```asm
# add_nums(rdi=a, rsi=b) -> rax = a+b
add_nums:
    movq  %rdi, %rax
    addq  %rsi, %rax
    ret

_main:
    movq  $40, %rdi
    movq  $2, %rsi
    call  add_nums
```

What I suggest is the use of an enum instead of a return address that is implicitly used above.

```asm
.section __DATA,__const
add_nums_jmp_table:
    .quad after_add_1 - add_nums_jmp_table
    .quad after_add_2 - add_nums_jmp_table

.section __TEXT,__text
.globl _main

# add_nums(rdi=a, rsi=b) -> rax = a+b
add_nums:
    movq  %rdi, %rax
    addq  %rsi, %rax

    # Return sequence — scratch registers chosen by allocator, shown here as R1/R2
    popq  <R1>                               # pop caller-pushed enum index
    cmpq  $2, <R1>                           # $N patched by X86_64_RELOC_ENUM_MAX_VALUE
    jae   mem_corruption
    leaq  add_nums_jmp_table(%rip), <R2>    # table base address
    movq  (<R2>,<R1>,8), <R1>               # offset = table[enum]
    addq  <R1>, <R2>                         # JTBA + offset = callsite
    jmpq  *<R2>

_main:
    pushq $0                # enum index; patched by X86_64_RELOC_ENUM_CALL_VALUE
    jmp   add_nums
after_add_1:
```

First clear difference is that the caller does `pushq $N; jmp fn` instead of `call fn`. No return address is pushed — only the enum index. The callee pops the index with `popq <R1>` and uses it to index into a read-only jump table. No writable stack data participates in the backward edge.

The index is bounds-checked against a hardcoded value that is patched at link time by `X86_64_RELOC_ENUM_MAX_VALUE`. The enum indexes a jump table which is an array of offsets, NOT POINTERS, for ASLR safety. When these offsets are added to the jump table base address you get the callsite address.

Two scratch registers are needed for the return sequence. The register allocator chooses caller-saved registers so the return sequence does not violate callee-save obligations.
1. **(IDX reg)** Receives the popped enum index, then the loaded table offset.
2. **(JTBA reg)** Holds the jump table base address, then the computed callsite address.

Adding the offset to JTBA gives the callsite address.

### Corruption handler

`mem_corruption` is a single shared stub per binary. It tail-calls `__stack_chk_fail`, which is exported by macOS libSystem and is therefore available to any binary without additional runtime code:

```asm
.globl _mem_corruption
_mem_corruption:
    jmpq  ___stack_chk_fail   # tail-call, noreturn
```

`__stack_chk_fail` terminates the process and generates a crash log. No message printing is needed in the stub itself.

### Non-leaf functions

A non-leaf function calls other functions, so it must preserve the incoming enum index across those calls. The index arrives on the stack (from the caller's `pushq $N`) and is popped immediately on entry into a callee-saved register, which it holds for the duration of the function body.

```asm
# Non-leaf prologue
popq  <ENUM>            # pop enum into callee-saved register (e.g. %r12)
pushq %rbp
movq  %rsp, %rbp
# ... save other callee-saved registers, allocate locals ...

# ... function body; internal calls use standard call/jmp ...

# Non-leaf epilogue
# ... restore other callee-saved registers ...
popq  %rbp

# Return sequence using preserved enum
cmpq  $N, <ENUM>        # N patched by X86_64_RELOC_ENUM_MAX_VALUE
jae   mem_corruption
leaq  fn_jmp_table(%rip), <R1>
movq  (<R1>,<ENUM>,8), <R2>
addq  <R2>, <R1>
jmpq  *<R1>
```

`<ENUM>` is a callee-saved register (`%r12`–`%r15`) chosen by the register allocator. Because it is callee-saved the function must push and pop it alongside `%rbp` as part of the frame. `<R1>` and `<R2>` in the return sequence are distinct caller-saved scratch registers.


## Jump table entry format

Three options with different size and instruction-count trade-offs.

### Option 1 — 64-bit table-base-relative (default)

```asm
.section __DATA,__const
add_nums_jmp_table:
    .quad after_add_1 - add_nums_jmp_table
    .quad after_add_2 - add_nums_jmp_table
```

Dispatch:
```asm
popq   <R1>
leaq   add_nums_jmp_table(%rip), <R2>
movq   (<R2>,<R1>,8), <R1>              # load 64-bit offset
addq   <R1>, <R2>                        # base + offset = target
jmpq   *<R2>
```

4 dispatch instructions. Read-only `__DATA,__const`, no ASLR fixup. 8 bytes per entry.

### Option 2 — Absolute addresses

```asm
.section __DATA,__data          # writable; dyld patches for ASLR
add_nums_jmp_table:
    .quad after_add_1
    .quad after_add_2
```

Dispatch:
```asm
popq   <R1>
leaq   add_nums_jmp_table(%rip), <R2>
jmpq   *(<R2>,<R1>,8)           # load absolute address and jump — one instruction
```

2 dispatch instructions. Requires writable `__DATA`; dyld patches each entry at load time. 8 bytes per entry.

### Option 3 — 32-bit table-base-relative (R_X86_64_PC32-equivalent)

```asm
.section __DATA,__const
add_nums_jmp_table:
    .long after_add_1 - add_nums_jmp_table
    .long after_add_2 - add_nums_jmp_table
```

Dispatch:
```asm
popq    <R1>
leaq    add_nums_jmp_table(%rip), <R2>
movslq  (<R2>,<R1>,4), <R1>    # sign-extend 32-bit offset; scale 4 for 4-byte entries
addq    <R1>, <R2>
jmpq    *<R2>
```

4 dispatch instructions. Read-only `__DATA,__const`, no ASLR fixup. 4 bytes per entry — half the size of options 1 and 2. ±2 GB range is sufficient for any realistic binary. Linker-generated cross-TU tables use this format because the entries are standard `X86_64_RELOC_SIGNED`-equivalent relocations that require no linker extension.


## Cross-TU: linker-generated tables

Within a single compilation unit, `after_add_1 - add_nums_jmp_table` is resolved at assemble time — the assembler computes the constant directly and emits no relocation. This breaks when the caller and callee are in separate object files because the assembler cannot compute the difference between symbols it does not own.

The solution is for the linker to generate the jump tables. Two new marker relocations signal the linker to do so.

### `X86_64_RELOC_CALLCONV_TABLE_DECLARE`

Emitted by the **callee** object file, associated with the function symbol. Tells the linker: "allocate a jump table for this function in `__DATA,__const` using the 32-bit table-base-relative format (Option 3)."

No value is patched at link time; this relocation is purely declarative.

### `X86_64_RELOC_CALLCONV_SITE`

Emitted by the **caller** object file at each call site label (e.g. `after_add_1`). The relocation record carries:
- The target function symbol (`add_nums`)
- The enum index (`0`, `1`, …)

The linker collects all `X86_64_RELOC_CALLCONV_SITE` records across every object file for a given function, sorts by index, and fills the corresponding table slot with `callsite_address - table_base` as a signed 32-bit value.

### Link-time workflow

1. Callee `.o`: function body + `X86_64_RELOC_CALLCONV_TABLE_DECLARE` on the function symbol.
2. Each caller `.o`: `pushq $N; jmp fn` + `X86_64_RELOC_CALLCONV_SITE(fn, N)` at `after_add_N`.
3. Linker synthesizes `fn_jmp_table` in `__DATA,__const`, fills entries from collected site addresses, and resolves the `leaq fn_jmp_table(%rip)` reference in the callee's dispatch sequence.

This is analogous to how Mach-O handles `__gcc_except_tab`, ObjC method lists, and Swift protocol witness tables — synthesized sections whose contents are scattered across multiple object files.


## Implementation A — tinycc (single-TU POC)

tinycc is chosen for its simplicity and *lack* of features. It assumes a single compilation unit — all functions in scope were compiled in the same invocation. This constraint is a feature for the POC: cross-TU is out of scope, so the compiler always knows whether a call target uses the convention.

> This convention cannot be used with most function pointer use cases. When detected the compiler should error. Any code that NEEDS to use function pointers should be compiled with clang and linked with this modified compiler afterwards.

The `Sym` struct is modified to carry an `enum_callconv` flag, set when a function body is compiled in the current invocation. At every call site, the compiler checks this flag:
- Flag set → emit `pushq $N; jmp fn` with `X86_64_RELOC_ENUM_CALL_VALUE` and `X86_64_RELOC_ENUM_MAX_VALUE`
- Flag not set (extern, shared lib) → emit standard `call fn`

The enum index N is a per-function counter incremented by the compiler each time a new call site is emitted. Because all callers are in the same TU, indices are globally consistent without linker involvement.

Relocations emitted per function definition:
1. `X86_64_RELOC_ENUM_MAX_VALUE` — patches the `cmpq $N` bound check in the return sequence
2. `X86_64_RELOC_JMPTABLE_OFFSET` — fills each table slot with `callsite - table_base`

Relocations emitted per call site:
3. `X86_64_RELOC_ENUM_CALL_VALUE` — patches the `pushq $N` immediate
