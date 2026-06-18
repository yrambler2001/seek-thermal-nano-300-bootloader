/* lpc43xx_spifi_boot.ld — SeekProFF_43X0_Bootloader (reconstructed)
 *
 * First-stage SPIFI bootloader for LPC43xx. The image is executed in place
 * (XIP) from the memory-mapped external SPI-NOR at 0x14000000. Only the
 * bespoke SPIFI driver is relocated into on-chip SRAM at boot; the boot
 * logic itself runs from flash.
 *
 * Fixed addresses below are not free parameters — they must reproduce the
 * disassembly of Seek_pro_FF_LQ-XXX (build "Jun 26 2016" / "11:08:46").
 */
ENTRY(Reset_Handler)
MEMORY
{
  /* External SPI-NOR, memory-mapped XIP. Boot code runs in place here. */
  BOOT_FLASH (rx)  : ORIGIN = 0x14000000, LENGTH = 0x00010000   /* 64 KB window  */

  /* On-chip local SRAM bank0. The low 64 KB is RESERVED for the decrypted
   * application; the loader's relocated driver, .data and .bss sit just
   * above it, and the initial MSP tops the bank. */
  APP_RAM    (rwx) : ORIGIN = 0x10000000, LENGTH = 0x00010000   /* app load area */
  BOOT_RAM   (rwx) : ORIGIN = 0x10010000, LENGTH = 0x00008000   /* driver+data+bss+stack */
}

/* runtime anchors fixed by the design (match the disassembly) */
__driver_ram_start = 0x10010000;    /* relocated SPIFI driver image (VMA)  */
__stack_top        = 0x10018000;    /* initial MSP = vector[0]             */

SECTIONS
{
  /* ---- vector table (all-trap) + boot code, XIP from flash ---- */
  .isr_vector 0x14000000 :
  {
    KEEP(*(.isr_vector))
  } > BOOT_FLASH

  /* system / fault handlers immediately follow the vectors */
  .after_vectors :
  {
    *(.after_vectors*)
  } > BOOT_FLASH

  /* Linker-emitted scatter-load table at 0x14000240, walked by Reset_Handler.
   *   [0..2] .data    : { LMA, VMA, byte_len }   (copy)
   *   [3..4] .bss     : { VMA, byte_len }        (zero)
   *   [5..7] .text_ram: { LMA, VMA, byte_len }   (copy SPIFI driver to RAM) */
  .scatter_table 0x14000240 :
  {
    PROVIDE(__scatter_load_table = .);
    LONG(LOADADDR(.data));      LONG(ADDR(.data));      LONG(SIZEOF(.data));
    LONG(ADDR(.bss));           LONG(SIZEOF(.bss));
    LONG(LOADADDR(.text_ram));  LONG(ADDR(.text_ram));  LONG(SIZEOF(.text_ram));
  } > BOOT_FLASH

  .text :
  {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > BOOT_FLASH

  /* the three flash-resident thunks sit just before the driver blob */
  .spifi_thunks :
  {
    KEEP(*(.spifi_thunks))
    . = ALIGN(4);
  } > BOOT_FLASH

  /* ---- bespoke SPIFI driver blob: loaded from flash, runs from RAM ----
   * LMA 0x14000A28 -> VMA 0x10010000, length 0x1D70. Imported as-is; the
   * loader only enters it through the three thunks. */
  .text_ram __driver_ram_start : AT (0x14000A28)
  {
    __text_ram_start__ = .;
    KEEP(*(.spifi_driver_blob))
    . = ALIGN(4);
    __text_ram_end__ = .;
  } > BOOT_RAM

  /* ---- build-info marker @0x14002798: 4-byte marker + ASCII date/time ---- */
  .build_info 0x14002798 :
  {
    KEEP(*(.build_info))
  } > BOOT_FLASH

  /* ---- initialised data: LMA 0x140027BC -> VMA 0x10011D70, length 0x160 ----
   * Leading 0x13C bytes are the imported driver's mutable tables; the loader's
   * own cells follow at the tail:
   *   .boot_config_ptr  VMA 0x10011EAC (LMA 0x140028F8) = 0x14010000
   *   g_key_mask (KeyA) VMA 0x10011EB0 (LMA 0x140028FC)
   *   g_keyB     (KeyB) VMA 0x10011EC0 (LMA 0x1400290C)                      */
  .data 0x10011D70 : AT (0x140027BC)
  {
    __data_start__ = .;
    *(.spifi_driver_data)         /* imported driver .data (0x13C bytes)      */
    KEEP(*(.boot_config_ptr))     /* dword_10011EAC                           */
    KEEP(*(.keys))                /* g_key_mask then g_keyB                    */
    . = ALIGN(4);
    __data_end__ = .;
  } > BOOT_RAM

  /* ---- zero-init: VMA 0x10011ED0, length 0x94 ----
   * Holds the marshalled op-request struct (+0x00) and the driver context
   * handle (+0x14) used by flash_if.c / spifi_glue.c. */
  .bss 0x10011ED0 (NOLOAD) :
  {
    __bss_start__ = .;
    *(.spifi_driver_bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > BOOT_RAM

  PROVIDE(_estack = __stack_top);

  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}