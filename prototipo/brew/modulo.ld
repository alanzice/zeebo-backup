ENTRY(AEEMod_Load)

SECTIONS {
    . = 0;
    .image : {
        KEEP(*(.text.brew_entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    .unexpected : {
        *(.data .data.* .bss .bss.* COMMON)
        *(.got .got.* .init_array .init_array.* .fini_array .fini_array.*)
    }
    ASSERT(SIZEOF(.unexpected) == 0, "Modulo nao admite dados globais gravaveis nem GOT")
    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .eh_frame* .comment .note*) }
}
