ENTRY(entry)
SECTIONS { . = 0x20001000; .text : { KEEP(*(.entry)) *(.text*) *(.rodata*) } }
