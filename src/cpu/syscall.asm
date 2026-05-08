; YamKernel — SYSCALL entry stub
;
; CPU-set on entry:
;   RCX = user RIP        R11 = user RFLAGS
;   CS  = kernel code     SS  = kernel data
;   RSP = STILL user RSP — we must swap to kernel stack ourselves
;   GS_BASE = user — SWAPGS to get percpu
;
; YamKernel syscall ABI (caller side):
;   RAX = nr       RDI = a1   RSI = a2   RDX = a3   R10 = a4   R8 = a5
;   (R10 instead of RCX since CPU clobbers RCX with the user RIP.)
;
; C dispatcher signature (SysV AMD64):
;   i64 syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
;     -> RDI=nr, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5

bits 64
section .text
extern syscall_dispatch
global syscall_entry

syscall_entry:
    swapgs                          ; GS_BASE = kernel percpu
    mov   [gs:32], rsp              ; transient user RSP save while IRQs are masked
    mov   rsp, [gs:40]              ; percpu.kernel_rsp

    push  qword [gs:32]             ; user RSP belongs to this syscall frame
    push  rcx                       ; user RIP
    push  r11                       ; user RFLAGS
    push  rbx
    push  rbp
    push  r12
    push  r13
    push  r14
    push  r15
    push  rdi
    push  rsi
    push  rdx
    push  r8
    push  r9
    push  r10
    sub   rsp, 8                    ; keep SysV 16-byte alignment before call

    ; Publish syscall frame slot pointers for signal delivery (offsets match layout below).
    lea   rbx, [rsp+112]
    mov   [gs:56], rbx
    lea   rbx, [rsp+120]
    mov   [gs:64], rbx
    lea   rbx, [rsp+48]
    mov   [gs:72], rbx

    ; Shift caller args into SysV positions (right-to-left to avoid clobber):
    mov   r9,  r8                   ; arg6 = a5
    mov   r8,  r10                  ; arg5 = a4
    mov   rcx, rdx                  ; arg4 = a3
    mov   rdx, rsi                  ; arg3 = a2
    mov   rsi, rdi                  ; arg2 = a1
    mov   rdi, rax                  ; arg1 = nr

    ; Capture before STI: must match RSP after `call syscall_dispatch` returns (alignment slot).
    mov   [gs:80], rsp
    sti
    call  syscall_dispatch
syscall_exit_from_dispatch:
    cli                             ; rax = return value (fork: parent=child pid, child=0)
    add   rsp, 8

    pop   r10
    pop   r9
    pop   r8
    pop   rdx
    pop   rsi
    pop   rdi
    pop   r15
    pop   r14
    pop   r13
    pop   r12
    pop   rbp
    pop   rbx
    pop   r11                       ; user RFLAGS
    pop   rcx                       ; user RIP
    pop   qword [gs:32]             ; user RSP for this syscall frame

    mov   rsp, [gs:32]              ; restore user RSP
    swapgs                          ; GS_BASE = user
    o64 sysret

; Fork child: context_switch ret jumps here with RSP pointing at the resume word (duplicate of
; syscall_resume_rsp).  Join the normal syscall return path with RAX=0.
global fork_child_trampoline
fork_child_trampoline:
    pop   rsp
    xor   eax, eax
    jmp   syscall_exit_from_dispatch

section .note.GNU-stack noalloc noexec nowrite progbits
