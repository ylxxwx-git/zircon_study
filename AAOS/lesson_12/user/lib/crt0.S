.section .text
.global _start
.global sys_write
.global sys_read
.global sys_exit
.global sys_yield

_start:
    bl main
    mov x0, #0
    bl sys_exit

sys_write:
    // x0 = buf, x1 = len
    // syscall expects: x0=fd, x1=buf, x2=len
    mov x2, x1
    mov x1, x0
    mov x0, #1      // fd = 1 (stdout)
    mov x8, #64     // sys_write
    svc #0
    ret

sys_read:
    // x0 = buf, x1 = len
    // syscall expects: x0=fd, x1=buf, x2=len
    mov x2, x1
    mov x1, x0
    mov x0, #0      // fd = 0 (stdin)
    mov x8, #63     // sys_read
    svc #0
    ret

sys_exit:
    mov x8, #93
    svc #0
    ret

sys_yield:
    mov x8, #124
    svc #0
    ret
