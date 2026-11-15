# Code placed in the zero padding after the load commands of the x86_64
# slice of GeForceGLDriverWeb (334.01.03f11), at slice offset 0xd30 =
# address 0x123440000d30 (immediately after the last load command).  The
# first 6 bytes of the query-buffer-changed callback (0x123440309094:
# push rbp; mov rbp,rsp; push r15) are replaced with a jump to Lstart.
#
# Lstart runs the original callback (Lorig re-executes the replaced
# instructions and jumps back to 0x12344030909a), then rebases every
# per-slot query record (hw + 0x42d90, 0x30 bytes each, count at
# hw + 0x42d98) that still refers to an old report buffer onto the
# current one:
#   - pointers at +0x10, +0x18, +0x20 move by (current base - old base),
#     where current base = ctx + 0xd38 and old base = record[+0x10] minus
#     the record's offset in the array (slots and records are both 0x30
#     bytes apart)
#   - the mapping reference at +0x28 moves to the current buffer's mapping
#     ([ctx + 0x638] + 0x20): one reference is taken on the new mapping
#     (refcount at +4, unit 0x10000) and the old one is released with the
#     driver's release function at 0x1234402f711c.
#
# The exit sequence sits before the loop so that every branch to it is a
# short jump.
#
# Registers after the callback: rbx = start of the record array, r12 =
# current mapping, r13 = current base, r14 = record being examined, r15 =
# end of the record array.  Five pushes on entry (rbx, then r12-r15 after
# the call) keep rsp 16-byte aligned at both calls.
#
# Labels are assembler-local (L prefix) so every internal branch is
# resolved by the assembler.  The two external targets are left as
# placeholders (0x11111111, 0x22222222); build_patch.py replaces them with
# rel32 displacements computed from the code's fixed address.

    .text
Lstart:
    pushq %rbx
    movq %rdi, %rbx
    call Lorig
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq 0x638(%rbx), %rax
    testq %rax, %rax
    jz Ldone
    movq 0x20(%rax), %r12
    testq %r12, %r12
    jz Ldone
    movq 0xd38(%rbx), %r13
    testq %r13, %r13
    jz Ldone
    movq 0x1a28(%rbx), %rax
    movl 0x42d98(%rax), %r15d
    movq 0x42d90(%rax), %rbx
    imulq $0x30, %r15, %r15
    addq %rbx, %r15
    movq %rbx, %r14
    jmp Lloop
Ldone:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    ret
Lloop:
    cmpq %r15, %r14
    jae Ldone
    movq 0x28(%r14), %rdi
    cmpq %r12, %rdi
    je Lnext
    testq %rdi, %rdi
    jz Lnext
    movq 0x10(%r14), %rcx
    jrcxz Lnext
    subq %r14, %rcx
    addq %rbx, %rcx
    testl $0xfff, %ecx
    jnz Lnext
    movq %r13, %rax
    subq %rcx, %rax
    addq %rax, 0x10(%r14)
    cmpq $0, 0x18(%r14)
    je Lskip18
    addq %rax, 0x18(%r14)
Lskip18:
    cmpq $0, 0x20(%r14)
    je Lskip20
    addq %rax, 0x20(%r14)
Lskip20:
    lock addl $0x10000, 4(%r12)
    movq %r12, 0x28(%r14)
    .byte 0xe8
    .long 0x11111111                # rel32 to the release function, filled in by build_patch.py
Lnext:
    addq $0x30, %r14
    jmp Lloop
Lorig:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r15
    .byte 0xe9
    .long 0x22222222                # rel32 back into the callback, filled in by build_patch.py
