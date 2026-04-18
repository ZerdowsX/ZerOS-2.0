; ZerDOS bootstrap (Multiboot v1)
BITS 32

MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
FLAGS    equ MBALIGN | MEMINFO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global start
extern kmain

start:
    cli
    mov esp, stack_top
    push ebx            ; Multiboot info
    push eax            ; Multiboot magic
    call kmain

.hang:
    hlt
    jmp .hang
