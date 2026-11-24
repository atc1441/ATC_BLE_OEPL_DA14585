MEMORY
{
    RAM (rwx) : ORIGIN = 0x07FD6000, LENGTH = 6K
}

ENTRY(flash_entry)

SECTIONS
{
    .text : {
        KEEP(*(.text.flash_entry))
        *(.text*)
        *(.rodata*)
    } > RAM
}
