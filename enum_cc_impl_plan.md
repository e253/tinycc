# Enumerated Calling Convention — Implementation Plan

## Context

Implement `enum_cc_spec.md`: a security-hardened calling convention for x86_64 macOS that replaces `call`/`ret` with `pushq $N; jmp fn` at call sites and a bounds-checked indirect jump through a read-only jump table at return sites. The goal is to eliminate writable-stack backward edges. This is a single-TU POC in tinycc.

**Branch**: `feature/enum-cc` off `mob`

---

## Convention Summary

**Caller** emits: `pushq $N; jmp fn` (no return address on stack — only the enum index)

**Callee return sequence** (replaces `ret`):
```asm
movq  -8(%rbp), %r12          # reload saved ENUM index
movq  %rbp, %rsp              # manual leave (part 1)
popq  %rbp                    # manual leave (part 2)
cmpq  $MAX, %r12              # bounds check; MAX patched at finalization
jae   mem_corruption          # (0F 83 rel32)
leaq  fn_jmp_table(%rip), %r11 # table base; rel32 patched at finalization
movq  (%r11,%r12,8), %r10    # load 64-bit offset
addq  %r10, %r11              # base + offset = callsite address
jmpq  *%r11
```

**Jump table** in `.rodata` (allocated at finalization):
```
fn_jmp_table:  .quad after_call_0 - fn_jmp_table
               .quad after_call_1 - fn_jmp_table
```

**mem_corruption stub** (one per binary): `jmpq ___stack_chk_fail`

**Register assignment** (fixed for POC):
- `%r12` — ENUM index register (callee-saved; saved at -8(%rbp) in prologue)
- `%r10`, `%r11` — scratch in return sequence (caller-saved; clobber is fine)

**Key constraint**: callee must be *defined* before its callers in the source file (single-pass limitation of the POC).

---

## Files Modified

| File | What changes |
|------|-------------|
| `tcc.h` | Add `enum_callconv:1` to `FuncAttr`; add `EnumCCCallSite`/`EnumCCFunc` structs; add 3 fields to `TCCState` |
| `tcctok.h` | Add `TOK_ENUM_CC1`/`TOK_ENUM_CC2` tokens |
| `tccgen.c` | Attribute parser case; `merge_funcattr` fix; `enum_cc_finalize()` function |
| `x86_64-gen.c` | `gfunc_prolog`, `gfunc_epilog`, `gfunc_call` (SysV path only); `emit_mem_corruption_stub()` |
| `tccmacho.c` | One-line finalization hook between `relocate_syms` and `relocate_sections` |

---

## Step-by-Step Changes

### 1. `tcc.h` — Data structures

**1a. FuncAttr** (line 528): change `xxxx : 15` → `enum_callconv : 1, xxxx : 14`

**1b. New structs** — add after line 566 (after `Sym`):
```c
struct EnumCCCallSite {
    unsigned long after_call_offset; /* offset in text_section of "after_call_N" */
};

struct EnumCCFunc {
    Sym               *fn_sym;
    unsigned long      cmpq_imm_offset;   /* offset of 4-byte imm in cmpq $MAX,%r12 */
    unsigned long      leaq_rel32_offset; /* offset of 4-byte rel32 in leaq fn_jmptbl(%rip),%r11 */
    unsigned long      jmptable_offset;   /* offset in rodata_section; set at finalization */
    struct EnumCCCallSite *callsites;
    int                nb_callsites;
    int                cap_callsites;
};
```

**1c. TCCState fields** — add after line 929 (after `eh_frame_hdr_section`):
```c
/* enum calling convention */
struct EnumCCFunc **enum_cc_funcs;
int                nb_enum_cc_funcs;
int                mem_corruption_emitted;
```

### 2. `tcctok.h` — New tokens (after line 150, after `TOK_PURE2`)
```c
DEF(TOK_ENUM_CC1, "enum_callconv")
DEF(TOK_ENUM_CC2, "__enum_callconv__")
```

### 3. `tccgen.c`

**3a. `merge_funcattr`** (line 1211, inside the function body):
```c
if (fa1->enum_callconv)
    fa->enum_callconv = 1;
```

**3b. Attribute parser** — add after the `TOK_NORETURN1/2` case (~line 4044):
```c
case TOK_ENUM_CC1:
case TOK_ENUM_CC2:
    ad->f.enum_callconv = 1;
    break;
```

**3c. `enum_cc_finalize()`** — new function, add near end of tccgen.c:
```c
ST_FUNC void enum_cc_finalize(TCCState *s1)
{
    int i, n;
    for (i = 0; i < s1->nb_enum_cc_funcs; i++) {
        struct EnumCCFunc *ecf = s1->enum_cc_funcs[i];
        int max = ecf->nb_callsites;

        /* Allocate contiguous jump table in rodata_section */
        ecf->jmptable_offset = section_add(rodata_section, (addr_t)max * 8, 8);

        /* Patch leaq rel32: table_va - (leaq_insn_end_va) */
        addr_t leaq_end = text_section->sh_addr + ecf->leaq_rel32_offset + 4;
        addr_t table_va = rodata_section->sh_addr + ecf->jmptable_offset;
        write32le(text_section->data + ecf->leaq_rel32_offset,
                  (uint32_t)(int32_t)(table_va - leaq_end));

        /* Patch cmpq $MAX */
        write32le(text_section->data + ecf->cmpq_imm_offset, (uint32_t)max);

        /* Fill jump table entries: callsite_va - table_va */
        for (n = 0; n < max; n++) {
            addr_t cs_va = text_section->sh_addr + ecf->callsites[n].after_call_offset;
            write64le(rodata_section->data + ecf->jmptable_offset + n * 8,
                      (uint64_t)(int64_t)(cs_va - table_va));
        }
    }
}
```

### 4. `x86_64-gen.c` (SysV non-PE path only)

**4a. New static state** — add after existing statics (~line 159):
```c
static int             func_enum_callconv;
static struct EnumCCFunc *func_enum_cc;
static Sym            *mem_corruption_sym;
```

**4b. `emit_mem_corruption_stub()`** — new function before `gfunc_prolog`:
```c
static void emit_mem_corruption_stub(void)
{
    Section *sv_text = cur_text_section;
    addr_t   sv_ind  = ind;
    Sym     *fail_sym;
    CType    ft = { VT_FUNC, NULL };

    cur_text_section = text_section;
    ind = text_section->data_offset;

    mem_corruption_sym = global_identifier_push(
        tok_alloc_const("mem_corruption"), VT_FUNC, 0);
    put_extern_sym(mem_corruption_sym, text_section, ind, 0);

    fail_sym = external_helper_sym(tok_alloc_const("__stack_chk_fail"));
    greloca(text_section, fail_sym, ind + 1, R_X86_64_PLT32, -4);
    g(0xE9); gen_le32(0); /* jmp __stack_chk_fail */

    text_section->data_offset = ind;
    cur_text_section = sv_text;
    ind = sv_ind;
    tcc_state->mem_corruption_emitted = 1;
}
```

**4c. `gfunc_prolog` changes** (SysV path, line 1447):

At the top of the function, before `addr = PTR_SIZE * 2` and `loc = 0`:
```c
int is_enum_cc = func_sym->type.ref->f.enum_callconv;
func_enum_callconv = is_enum_cc;
func_enum_cc = NULL;

if (is_enum_cc && func_sym->v == TOK_main)
    tcc_error("main() cannot use enum_callconv");
if (is_enum_cc && func_var)
    tcc_error("variadic functions cannot use enum_callconv");
```

Replace `addr = PTR_SIZE * 2` (line 1457) with:
```c
if (is_enum_cc) {
    if (!tcc_state->mem_corruption_emitted)
        emit_mem_corruption_stub();
    g(0x41); g(0x5C);  /* popq %r12 */
    addr = PTR_SIZE;   /* no return address; first stack arg at rbp+8 */

    struct EnumCCFunc *ecf = tcc_mallocz(sizeof(*ecf));
    ecf->fn_sym = func_sym;
    dynarray_add(&tcc_state->enum_cc_funcs, &tcc_state->nb_enum_cc_funcs, ecf);
    func_enum_cc = ecf;
} else {
    addr = PTR_SIZE * 2;
}
```

After `ind += FUNC_PROLOG_SIZE` (line 1459), add:
```c
if (is_enum_cc) {
    /* movq %r12, -8(%rbp) = 4C 89 65 F8 */
    g(0x4C); g(0x89); g(0x65); g(0xF8);
    loc = -8;  /* reserve -8(%rbp) for ENUM; locals start at -16 */
}
```

**4d. `gfunc_epilog` changes** (SysV path, replace lines 1608–1615):
```c
if (func_enum_callconv) {
    /* reload ENUM: movq -8(%rbp), %r12 = 4C 8B 65 F8 */
    g(0x4C); g(0x8B); g(0x65); g(0xF8);
    /* manual leave: movq %rbp,%rsp = 48 89 EC */
    g(0x48); g(0x89); g(0xEC);
    /* popq %rbp = 5D */
    g(0x5D);
    /* cmpq $MAX, %r12 = 49 81 FC [imm32] */
    g(0x49); g(0x81); g(0xFC);
    func_enum_cc->cmpq_imm_offset = ind;
    gen_le32(0);
    /* jae mem_corruption = 0F 83 [rel32] */
    g(0x0F); g(0x83);
    greloca(cur_text_section, mem_corruption_sym, ind, R_X86_64_PC32, -4);
    gen_le32(0);
    /* leaq fn_jmptbl(%rip), %r11 = 4C 8D 1D [rel32] */
    g(0x4C); g(0x8D); g(0x1D);
    func_enum_cc->leaq_rel32_offset = ind;
    gen_le32(0);  /* patched in enum_cc_finalize */
    /* movq (%r11,%r12,8), %r10 = 4F 8B 14 E3 */
    g(0x4F); g(0x8B); g(0x14); g(0xE3);
    /* addq %r10, %r11 = 4D 01 D3 */
    g(0x4D); g(0x01); g(0xD3);
    /* jmpq *%r11 = 41 FF E3 */
    g(0x41); g(0xFF); g(0xE3);
} else {
    o(0xc9); /* leave */
    if (func_ret_sub == 0) {
        o(0xc3); /* ret */
    } else {
        o(0xc2); g(func_ret_sub); g(func_ret_sub >> 8);
    }
}
```

**4e. `gfunc_call` changes** (SysV path, replace the `gcall_or_jmp(0)` call at line 1433):
```c
    Sym *callee_sym = (vtop->r & VT_SYM) ? vtop->sym : NULL;
    int callee_ec = callee_sym
                 && callee_sym->type.ref
                 && callee_sym->type.ref->f.enum_callconv;

    if (callee_ec) {
        struct EnumCCFunc *ecf = NULL;
        for (int ei = 0; ei < tcc_state->nb_enum_cc_funcs; ei++) {
            if (tcc_state->enum_cc_funcs[ei]->fn_sym == callee_sym) {
                ecf = tcc_state->enum_cc_funcs[ei]; break;
            }
        }
        if (!ecf)
            tcc_error("enum_callconv: callee '%s' must be defined before caller",
                      get_tok_str(callee_sym->v, NULL));
        /* pushq $N = 68 [imm32] */
        g(0x68); gen_le32(ecf->nb_callsites);
        /* jmp fn */
        gcall_or_jmp(1);
        /* record after_call_N */
        unsigned long after_call = ind;
        /* if caller is also enum_callconv, reload %r12 (callee may have clobbered it) */
        if (func_enum_callconv) {
            g(0x4C); g(0x8B); g(0x65); g(0xF8); /* movq -8(%rbp), %r12 */
        }
        /* grow callsite array */
        if (ecf->nb_callsites >= ecf->cap_callsites) {
            ecf->cap_callsites = ecf->cap_callsites ? ecf->cap_callsites * 2 : 4;
            ecf->callsites = tcc_realloc(ecf->callsites,
                ecf->cap_callsites * sizeof(*ecf->callsites));
        }
        ecf->callsites[ecf->nb_callsites++].after_call_offset = after_call;
    } else {
        gcall_or_jmp(0);
    }
    if (args_size)
        gadd_sp(args_size);
    vtop--;
```

### 5. `tccmacho.c` — Finalization hook

Add forward declaration at top (after includes):
```c
ST_FUNC void enum_cc_finalize(TCCState *s1);
```

Add call after `relocate_syms` (after line 2208):
```c
        enum_cc_finalize(s1);
```

---

## Instruction Encoding Reference

| Instruction | Bytes |
|-------------|-------|
| `popq %r12` | `41 5C` |
| `movq %r12, -8(%rbp)` | `4C 89 65 F8` |
| `movq -8(%rbp), %r12` | `4C 8B 65 F8` |
| `movq %rbp, %rsp` | `48 89 EC` |
| `popq %rbp` | `5D` |
| `pushq $imm32` | `68 NN NN NN NN` |
| `cmpq $imm32, %r12` | `49 81 FC NN NN NN NN` |
| `jae rel32` | `0F 83 NN NN NN NN` |
| `leaq sym(%rip), %r11` | `4C 8D 1D NN NN NN NN` |
| `movq (%r11,%r12,8), %r10` | `4F 8B 14 E3` |
| `addq %r10, %r11` | `4D 01 D3` |
| `jmpq *%r11` | `41 FF E3` |
| `jmp rel32` | `E9 NN NN NN NN` |

---

## Test Program

```c
/* test_enum_cc.c */
#include <stdio.h>

static int __attribute__((enum_callconv)) add(int a, int b) { return a + b; }
static int __attribute__((enum_callconv)) mul(int a, int b) { return a * b; }
static int __attribute__((enum_callconv)) compute(int x, int y, int z) {
    int s = add(x, y);   /* call site 0 for add */
    int p = mul(s, z);   /* call site 0 for mul */
    return p;
}
int main(void) {
    int a = add(3, 4);        /* call site 1 for add */
    int b = mul(2, 5);        /* call site 1 for mul */
    int c = compute(1, 2, 3); /* call site 0 for compute */
    printf("add=%d mul=%d compute=%d\n", a, b, c);
    return !(a == 7 && b == 10 && c == 9);
}
```

Expected: `add=7 mul=10 compute=9`, exit 0.

---

## Verification

1. `git checkout -b feature/enum-cc`
2. `make` — rebuild tinycc
3. `./tcc -o test_enum_cc test_enum_cc.c`
4. `./test_enum_cc && echo PASS`
5. `otool -tv test_enum_cc | grep -A20 '_add\b'` — confirm no `call`/`ret`
6. `otool -s __TEXT __rodata test_enum_cc | xxd` — confirm jump table entries

---

## Known Limitations (POC)

- Callee must be defined before any caller (single-pass constraint)
- Variadic functions and `main` cannot use `enum_callconv` (enforced)
- Function-pointer use of enum_callconv functions is not detected/blocked
- Only x86_64 macOS SysV ABI; PE path unchanged
- Jump tables patched via direct memory write; object file output not supported
