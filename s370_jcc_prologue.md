# S/370 MVS Assembler — JCC Save Area Chaining and Prologue

## Overview

This document explains the save area chaining and prologue convention used when
writing S/370 MVS assembler routines that are called from code compiled by the
JCC C compiler.

The prologue establishes a new **C stack frame**, hooks it into the JCC compiler's
linked chain of stack frames, and handles the case where the stack has run out of
pre-allocated space.  It is an adaptation between the standard MVS register save
area convention and the JCC compiler's dynamic C stack.

---

## Example Routine Entry

```asm
FTPSU    CSECT ,                start of program
         STM   R14,R12,12(R13)  save registers
         L     R2,8(,R13)       \
         LA    R14,96(,R2)       \
         L     R12,0(,R13)        \
         CL    R14,4(,R12)         \
         BL    F1-FTPSU+4(,R15)     \
         L     R10,0(,R12)           \ save area chaining
         BALR  R11,R10               / and JCC prologue
         CNOP  0,4                  /
F1       DS    0H                  /
         DC    F'96'              /
         STM   R12,R14,0(R2)     /
         LR    R13,R2           /
         LR    R12,R15          establish module addressability
         USING FTPSU,R12        tell assembler of base
```

---

## Background: What JCC Keeps in R13

In standard MVS assembler, R13 points to a static 18-fullword (72-byte) save area.
JCC uses a different model: R13 points to a **dynamic stack frame** with this
layout:

| Offset | Contents |
|---|---|
| 0  | Pointer to the **Stack Control Block (SCB)** — same value in every frame |
| 4  | **Back chain** — pointer to the calling frame |
| 8  | **Forward chain** — pointer to the next available stack space |
| 12–68 | Saved registers R14, R15, R0–R12 (standard MVS offsets) |
| 72–95 | Local C variables for this frame |

The SCB itself contains:

| Offset | Contents |
|---|---|
| 0 | Address of the **stack overflow extension routine** |
| 4 | Address of the **current top-of-stack limit** |

---

## Step-by-Step Walkthrough

### 1. Save Caller's Registers

```asm
STM   R14,R12,12(R13)
```

Standard MVS entry: save all registers (R14 wraps around to R12, covering all 15
general registers except R13) into the **caller's** frame at offset 12.  At this
point R13 still points to the caller's frame.

---

### 2. Load Pre-Allocated Frame Address

```asm
L     R2,8(,R13)
```

Load the **forward chain pointer** from offset 8 of the caller's frame.  This is
the pre-allocated address for **our** new 96-byte frame.  The caller's C prologue
already set this pointer up when it built its own frame.

---

### 3. Calculate End of Proposed Frame

```asm
LA    R14,96(,R2)
```

Calculate the **end address** of our proposed 96-byte frame (`frame_start + 96`).
This value will be compared against the stack limit to detect overflow.

---

### 4. Load the SCB Pointer

```asm
L     R12,0(,R13)
```

Load the **SCB pointer** from offset 0 of the caller's frame.  This pointer is
threaded through every frame in the chain so that any frame can reach the SCB.

---

### 5. Stack Overflow Check

```asm
CL    R14,4(,R12)
```

Compare the computed end-of-frame address against the stack limit stored in the SCB
at offset 4.  If our end address is below the limit, there is enough space; if not,
the stack has overflowed.

---

### 6. Branch if No Overflow

```asm
BL    F1-FTPSU+4(,R15)
```

**Branch if no overflow.**  The expression `F1-FTPSU+4` is a displacement from the
module entry point address held in R15 at this point in the routine.  The branch
target resolves to the `STM R12,R14,0(R2)` instruction at label F1+4, bypassing
the overflow handler entirely.

---

### 7. Stack Overflow Handler Call

```asm
L     R10,0(,R12)
BALR  R11,R10
CNOP  0,4
F1    DS    0H
      DC    F'96'
```

This block executes only when the stack is full.

- `L R10,0(,R12)` — load the address of the **stack extension routine** from the
  SCB at offset 0.
- `BALR R11,R10` — call the extension routine.  R11 receives the return address
  (the address of the instruction immediately after the BALR).
- `CNOP 0,4` — pad to a fullword boundary (2 NOP bytes after the 2-byte BALR
  instruction).
- `DC F'96'` — this fullword is **not executed**; it is a parameter embedded in
  the instruction stream immediately after the call.

The overflow routine reads the value `96` from the word at R11 (adjusted past the
CNOP padding) to determine how many bytes of stack to allocate.  It extends the
stack, updates R2 with the new frame address, and **returns past the DC** to the
`STM` instruction at F1+4.

> **Note:** Embedding a parameter immediately after a BALR and returning past it
> was a common IBM compiler idiom for passing size or type information to runtime
> helper routines without using a general-purpose register.

---

### 8. Chain the New Frame

```asm
STM   R12,R14,0(R2)
```

Both the no-overflow path (via branch) and the overflow path (via the extension
routine) arrive here.  Store three words into our new frame:

| Target offset | Register | Value stored |
|---|---|---|
| Frame + 0 | R12 | SCB pointer — threaded forward from the caller's frame |
| Frame + 4 | R13 | Back chain — address of the caller's frame |
| Frame + 8 | R14 | Forward chain — `R2+96`, next available stack space after our frame |

---

### 9. Switch R13 to Our Frame

```asm
LR    R13,R2
```

R13 now points to **our** frame.  The chain is complete.  Any routine we
subsequently call will find our forward pointer at offset 8 and use it as the base
address for its own frame.

---

## Chain Structure Diagram

```
SCB
┌─────────────────┐
│ overflow rtn    │ ◄── loaded into R10 on stack overflow
│ stack limit     │ ◄── compared against R2+96
└─────────────────┘
        ▲  (pointed to by offset 0 of every frame)
        │
caller's frame  ◄── R13 on entry to our routine
┌─────────────────┐
│ SCB ptr         │  offset  0
│ back chain      │  offset  4  (points to caller's caller)
│ fwd chain ──────┼──────────────────────────────────┐
│ saved regs      │  offsets 12–68                   │
│ locals          │  offsets 72–95                   │
└─────────────────┘                                  │
                                                     ▼
                                          our new frame  ◄── R13 on exit
                                          ┌─────────────────┐
                                          │ SCB ptr         │  offset  0
                                          │ back chain ─────┼──► caller's frame
                                          │ fwd chain ──────┼──► R2+96 (next free)
                                          │ saved regs      │  offsets 12–68
                                          │ locals          │  offsets 72–95
                                          └─────────────────┘
```

---

## Frame Size

The frame size in this example is **96 bytes**, broken down as:

| Region | Size |
|---|---|
| Standard MVS save area (18 fullwords) | 72 bytes |
| Local C variables and extra linkage data | 24 bytes |
| **Total** | **96 bytes** |

The size embedded in the `DC F'96'` instruction parameter must match the actual
frame size used throughout the prologue.  If local variable requirements change,
both the `LA R14,96(,R2)` displacement and the `DC` value must be updated to match.

---

## Standard Epilogue (Return to Caller)

```asm
RETURN   L     R13,4(,R13)      reload caller's frame address (back chain)
         L     R14,12(,R13)     restore R14 (return address)
         LM    R1,R12,24(R13)   restore R1 through R12
         BR    R14              return to caller
```

The epilogue follows the back chain at offset 4 to restore the caller's R13, then
reloads the saved registers from the caller's frame at their standard offsets.

---

## Register Usage Summary

| Register | Role during prologue |
|---|---|
| R1  | Parameter list pointer on entry; preserved in R11 after prologue |
| R2  | Address of our new stack frame |
| R10 | Address of the stack overflow extension routine (temporary) |
| R11 | Return address passed to overflow routine / parameter list after prologue |
| R12 | SCB pointer during prologue; module base register after `USING` |
| R13 | Caller's frame on entry; our frame on exit |
| R14 | End-of-frame address during overflow check; restored from save area at return |
| R15 | Module entry point address on entry (used for branch displacement calculation) |
