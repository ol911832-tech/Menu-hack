# Kuang Precision Fix cave (x86-64, Intel syntax). Replaces the dt math of CCDirector::calculateDeltaTime (libcocos2d 2.2091, RVA 0xc1887..0xc18ba).
# in: rdi = CCDirector, rbx = "now" timeval (sec lo32 / usec hi32), [rsp+0x3c] = now.usec, rsp 16-aligned with 0x20 shadow. out: xmm0 = float dt, then jmp 0xc18ba
.intel_syntax noprefix
.text
.globl blob_start
blob_start:
pQPC:   .quad 0          # 0x00 QueryPerformanceCounter
freqd:  .double 1.0      # 0x08 QueryPerformanceFrequency as double
oned:   .double 1.0      # 0x10
e6f:    .float 1000000.0 # 0x18
        .long 0
ret:    .quad 0          # 0x20 -> 0xc18ba
lastq:  .quad 0          # 0x28
lasttv: .quad 0          # 0x30
cur:    .quad 0          # 0x38
cnt:    .quad 0          # 0x40 precise frames
fb:     .quad 0          # 0x48 fallback frames
.balign 16
.org 0x80
.globl cave
cave:
    lea  rcx, [rip + cur]
    call qword ptr [rip + pQPC]
    mov  rcx, qword ptr [rdi + 0x130]
    mov  rax, qword ptr [rcx]
    cmp  rax, qword ptr [rip + lasttv]
    jne  9f
    mov  rax, qword ptr [rip + cur]
    mov  rdx, qword ptr [rip + lastq]
    mov  qword ptr [rip + lastq], rax
    mov  qword ptr [rip + lasttv], rbx
    test rdx, rdx
    jz   8f
    sub  rax, rdx
    js   8f
    cvtsi2sd xmm0, rax
    divsd xmm0, qword ptr [rip + freqd]
    comisd xmm0, qword ptr [rip + oned]
    ja   8f
    cvtsd2ss xmm0, xmm0
    inc  qword ptr [rip + cnt]
    jmp  qword ptr [rip + ret]
9:  mov  rax, qword ptr [rip + cur]
    mov  qword ptr [rip + lastq], rax
    mov  qword ptr [rip + lasttv], rbx
8:  mov  eax, dword ptr [rsp + 0x3c]
    sub  eax, dword ptr [rcx + 4]
    movd xmm1, eax
    mov  eax, ebx
    sub  eax, dword ptr [rcx]
    cvtdq2ps xmm1, xmm1
    movd xmm0, eax
    divss xmm1, dword ptr [rip + e6f]
    cvtdq2ps xmm0, xmm0
    addss xmm1, xmm0
    xorps xmm0, xmm0
    maxss xmm0, xmm1
    inc  qword ptr [rip + fb]
    jmp  qword ptr [rip + ret]
.globl blob_end
blob_end:
