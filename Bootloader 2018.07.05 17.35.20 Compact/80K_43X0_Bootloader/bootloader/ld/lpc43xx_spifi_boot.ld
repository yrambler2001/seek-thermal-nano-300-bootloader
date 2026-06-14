/* lpc43xx_spifi_boot.ld — 80K_43X0_Bootloader / Compact Android CW (reconstructed)
 *
 * Memory map taken from the dump:
 *   FLASH image  : 0x14000000  external SPI-NOR, 64 KB used
 *   App window   : 0x10000000 .. 0x1001C000   (112 KB, written at run time)
 *   Boot runtime : 0x1001C000 .. 0x10020000   (16 KB: text_ram+data+bss+stack)
 *
 * This build drives the SPI-NOR in command mode (it never enters XIP), but the
 * flash is still placed at 0x14000000 in the address map and the boot code is
 * still relocated into SRAM before it runs.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH    (rx)  : ORIGIN = 0x14000000, LENGTH = 64K
    RAM_BOOT (rwx) : ORIGIN = 0x1001C000, LENGTH = 16K
    /* 0x10000000..0x1001C000 is the 112 KB region the decryptor fills with the
     * application; deliberately NOT an output region here.
     * The build name lineage still reads "80K"; this revision reserves 112 KB. */
}

__stack_top = 0x10020000;          /* initial MSP (== vector[0]) */

SECTIONS
{
    /* ================= FLASH-resident ================= */

    .vectors 0x14000000 : { KEEP(*(.vectors)) } > FLASH

    /* Reserved per-unit key slot — pinned at 0x14000218. */
    .dev_key 0x14000218 : { KEEP(*(.dev_key)) } > FLASH

    /* Reset/CRT0 + trap handlers + jump_to_ram_stage. Stays in flash because
     * it is what relocates everything below into SRAM. */
    .text_boot : ALIGN(4) {
        *(.text_boot) *(.text_boot.*)
    } > FLASH

    /* Scatter-load table walked by Reset_Handler (LPCXpresso "Global Section
     * Table": DATA triplets {LMA,VMA,len}, then BSS pairs {addr,len}). */
    .section_table : ALIGN(4) {
        __data_section_table = .;
            LONG(LOADADDR(.text_ram)); LONG(ADDR(.text_ram)); LONG(SIZEOF(.text_ram));
            LONG(LOADADDR(.data));     LONG(ADDR(.data));     LONG(SIZEOF(.data));
        __data_section_table_end = .;
        __bss_section_table = .;
            LONG(ADDR(.bss));          LONG(SIZEOF(.bss));
        __bss_section_table_end = .;
    } > FLASH

    /* Read-only constants stay in flash (e.g. the lpcspifilib device-name
     * strings; the descriptor STRUCTS that get list-linked at run time are
     * writable and therefore land in .data below). */
    .rodata : ALIGN(4) { *(.rodata .rodata.*) } > FLASH

    /* Build-identity block: a 4-byte marker (0x03020002) followed by the build
     * date/time. The info pointer handed to the application is 0x140019B8.
     * NOTE: there is no embedded build-name string in this build. */
    .rodata_flash : ALIGN(4) { *(.rodata_flash .rodata_flash.*) } > FLASH

    /* ================= Copied to SRAM, executed from SRAM ================= */

    /* boot_main + crypto + flash layer + board/timer code + lpcspifilib —
     * everything EXCEPT .text_boot. VMA starts at 0x1001C000; LMA trails the
     * section table in flash (0x140003B8 in the original). This build installs
     * NO RAM-resident interrupt handlers (every peripheral vector traps). */
    .text_ram : ALIGN(4) {
        __text_ram_start__ = .;
        *(.text_ram .text_ram.*)
        *(.text .text.*)               /* code + inline literal pools          */
        . = ALIGN(4);
        __text_ram_end__ = .;
    } > RAM_BOOT AT> FLASH

    /* Initialised data: g_boot_config_base, g_keyA, g_keyB, plus the
     * lpcspifilib device database (its nodes are linked into a list at run
     * time, so they must be writable). VMA == 0x1001D600 in the original. */
    .data : ALIGN(4) {
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM_BOOT AT> FLASH

    /* lpcspifilib scratch + device handle (0x1001DA40) + handle pointer
     * (0x1001DA94) + the board status/config and timer-calibration cells + BSS.
     * VMA == 0x1001DA40 in the original; the region ends at 0x1001DB9C. */
    .bss (NOLOAD) : ALIGN(4) {
        __bss_start__ = .;
        *(.spifi_bss .spifi_bss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM_BOOT

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}