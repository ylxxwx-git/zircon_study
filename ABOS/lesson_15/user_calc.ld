ENTRY(_start)
SECTIONS
{
    . = 0x20000000;
    .text : {
        *(.text.entry)
        *(.text*)
    }
    .rodata : ALIGN(16) {
        *(.rodata*)
    }
    .data : ALIGN(16) {
        *(.data*)
    }
    .bss : ALIGN(16) {
        *(.bss*)
        *(COMMON)
    }
}
