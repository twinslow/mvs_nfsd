# S/370 MVS Assembler — JCC Save Area Chaining and Prologue

## Overview

This document explains the save area chaining and prologue convention used when
writing S/370 MVS assembler routines that are called from code compiled by the
JCC C compiler.

The prologue establishes a new **C stack frame**, hooks it into the JCC compiler's
linked chain of stack frames, and handles the case where the stack has run out of
pre-allocated space.  It is an adaptation between the standard MVS register save
area convention and the JCC compiler's dynamic C stack.

This information was originally pulled from ASMF source in the FTPD application as written by
*Jason Winter* (author/owner of JCC) and later modified by *Juergen Winkelmann*. A C langauge
test program was created, compiled with JCC, to generate ASM source. This was analyzed to
educate me, and independently confirm information presented here. The test C program is
documented later in this file.

### Assembler Macros

Two (stupidly simple) ASMF macros have been created to handle the entry and exit contracts
as required by JCC compiled code. These are --

* `JCCPROLG` - Stores registers in given stack frame and adjusts given (new) frame to required size. Links the new frame to the previous stack frame and initializes service routine address etc.
* `JCCRETRN` - Restores registers, with the exception of R15, which is assumed to hold the return value for the assembler routine.

---

## Example Routine Entry

```asm
* Copied from FTPSU routine, part of FTPD.
* By Juergen Winkelmann, ETH Zuerich.
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
*
         LR    R12,R15          establish module addressability
         USING FTPSU,R12        tell assembler of base
```

This same routine entry code can be seen as the entry code in JCC compiled
functions. The stack frame size being used for a C function with no local
variables is 84 bytes. The usage of byte offsets 72 through 83 has not been
determined.

The above has been implemented in a macro, `JCCPROLG`.
---

## Background: What JCC Keeps in R13

In standard MVS assembler, R13 points to a static 18-fullword (84 bytes) save area.
JCC uses a different model: R13 points to a **dynamic stack frame** with this layout
which is 84 bytes or more, allocated in a multiple of full words:

| Offset | Contents |
|--------|--------------------------------------------------------------------------|
| 0      | Pointer to the **Stack Control Block (SCB)** — same value in every frame |
| 4      | **Back chain** — pointer to the calling frame |
| 8      | **Forward chain** — pointer to the next available stack space |
| 12–68  | Saved registers R14, R15, R0–R12 (standard MVS offsets) |
| 72–83  | Reserved ... usage unknown |
| 84-    | **Local storage** for C variables, parameter lists etc. |

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
This value will be compared against the stack limit to detect overflow. In this
example an additional 12 bytes of local storage is being requested in the stack
frame `84 + 12 = 96`.

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
│ saved regs      │  offsets 12–71                   │
│ reserved        │  offsets 72–83                   │
│ locals          │  offsets 84–                     │
└─────────────────┘                                  │
                                                     ▼
                                          our new frame  ◄── R13 on exit
                                          ┌─────────────────┐
                                          │ SCB ptr         │  offset  0
                                          │ back chain ─────┼──► caller's frame
                                          │ fwd chain ──────┼──► R2+96 (next free)
                                          │ saved regs      │  offsets 12–71
                                          │ reserved        │  offsets 72–83
                                          │ locals          │  offsets 84–95
                                          └─────────────────┘
```

---

## Frame Size

The frame size in this example is **96 bytes**, broken down as:

| Region | Size |
|---|---|
| Standard MVS save area (18 fullwords)      | 72 bytes |
| 3 additional full words - reserved         | 12 bytes |
| Local C variables and extra linkage data   | 12 bytes |
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

The above has been implemented in a macro, `JCCRETRN`.
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

# Test C Program

```c
#include <stdio.h>
#include <string.h>

int func_nolocal(int in) {
    return in;
}

int func_nolocal2(int in1, int in2) {
    return in1 + in2;
}

char func_1char(int in1) {
    char   c;
    c = (char) in1 + 1;
    return c;
}

long long func_ll_nolocal(int in1) {
    return (long long)in1*32;
}

int func_1int(int in) {
    int loc1;
    loc1 = in*2;
    return loc1;
}

int func_4int(int in) {
    int loc[4];
    loc[0] = in * 2;
    loc[1] = in * 3;
    loc[2] = in * 4;
    loc[3] = in * 5;
    return loc[0] + loc[1] + loc[2] + loc[3];
}

int main(int argc, char **argv) {

    int        x;

    x = func_nolocal(1);
    printf("x=%d\n",x);

    x = func_nolocal2(2,2);
    printf("x=%d\n",x);

    x = func_1int(2);
    printf("x=%d\n",x);

    x = func_4int(2);
    printf("x=%d\n",x);

    return 0;
}
```


## JCC Compiler output source code --
```
00000000 *
00000000 * Compiled by JCC - version 1.50.00
00000000 *          on Wed Jul 22 11:02:19 2026
00000000 *
00000000 @CODE    ALIAS C'@SYSIN'
00000000 @CODE    CSECT
00000000 @CODE    AMODE ANY
00000000 @CODE    RMODE ANY
00000000 *
00000000 printf   ALIAS C'printf'
00000000          EXTRN printf
00000000 @CRT0    ALIAS C'@crt0'
00000000          EXTRN @CRT0
00000000 *
00000000 ***************
00000000 *
00000000 * ****
00000000 * *****         func_nolocal
00000000 * ****
00000000 *
00000000 ***************
00000000 func_nolocal ALIAS C'func_nolocal'
00000000          ENTRY func_nolocal
00000000 func_nolocal DS    0D
00000000 @REGION_1_1 DS    0H
00000000          STM   14,12,12(13)
00000004          L     2,8(0,13)
00000008          LA    14,84(0,2)
0000000C          L     12,0(0,13)
00000010          CL    14,4(0,12)
00000014          BL    @F1-@REGION_1_1+4(0,15)
00000018          L     10,0(0,12)
0000001C          BALR  11,10
0000001E          CNOP  0,4
00000020 @F1      DS    0H
00000020          DC    F'84'
00000024          STM   12,14,0(2)
00000028          LR    13,2
0000002A          LR    12,15
0000002C          USING @REGION_1_1,12
0000002C *
0000002C          LR    11,1
0000002E *
0000002E          L     10,@dnx_1_1
00000032          USING @dnx_1,10
00000032 *
00000032 *
00000032 * ***          //DDN:SYSIN:5 [func_nolocal]
00000032 *
00000032          L     15,0(0,11)
00000036 @lit_105 DS    0H
00000036          L     13,4(0,13)
0000003A          L     14,12(0,13)
0000003E          LM    1,12,24(13)
00000042          BR    14
00000044 *
00000044          DROP
00000044 *
00000044          DS    0E
00000044 @dnx_1_1 DC    A(@dnx_1)
00000048 ***************
00000048 *
00000048 * ****
00000048 * *****         func_nolocal2
00000048 * ****
00000048 *
00000048 ***************
00000048 func_nolocal2 ALIAS C'func_nolocal2'
00000048          ENTRY func_nolocal2
00000048 func_nolocal2 DS    0D
00000048 @REGION_2_1 DS    0H
00000048          STM   14,12,12(13)
0000004C          L     2,8(0,13)
00000050          LA    14,84(0,2)
00000054          L     12,0(0,13)
00000058          CL    14,4(0,12)
0000005C          BL    @F2-@REGION_2_1+4(0,15)
00000060          L     10,0(0,12)
00000064          BALR  11,10
00000066          CNOP  0,4
00000068 @F2      DS    0H
00000068          DC    F'84'
0000006C          STM   12,14,0(2)
00000070          LR    13,2
00000072          LR    12,15
00000074          USING @REGION_2_1,12
00000074 *
00000074          LR    11,1
00000076 *
00000076          L     10,@dnx_2_1
0000007A          USING @dnx_1,10
0000007A *
0000007A *
0000007A * ***          //DDN:SYSIN:9 [func_nolocal2]
0000007A *
0000007A          L     2,0(0,11)
0000007E          A     2,4(0,11)
00000082          LR    15,2
00000084 @lit_107 DS    0H
00000084          L     13,4(0,13)
00000088          L     14,12(0,13)
0000008C          LM    1,12,24(13)
00000090          BR    14
00000092 *
00000092          DROP
00000092 *
00000092          DS    0E
00000094 @dnx_2_1 DC    A(@dnx_1)
00000098 ***************
00000098 *
00000098 * ****
00000098 * *****         func_1char
00000098 * ****
00000098 *
00000098 ***************
00000098 func_1char ALIAS C'func_1char'
00000098          ENTRY func_1char
00000098 func_1char DS    0D
00000098 @REGION_3_1 DS    0H
00000098          STM   14,12,12(13)
0000009C          L     2,8(0,13)
000000A0          LA    14,88(0,2)
000000A4          L     12,0(0,13)
000000A8          CL    14,4(0,12)
000000AC          BL    @F3-@REGION_3_1+4(0,15)
000000B0          L     10,0(0,12)
000000B4          BALR  11,10
000000B6          CNOP  0,4
000000B8 @F3      DS    0H
000000B8          DC    F'88'
000000BC          STM   12,14,0(2)
000000C0          LR    13,2
000000C2          LR    12,15
000000C4          USING @REGION_3_1,12
000000C4 *
000000C4          LR    11,1
000000C6 *
000000C6          L     10,@dnx_3_1
000000CA          USING @dnx_1,10
000000CA *
000000CA *
000000CA * ***          //DDN:SYSIN:14 [func_1char]
000000CA *
000000CA          L     2,0(0,11)
000000CE          A     2,@cst_1
000000D2          STC   2,84(0,13)
000000D6 *
000000D6 * ***          //DDN:SYSIN:15 [func_1char]
000000D6 *
000000D6          XR    2,2
000000D8          ICM   2,1,84(13)
000000DC          LR    15,2
000000DE @lit_109 DS    0H
000000DE          L     13,4(0,13)
000000E2          L     14,12(0,13)
000000E6          LM    1,12,24(13)
000000EA          BR    14
000000EC *
000000EC          DROP
000000EC *
000000EC          DS    0E
000000EC @dnx_3_1 DC    A(@dnx_1)
000000F0 ***************
000000F0 *
000000F0 * ****
000000F0 * *****         func_ll_nolocal
000000F0 * ****
000000F0 *
000000F0 ***************
000000F0 func_ll_nolocal ALIAS C'func_ll_nolocal'
000000F0          ENTRY func_ll_nolocal
000000F0 func_ll_nolocal DS    0D
000000F0 @REGION_4_1 DS    0H
000000F0          STM   14,12,12(13)
000000F4          L     2,8(0,13)
000000F8          LA    14,84(0,2)
000000FC          L     12,0(0,13)
00000100          CL    14,4(0,12)
00000104          BL    @F4-@REGION_4_1+4(0,15)
00000108          L     10,0(0,12)
0000010C          BALR  11,10
0000010E          CNOP  0,4
00000110 @F4      DS    0H
00000110          DC    F'84'
00000114          STM   12,14,0(2)
00000118          LR    13,2
0000011A          LR    12,15
0000011C          USING @REGION_4_1,12
0000011C *
0000011C          LR    11,1
0000011E *
0000011E          L     10,@dnx_4_1
00000122          USING @dnx_1,10
00000122 *
00000122 *
00000122 * ***          //DDN:SYSIN:19 [func_ll_nolocal]
00000122 *
00000122          L     2,0(0,11)
00000126          SRDA  2,32(0)
0000012A          SLDL  2,5(0)
0000012E          LR    0,3
00000130          LR    15,2
00000132 @lit_111 DS    0H
00000132          L     13,4(0,13)
00000136          L     14,12(0,13)
0000013A          LM    1,12,24(13)
0000013E          BR    14
00000140 *
00000140          DROP
00000140 *
00000140          DS    0E
00000140 @dnx_4_1 DC    A(@dnx_1)
00000144 ***************
00000144 *
00000144 * ****
00000144 * *****         func_1int
00000144 * ****
00000144 *
00000144 ***************
00000144 func_1int ALIAS C'func_1int'
00000144          ENTRY func_1int
00000144 func_1int DS    0D
00000148 @REGION_5_1 DS    0H
00000148          STM   14,12,12(13)
0000014C          L     2,8(0,13)
00000150          LA    14,88(0,2)
00000154          L     12,0(0,13)
00000158          CL    14,4(0,12)
0000015C          BL    @F5-@REGION_5_1+4(0,15)
00000160          L     10,0(0,12)
00000164          BALR  11,10
00000166          CNOP  0,4
00000168 @F5      DS    0H
00000168          DC    F'88'
0000016C          STM   12,14,0(2)
00000170          LR    13,2
00000172          LR    12,15
00000174          USING @REGION_5_1,12
00000174 *
00000174          LR    11,1
00000176 *
00000176          L     10,@dnx_5_1
0000017A          USING @dnx_1,10
0000017A *
0000017A *
0000017A * ***          //DDN:SYSIN:24 [func_1int]
0000017A *
0000017A          L     2,0(0,11)
0000017E          SLL   2,1(0)
00000182          ST    2,84(0,13)
00000186 *
00000186 * ***          //DDN:SYSIN:25 [func_1int]
00000186 *
00000186          L     15,84(0,13)
0000018A @lit_113 DS    0H
0000018A          L     13,4(0,13)
0000018E          L     14,12(0,13)
00000192          LM    1,12,24(13)
00000196          BR    14
00000198 *
00000198          DROP
00000198 *
00000198          DS    0E
00000198 @dnx_5_1 DC    A(@dnx_1)
0000019C ***************
0000019C *
0000019C * ****
0000019C * *****         func_4int
0000019C * ****
0000019C *
0000019C ***************
0000019C func_4int ALIAS C'func_4int'
0000019C          ENTRY func_4int
0000019C func_4int DS    0D
000001A0 @REGION_6_1 DS    0H
000001A0          STM   14,12,12(13)
000001A4          L     2,8(0,13)
000001A8          LA    14,100(0,2)
000001AC          L     12,0(0,13)
000001B0          CL    14,4(0,12)
000001B4          BL    @F6-@REGION_6_1+4(0,15)
000001B8          L     10,0(0,12)
000001BC          BALR  11,10
000001BE          CNOP  0,4
000001C0 @F6      DS    0H
000001C0          DC    F'100'
000001C4          STM   12,14,0(2)
000001C8          LR    13,2
000001CA          LR    12,15
000001CC          USING @REGION_6_1,12
000001CC *
000001CC          LR    11,1
000001CE *
000001CE          L     10,@dnx_6_1
000001D2          USING @dnx_1,10
000001D2 *
000001D2 *
000001D2 * ***          //DDN:SYSIN:30 [func_4int]
000001D2 *
000001D2          L     2,0(0,11)
000001D6          SLL   2,1(0)
000001DA          ST    2,84(0,13)
000001DE *
000001DE * ***          //DDN:SYSIN:31 [func_4int]
000001DE *
000001DE          LA    2,3(0,0)
000001E2          L     3,0(0,11)
000001E6          LR    1,2
000001E8          MR    0,3
000001EA          ST    1,88(0,13)
000001EE *
000001EE * ***          //DDN:SYSIN:32 [func_4int]
000001EE *
000001EE          L     2,0(0,11)
000001F2          SLL   2,2(0)
000001F6          ST    2,92(0,13)
000001FA *
000001FA * ***          //DDN:SYSIN:33 [func_4int]
000001FA *
000001FA          LA    2,5(0,0)
000001FE          L     3,0(0,11)
00000202          LR    1,2
00000204          MR    0,3
00000206          ST    1,96(0,13)
0000020A *
0000020A * ***          //DDN:SYSIN:34 [func_4int]
0000020A *
0000020A          L     2,84(0,13)
0000020E          A     2,88(0,13)
00000212          A     2,92(0,13)
00000216          A     2,96(0,13)
0000021A          LR    15,2
0000021C @lit_115 DS    0H
0000021C          L     13,4(0,13)
00000220          L     14,12(0,13)
00000224          LM    1,12,24(13)
00000228          BR    14
0000022A *
0000022A          DROP
0000022A *
0000022A          DS    0E
0000022C @dnx_6_1 DC    A(@dnx_1)
00000230 ***************
00000230 *
00000230 * ****
00000230 * *****         main
00000230 * ****
00000230 *
00000230 ***************
00000230 main     ALIAS C'main'
00000230          ENTRY main
00000230 main     DS    0D
00000230 @REGION_7_1 DS    0H
00000230          STM   14,12,12(13)
00000234          L     2,8(0,13)
00000238          LA    14,116(0,2)
0000023C          L     12,0(0,13)
00000240          CL    14,4(0,12)
00000244          BL    @F7-@REGION_7_1+4(0,15)
00000248          L     10,0(0,12)
0000024C          BALR  11,10
0000024E          CNOP  0,4
00000250 @F7      DS    0H
00000250          DC    F'116'
00000254          STM   12,14,0(2)
00000258          LR    13,2
0000025A          LR    12,15
0000025C          USING @REGION_7_1,12
0000025C *
0000025C          LR    11,1
0000025E *
0000025E          L     10,@dnx_7_1
00000262          USING @dnx_1,10
00000262 *
00000262 *
00000262 * ***          //DDN:SYSIN:41 [main]
00000262 *
00000262          LA    2,1(0,0)
00000266          ST    2,108(0,13)
0000026A          L     15,@ext_104
0000026E          LA    1,108(0,13)
00000272          BALR  14,15
00000274          LR    4,15
00000276 *
00000276 * ***          //DDN:SYSIN:42 [main]
00000276 *
00000276          LA    2,@lit_124
0000027A          ST    2,108(0,13)
0000027E          ST    4,112(0,13)
00000282          L     15,@ext_59
00000286          LA    1,108(0,13)
0000028A          BALR  14,15
0000028C *
0000028C * ***          //DDN:SYSIN:44 [main]
0000028C *
0000028C          LA    5,2(0,0)
00000290          ST    5,108(0,13)
00000294          ST    5,112(0,13)
00000298          L     15,@ext_106
0000029C          LA    1,108(0,13)
000002A0          BALR  14,15
000002A2          LR    4,15
000002A4 *
000002A4 * ***          //DDN:SYSIN:45 [main]
000002A4 *
000002A4          LA    2,@lit_124
000002A8          ST    2,108(0,13)
000002AC          ST    4,112(0,13)
000002B0          L     15,@ext_59
000002B4          LA    1,108(0,13)
000002B8          BALR  14,15
000002BA *
000002BA * ***          //DDN:SYSIN:47 [main]
000002BA *
000002BA          LA    2,2(0,0)
000002BE          ST    2,108(0,13)
000002C2          L     15,@ext_112
000002C6          LA    1,108(0,13)
000002CA          BALR  14,15
000002CC          LR    4,15
000002CE *
000002CE * ***          //DDN:SYSIN:48 [main]
000002CE *
000002CE          LA    2,@lit_124
000002D2          ST    2,108(0,13)
000002D6          ST    4,112(0,13)
000002DA          L     15,@ext_59
000002DE          LA    1,108(0,13)
000002E2          BALR  14,15
000002E4 *
000002E4 * ***          //DDN:SYSIN:50 [main]
000002E4 *
000002E4          LA    2,2(0,0)
000002E8          ST    2,108(0,13)
000002EC          L     15,@ext_114
000002F0          LA    1,108(0,13)
000002F4          BALR  14,15
000002F6          LR    4,15
000002F8 *
000002F8 * ***          //DDN:SYSIN:51 [main]
000002F8 *
000002F8          LA    2,@lit_124
000002FC          ST    2,108(0,13)
00000300          ST    4,112(0,13)
00000304          L     15,@ext_59
00000308          LA    1,108(0,13)
0000030C          BALR  14,15
0000030E *
0000030E * ***          //DDN:SYSIN:53 [main]
0000030E *
0000030E          XR    15,15
00000310 @lit_123 DS    0H
00000310          L     13,4(0,13)
00000314          L     14,12(0,13)
00000318          LM    1,12,24(13)
0000031C          BR    14
0000031E *
0000031E          DROP
0000031E *
0000031E          DS    0E
00000320 @dnx_7_1 DC    A(@dnx_1)
00000324 @dnx_1   DS    0E
00000324 *
00000324 @ext_59  DC    V(printf)
00000328 @ext_104 DC    A(func_nolocal)
0000032C @ext_106 DC    A(func_nolocal2)
00000330 @ext_112 DC    A(func_1int)
00000334 @ext_114 DC    A(func_4int)
00000338 @cst_0   DC    X'00000000'
0000033C @cst_1   DC    X'00000001'
00000340 @cst_2   DC    X'00000002'
00000344 @cst_3   DC    X'00000003'
00000348 @cst_4   DC    X'00000004'
0000034C @cst_5   DC    X'00000005'
00000350 @lit_124 DS    0E
00000350          DC    X'A77E6C841500'      x=%d..
00000356          DC    2X'00'
00000358 *
00000358          END
```



