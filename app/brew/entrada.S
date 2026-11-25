.syntax unified
.arm
.section .text.brew_entry,"ax",%progbits
.global AEEMod_Load
.type AEEMod_Load,%function
AEEMod_Load:
    adr r3, AEEMod_Load
    ldr r3, [r3, #-4]
    b brew_load
.size AEEMod_Load, .-AEEMod_Load
