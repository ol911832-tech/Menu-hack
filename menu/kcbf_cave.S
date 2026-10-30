# Kuang exact-CBF code caves (x86-64, position independent). Intel syntax.
# Layout: data block (0x00..0x7f) then code. Jumps back into GD go through the ret* slots (absolute addresses).
.intel_syntax noprefix
.text
.globl blob_start
blob_start:
kMin:    .double 0.015625
kMax:    .double 0.984375
shrink:  .double 0.99999904632568359375
half:    .double 0.5
one_d:   .double 1.0
one_f:   .float 1.0
         .long 0
frac:    .double 0.5
ret0:    .quad 0
ret1:    .quad 0
ret2:    .quad 0
ret3:    .quad 0
cnt:     .quad 0
f2:      .double 0.0   # 0x60 cumulative fraction of the 2nd cut (0 = none)
cntm:    .quad 0       # 0x68 steps cut more than once
qidx:    .quad 0       # 0x70 next queued cut to hand to f2
qcnt:    .quad 0       # 0x78 number of queued extra cuts (max 4)
q:       .double 0,0,0,0,0,0,0,0   # 0x80 cumulative fractions of the extra cuts, ascending
t0p:     .double 0.0   # 0xc0 start of the current step in the clicks' clock (cave0 -> cave1)
         .quad 0       # 0xc8
twon:    .quad 1       # 0xd0 1 = exact cuts also with Time Warp != 1 (menu switch), 0 = the game's half cut
k1000:   .float 1000.0 # 0xd8
khalf:   .float 0.5    # 0xdc
.balign 16
.org 0x100

# ---- cave0: replaces 0x340a54..0x340a6f (lower bound of the split window) -------------------
# in: xmm1 = float step dt, xmm2 = 0.5, xmm14 = game-time length of a step, rsi = GJBaseGameLayer, [rsp+0x28] = step index
# out: xmm1 = window offset, xmm0 = dt, xmm8 = t0', xmm7 = t0'+dt  (t0' also saved in t0p)
# Time Warp != 1: the game's clock (m_stepTime) advances tw*dt per step but the clicks' clock runs in real time, so step i
# really starts at t0 + i*(dt - g) (g = xmm14, the game-time step). With tw == 1 this is exactly t0 as before.
.globl cave0
cave0:
    cvtps2pd xmm0, xmm1
    movaps   xmm1, xmm0
    movss    xmm3, dword ptr [rsi + 0x330]
    ucomiss  xmm3, dword ptr [rip + one_f]
    jne      1f
    jp       1f
    mulsd    xmm1, qword ptr [rip + kMin]
    movsd    xmm8, qword ptr [rsi + 0x38e8]
    jmp      2f
1:  cmp      qword ptr [rip + twon], 0
    je       4f
    movaps   xmm0, xmm14                         # Time Warp: the step's REAL length (m_stepTime advances by it);
    movaps   xmm1, xmm0                          # kmenu's processQueuedButtons hook scales its limit the same way
    mulsd    xmm1, qword ptr [rip + kMin]
    movsd    xmm8, qword ptr [rsi + 0x38e8]
    jmp      2f
4:  mulsd    xmm1, xmm2                          # the game's own half cut
    movsd    xmm8, qword ptr [rsi + 0x38e8]
2:  movsd    qword ptr [rip + t0p], xmm8
    movaps   xmm7, xmm8
    addsd    xmm7, xmm0
    jmp      qword ptr [rip + ret0]

# ---- cave1: replaces 0x340ac2..0x340afb (the split itself) -----------------------------------
# in: xmm6 = first click time in the window, xmm7 = end of step, xmm14/xmm15 + [rsp+0x68] + [rsp+0x38] = full step values,
#     rbx = entry of that click, rdi = end of the queue.  (rax/rcx/rdx/r8 and xmm0-5 are free: we just came back from a call)
# One cut per click: the first click makes the 1st cut (frac); every later click of the step that is >= 1/32 of a step after the
# previous cut is queued (up to 4) and becomes a middle pass in cave2. Time Warp != 1 works the same (t0' from cave0); with the
# Time Warp switch off it is the game's half cut and nothing is queued.
.globl cave1
cave1:
    mov      byte ptr [rsi + 0x38f0], 1
    xorps    xmm4, xmm4
    movsd    qword ptr [rip + f2], xmm4
    mov      qword ptr [rip + qcnt], 0
    mov      qword ptr [rip + qidx], 0
    movsd    xmm2, qword ptr [rip + half]
    movss    xmm3, dword ptr [rsi + 0x330]
    ucomiss  xmm3, dword ptr [rip + one_f]
    jne      9f
    jp       9f
    jmp      10f
9:  cmp      qword ptr [rip + twon], 0           # Time Warp != 1 and the switch is off: the game's half cut
    je       3f
10: movsd    xmm5, qword ptr [rip + t0p]         # t0' (start of this step in the clicks' clock)
    movaps   xmm3, xmm7
    subsd    xmm3, xmm5                          # step length
    movaps   xmm1, xmm6
    subsd    xmm1, xmm5
    divsd    xmm1, xmm3
    mulsd    xmm1, qword ptr [rip + shrink]
    maxsd    xmm1, qword ptr [rip + kMin]
    minsd    xmm1, qword ptr [rip + kMax]
    movaps   xmm2, xmm1                          # fraction of the 1st cut
    movaps   xmm4, xmm2                          # previous cut
    lea      rax, [rbx + 0x18]
    xor      edx, edx
    lea      r8, [rip + q]
5:  cmp      rax, rdi
    jae      6f
    movsd    xmm0, qword ptr [rax + 0x10]
    comisd   xmm7, xmm0
    jbe      6f
    movaps   xmm1, xmm0
    subsd    xmm1, xmm5
    divsd    xmm1, xmm3
    mulsd    xmm1, qword ptr [rip + shrink]
    maxsd    xmm1, qword ptr [rip + kMin]
    minsd    xmm1, qword ptr [rip + kMax]
    movaps   xmm0, xmm4
    addsd    xmm0, qword ptr [rip + kMin]
    comisd   xmm1, xmm0
    jb       7f
    movsd    qword ptr [r8 + rdx*8], xmm1
    inc      rdx
    movaps   xmm4, xmm1
    cmp      rdx, 4
    jae      6f
7:  add      rax, 0x18
    jmp      5b
6:  test     rdx, rdx
    jz       3f
    mov      qword ptr [rip + qcnt], rdx
    movsd    xmm0, qword ptr [r8]
    movsd    qword ptr [rip + f2], xmm0
    mov      qword ptr [rip + qidx], 1
    inc      qword ptr [rip + cntm]
3:  movsd    qword ptr [rip + frac], xmm2
    inc      qword ptr [rip + cnt]
    movaps   xmm10, xmm15
    mulsd    xmm10, xmm2
    movaps   xmm11, xmm14
    mulsd    xmm11, xmm2
    movsd    xmm13, qword ptr [rsp + 0x68]
    mulsd    xmm13, xmm2
    movsd    xmm9, qword ptr [rsp + 0x38]
    mulsd    xmm9, xmm2
    jmp      qword ptr [rip + ret1]

# ---- cave2: replaces 0x341803..0x341824 (end of a pass) ------------------------------------------
# first pass of a cut step (r13d == 0, f2 != 0): run a MIDDLE pass (r13d stays 0) of f2-frac, once per queued cut; the step counters the
# game bumps once per part (0x3e0, 0x3e8, 0x3f4, 0x3668) are lowered first so a step still adds the same total.
.globl cave2
cave2:
    test     r13d, r13d
    jnz      5f
    movsd    xmm2, qword ptr [rip + f2]
    xorps    xmm3, xmm3
    ucomisd  xmm2, xmm3
    je       5f
    movsd    xmm3, qword ptr [rip + frac]
    movsd    qword ptr [rip + frac], xmm2
    subsd    xmm2, xmm3
    mov      rcx, qword ptr [rip + qidx]         # rcx/rdi are overwritten by the game right after: free scratch
    cmp      rcx, qword ptr [rip + qcnt]
    jae      7f
    lea      rdi, [rip + q]
    movsd    xmm3, qword ptr [rdi + rcx*8]
    inc      rcx
    mov      qword ptr [rip + qidx], rcx
    jmp      8f
7:  xorps    xmm3, xmm3
8:  movsd    qword ptr [rip + f2], xmm3
    dec      dword ptr [rsi + 0x3e0]
    movss    xmm3, dword ptr [rsi + 0x330]       # the game adds roundf(tw*1000)*0.5 per part (500 with tw = 1)
    mulss    xmm3, dword ptr [rip + k1000]
    addss    xmm3, dword ptr [rip + khalf]
    cvttss2si eax, xmm3
    shr      eax, 1
    sub      dword ptr [rsi + 0x3f4], eax
    cmp      byte ptr [rsi + 0x334a], 0
    jne      6f
    dec      dword ptr [rsi + 0x3e8]
    cmp      byte ptr [rsi + 0x3670], 0
    jne      6f
    dec      dword ptr [rsi + 0x3668]
6:  mov      dword ptr [rsp + 0x50], r13d
    lea      rbx, [rsi + 0x3804]
    mov      ecx, dword ptr [rsp + 0x28]
    mov      edi, dword ptr [rsp + 0x24]
    movsd    xmm10, qword ptr [rbp - 0x80]
    mulsd    xmm10, xmm2
    movaps   xmm11, xmm14
    mulsd    xmm11, xmm2
    movsd    xmm13, qword ptr [rsp + 0x68]
    mulsd    xmm13, xmm2
    movsd    xmm9, qword ptr [rsp + 0x38]
    mulsd    xmm9, xmm2
    jmp      qword ptr [rip + ret3]
5:  inc      r13d
    mov      dword ptr [rsp + 0x50], r13d
    cmp      r13d, 2
    lea      rbx, [rsi + 0x3804]
    mov      ecx, dword ptr [rsp + 0x28]
    mov      edi, dword ptr [rsp + 0x24]
    jl       4f
    jmp      qword ptr [rip + ret2]
4:  movsd    xmm2, qword ptr [rip + one_d]
    subsd    xmm2, qword ptr [rip + frac]
    movsd    xmm10, qword ptr [rbp - 0x80]
    mulsd    xmm10, xmm2
    movaps   xmm11, xmm14
    mulsd    xmm11, xmm2
    movsd    xmm13, qword ptr [rsp + 0x68]
    mulsd    xmm13, xmm2
    movsd    xmm9, qword ptr [rsp + 0x38]
    mulsd    xmm9, xmm2
    jmp      qword ptr [rip + ret3]
.globl blob_end
blob_end:
