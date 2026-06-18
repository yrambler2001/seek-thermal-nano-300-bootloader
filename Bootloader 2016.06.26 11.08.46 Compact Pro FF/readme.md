# Seek Thermal Compact Pro (FF) Bootloader

**A reverse-engineered and reconstructed first-stage bootloader for the Seek Thermal Compact Pro (FF variant, product code LQ-XXX), built on NXP LPC43xx (ARM Cortex-M4) and running from external SPIFI flash.**

This repository is a from-scratch C reconstruction of the Compact Pro (FF) LPC43xx boot image, recovered by disassembling a 4 MB SPI-NOR flash dump. The reconstruction is **strictly faithful to the original machine code** — the disassembly is the source of truth, and where the natural "developer-style" idealization would diverge from the binary, the binary wins. The dump contains no function symbols; the labels here were assigned manually during analysis from behavior and call context, then kept consistent so the source lines up 1:1 with the listing.

---

## Table of contents

1. [What this is](#1-what-this-is)
2. [Security notice (read first)](#2-security-notice-read-first)
3. [At a glance](#3-at-a-glance)
4. [Target hardware](#4-target-hardware)
5. [Repository layout](#5-repository-layout)
6. [What is reconstructed vs. imported](#6-what-is-reconstructed-vs-imported)
7. [Manually assigned function labels](#7-manually-assigned-function-labels)
8. [Build prerequisites](#8-build-prerequisites)
9. [Building in MCUXpresso](#9-building-in-mcuxpresso)
10. [Memory map](#10-memory-map)
11. [The build identity](#11-the-build-identity)
12. [Boot flow, end to end](#12-boot-flow-end-to-end)
13. [Image format](#13-image-format)
14. [The slot system](#14-the-slot-system)
15. [Cryptographic scheme](#15-cryptographic-scheme)
16. [Keys and where they live](#16-keys-and-where-they-live)
17. [Module reference](#17-module-reference)
18. [Configuration constants](#18-configuration-constants)
19. [Naming conventions and known misnomers](#19-naming-conventions-and-known-misnomers)
20. [Reconstruction caveats (load-bearing assumptions)](#20-reconstruction-caveats-load-bearing-assumptions)
21. [Intentional oddities preserved from the binary](#21-intentional-oddities-preserved-from-the-binary)
22. [Verifying the reconstruction](#22-verifying-the-reconstruction)
23. [Security analysis](#23-security-analysis)
24. [Relationship to sibling LPC43xx builds](#24-relationship-to-sibling-lpc43xx-builds)
25. [Known issues / TODO](#25-known-issues--todo)
26. [Glossary](#26-glossary)
27. [References](#27-references)

---

## 1. What this is

This is the **first-stage boot image** for the Compact Pro (FF) LPC43xx part. It lives in external SPIFI flash mapped at `0x14000000` and its job is narrow:

1. Bring up the core and the SPI-NOR flash controller.
2. Migrate any firmware slot still in transport (Key-A) form to the device-storage (Key-B) form.
3. Pick one of three firmware "slots" in flash, based on a boot-config record (and an optional warm-boot rollback request).
4. Verify, decrypt, and segment-load the chosen firmware into internal SRAM, then jump to it.

The application image is protected with a **software xorshift128 stream cipher** keyed by a 128-bit value baked into the bootloader's own flash image. It is **not** authenticated encryption — there is no signature or MAC, only a 32-bit additive checksum. (See [§23](#23-security-analysis).)

This build runs its **entire boot pipeline from flash (XIP)** rather than relocating to SRAM, and carries a **bespoke hand-written SPIFI driver** (not NXP lpcspifilib) which is the only thing relocated into RAM.

### What this is _not_

- It is **not** a clean-room rewrite. It is a behavioral reconstruction traced against the actual disassembly, byte-for-byte faithful on the crypto path and structurally faithful elsewhere. Where a tidy CRT0 would add guards the silicon does not run, this reconstruction omits them to match the binary.
- It is **not** a turnkey, flashable artifact. The imported SPIFI driver blob is treated as a black box (see [§6](#6-what-is-reconstructed-vs-imported), [§20](#20-reconstruction-caveats-load-bearing-assumptions)).
- It is **not** using the LPC43**S** hardware AES engine or OTP. The application is protected purely in software.

---

## 2. Security notice (read first)

> **The encryption keys in this repository are recovered constants, not secrets.**
>
> Both 128-bit keys are embedded in clear in the original flash image. Anyone with the dump already has them. They are reproduced here ([§16](#16-keys-and-where-they-live)) because they are required to understand and reproduce the loader's behavior, **not** because they protect anything.
>
> **The protection scheme is obfuscation, not security.** A non-linear PRNG keyed by fixed values, plus a 32-bit additive checksum, provides no authenticity guarantee. xorshift128 is fully invertible; a known-plaintext attack on the cleartext 64-byte header window (always present at image offset `0x200`) plus the predictable Cortex-M vector-table structure recovers keystream trivially. The two-key Key-A→Key-B migration is a key-management convenience, not a strengthening. Do **not** reuse this design where tamper-resistance matters. See [§23](#23-security-analysis).

---

## 3. At a glance

| Property               | Value                                                                              |
| ---------------------- | ---------------------------------------------------------------------------------- |
| Product                | Seek Thermal Compact Pro (FF variant), product code LQ-XXX                         |
| Internal build name    | _none embedded_ (marker `{01,02,00,00}` + build date/time only)                    |
| Build timestamp        | `Jun 26 2016  11:08:46` (at flash `0x14002798`)                                    |
| Image source           | 4 MB (32 Mbit) SPI-NOR dump (4,194,304 bytes); bootloader region is the low 64 KB  |
| Image SHA-256          | `195f53383fd3bdd33dd867aa53df9350d01d035b50eeed338b4b56c2e447cdbe`                 |
| Target                 | NXP LPC43xx, ARM Cortex-M4 (ARMv7-M), little-endian                                |
| Mapped base            | `0x14000000` (SPIFI memory-mapped external SPI-NOR)                                |
| Initial SP (MSP)       | `0x10018000`                                                                       |
| Reset vector           | `0x14000263` (Thumb), i.e. `Reset_Handler` at `0x14000262`                         |
| Application region     | `0x10000000`–`0x10010000` (64 KB)                                                  |
| Bootloader RAM runtime | `0x10010000`–`0x10018000` (32 KB: imported driver + data + bss + stack)            |
| Cipher                 | software xorshift128 (Marsaglia), shift triple 11/19/8                             |
| Key whitening          | **none** (seeds taken from key words directly)                                     |
| Acceptance test        | 32-bit decrypted-word checksum `== 0x00000000`                                     |
| Keys                   | two embedded (Key A = transport, Key B = at-rest) + optional silicon-derived Key B |
| Language               | C (GNU C99/C11)                                                                    |

For the specific dump this was reconstructed against, the boot decision resolves to: **recovery slot (`0x14070000`), Key B, segmented path, installed MSP `0x1000A000`, jump `0x100806A5`** — confirmed bit-exact in [§22](#22-verifying-the-reconstruction).

---

## 4. Target hardware

- **Product:** Seek Thermal Compact Pro (FF variant), product code LQ-XXX.
- **MCU:** NXP LPC43xx family (LPC4337-class). The loader places a 64 KB application at `0x10000000` plus its own 32 KB runtime above it, so the part has at least 96 KB of contiguous SRAM at `0x10000000` (the staging buffer at `0x20000000` is a separate AHB bank). The installed application additionally uses the second SRAM bank around `0x10080000` and AHB SRAM at `0x18000000`.
- **Core:** ARM Cortex-M4, ARMv7-M, little-endian, Thumb-2.
- **Boot source:** external SPI-NOR over the SPIFI controller (peripheral base `0x40003000`), memory-mapped (XIP) at `0x14000000`.
- **Flash device:** a 4 MB (32 Mbit) SPI-NOR part. The bespoke driver detects the device by JEDEC ID and carries per-vendor configuration paths (Spansion/Macronix/Winbond/ISSI/GigaDevice/SST/Micron — see the `spifi_cfg_*` routines in [§7](#7-manually-assigned-function-labels)).
- **SRAM regions used:** the low bank from `0x10000000` (application + bootloader runtime), the second bank at `0x10080000`, AHB SRAM at `0x18000000`, and an AHB SRAM staging buffer at `0x20000000`.

---

## 5. Repository layout

```
SeekProFF_43X0_Bootloader/                   ← workspace
│
├─ lpc_chip_43xx/                             ← LPCOpen chip library      [IMPORT, unchanged]
│  ├─ inc/   chip.h, cmsis.h, core_cm4.h,
│  │         scu_18xx_43xx.h, rgu_18xx_43xx.h, creg_18xx_43xx.h, …
│  └─ src/   (CMSIS + the few peripheral TUs actually linked)
│
├─ spifi_driver_blob/                         ← bespoke SPIFI driver      [IMPORT, black box]
│  └─ (relocated code+data blob; flash 0x14000A28 → RAM 0x10010000, 0x1D70 bytes;
│      ~40 spifi_* / memcpy_fast routines; NOT NXP lpcspifilib)
│
└─ bootloader/                                ← THE APPLICATION           [RECONSTRUCT]
   ├─ inc/
   │   └─ bootloader.h          ← memory map, magic numbers, slot table, request ABI, prototypes
   ├─ src/
   │   ├─ startup_lpc43xx.c     ← g_pfnVectors[], the full monolithic Reset_Handler
   │   │                          (minimal CRT0 + mailbox + spifi_init + A→B migration + slot
   │   │                          select + segmented load + handoff), spin fault handlers,
   │   │                          IRQ52_Handler catch-all, scatterload_copy_words
   │   ├─ boot_main.c           ← select_boot_slot, image_try_keys / _copy2 (fused checksum),
   │   │                          memzero_words
   │   ├─ crypto_stream.c       ← xorshift128_next (standalone), prng_seed_from_key,
   │   │                          stream_checksum16 / _copy2, stream_reencrypt_keyA_to_keyB,
   │   │                          stream_decrypt_skip_header, stream_decrypt_segment
   │   ├─ flash_if.c            ← spifi_init (request-first), flash_program (4-arg, no memcpy),
   │   │                          flash_erase_region, flash_program_rmw, flash_chip_erase
   │   ├─ spifi_glue.c          ← imported-driver request ABI + context + the three thunks
   │   ├─ keys.c                ← g_key_mask (Key A), g_keyB, g_boot_config_ptr, g_build_info
   │   └─ util_mem.c            ← memcpy_auto, memcpy_bytes
   └─ ld/
       └─ lpc43xx_spifi_boot.ld ← 64 KB image @0x14000000; driver/.data/.bss relocated to
                                   0x10010000+; stack top 0x10018000
```

The on-disk folder for this reconstruction is kept as `80K_43X0_Bootloader/` to mirror the sibling-lineage skeleton; the descriptive workspace name above (`SeekProFF_43X0_Bootloader`) is anchored to the product code LQ-XXX and the build date.

---

## 6. What is reconstructed vs. imported

The current IDA database defines exactly **72 functions**. The source reconstruction covers the 33 bootloader/control-surface routines up to and including the three RAM-driver thunks. The 39 routines from `memcpy_fast` onward are the imported SPIFI driver blob. The address-level inventory lives in [§7](#7-manually-assigned-function-labels).

- **Reconstructed/source-level routines (33):** `IRQ52_Handler`, `Reset_Handler`, the core fault/system handlers, `scatterload_copy_words`, `memzero_words`, `image_try_keys`, `image_try_keys_copy2`, `spifi_init`, `flash_program`, `flash_erase_region`, `flash_program_rmw`, `flash_chip_erase`, `select_boot_slot`, `memcpy_auto`, `memcpy_bytes`, `prng_seed_from_key`, `xorshift128_next`, `stream_checksum16`, `stream_reencrypt_keyA_to_keyB`, `stream_decrypt_skip_header`, `stream_decrypt_segment`, `stream_checksum16_copy2`, `spifi_drv_init_thunk`, `spifi_drv_program_thunk`, and `spifi_drv_op_thunk`.
- **Imported SPIFI driver blob routines (39):** `memcpy_fast`, `spifi_cmd_read`, `spifi_make_cmd`, `spifi_quad_opt_bits`, `spifi_cmd`, `spifi_cmd_addr`, `spifi_cmd_data`, `spifi_write_enable`, `spifi_wren_then_cmd`, `spifi_cmd_addr_data`, `spifi_exit_mem_mode`, `spifi_wait_program_done`, `spifi_write_status`, `spifi_enter_mem_mode`, `spifi_detect_4byte_addr`, `spifi_sum_sfdp_params`, `spifi_get_read_count`, `spifi_read_sfdp`, `spifi_set_capacity`, `spifi_configure_modes`, `spifi_block_protect_engine`, `spifi_check_range`, `spifi_erase_cmd`, `spifi_program_setup`, `spifi_program_pages`, `spifi_find_nonff_word`, `spifi_addr_aligned`, `spifi_verify_equal`, `spifi_verify_erased`, `spifi_program_region`, `spifi_cfg_micron`, `spifi_dummy_from_freq_a`, `spifi_cfg_winbond_spansion`, `spifi_cfg_read_cr`, `spifi_cfg_micron_2`, `spifi_cfg_sst`, `spifi_dummy_from_freq_b`, `spifi_cfg_macronix`, and `memcpy_bytes_thunk`.

The imported driver is **not** NXP lpcspifilib: it pokes the SPIFI registers directly and switches on the JEDEC ID per vendor. It is treated as a black box: a relocated blob that gives the reconstructed bootloader read/program/erase access to the external flash. The blob itself is byte-identical to the sibling lineage; only its load address differs.

A practical consequence: **the bootloader-specific logic is the entire attack/patch surface.** Everything behind the three thunks is vendor plumbing.

---

## 7. Manually assigned function labels

The dump contains no function symbols. The labels below were assigned manually during analysis from each routine's behavior and call context, then kept consistent so the source compares directly with the listing. Addresses are flash/image addresses; the relocated driver routines map into RAM at `0x10010000` (see [§10](#10-memory-map)). This table lists every function currently defined in the IDA database.

| Address      | Function label                  | Notes                                                    |
| ------------ | ------------------------------- | -------------------------------------------------------- |
| `0x14000260` | `IRQ52_Handler`                 | shared trap for every peripheral IRQ                     |
| `0x14000262` | `Reset_Handler`                 | minimal CRT0 + the whole boot pipeline                   |
| `0x140004D0` | `NMI_Handler`                   | plain spin                                               |
| `0x140004D2` | `HardFault_Handler`             | plain spin                                               |
| `0x140004D4` | `MemManage_Handler`             | plain spin                                               |
| `0x140004D6` | `BusFault_Handler`              | plain spin                                               |
| `0x140004D8` | `UsageFault_Handler`            | plain spin                                               |
| `0x140004DA` | `SVC_Handler`                   | plain spin                                               |
| `0x140004DC` | `DebugMon_Handler`              | plain spin                                               |
| `0x140004DE` | `PendSV_Handler`                | plain spin                                               |
| `0x140004E0` | `SysTick_Handler`               | plain spin                                               |
| `0x140004E6` | `scatterload_copy_words`        | CRT0 word copy (src, dst, len)                           |
| `0x140004FC` | `memzero_words`                 | CRT0 / BSS-descriptor zero                               |
| `0x1400050E` | `image_try_keys`                | fused: magic + len + Key-A checksum                      |
| `0x14000548` | `image_try_keys_copy2`          | fused: magic + len + Key-B checksum                      |
| `0x14000588` | `spifi_init`                    | request-struct init **first**, then pin-mux, then driver |
| `0x140005DC` | `flash_program`                 | 4-arg, sentinel 0, no memcpy, `src` vestigial            |
| `0x14000614` | `flash_erase_region`            | stage_buf `0x20000000`, opcode `0x20`                    |
| `0x14000644` | `flash_program_rmw`             | erase-then-program (NOT block RMW)                       |
| `0x14000668` | `flash_chip_erase`              | erases ONE 64K block (fixed `0x10000`)                   |
| `0x14000698` | `select_boot_slot`              | reads selector directly; recursive rollback hook         |
| `0x14000740` | `memcpy_auto`                   | word copy + byte tail                                    |
| `0x14000774` | `memcpy_bytes`                  | byte copy; target of driver's `memcpy_bytes_thunk`       |
| `0x14000788` | `prng_seed_from_key`            | shared Key-B / silicon seeder                            |
| `0x140007F4` | `xorshift128_next`              | **standalone, shared** PRNG step (7 call sites)          |
| `0x14000818` | `stream_checksum16`             | direct-XIP keystream checksum, Key A                     |
| `0x1400087C` | `stream_reencrypt_keyA_to_keyB` | dual-key A->B migration transform (see rename note)      |
| `0x140008F0` | `stream_decrypt_skip_header`    | **single** Key-B skip-header decryptor (3-arg)           |
| `0x14000938` | `stream_decrypt_segment`        | positional Key-B segment decrypt (4-arg)                 |
| `0x140009A2` | `stream_checksum16_copy2`       | direct-XIP keystream checksum, Key B                     |
| `0x140009F8` | `spifi_drv_init_thunk`          | -> RAM `0x100105FF`                                      |
| `0x14000A08` | `spifi_drv_program_thunk`       | -> RAM `0x10010E95`                                      |
| `0x14000A18` | `spifi_drv_op_thunk`            | -> RAM `0x100110EB`                                      |
| `0x14000A28` | `memcpy_fast`                   | start of the imported driver blob                        |
| `0x14000A6A` | `spifi_cmd_read`                | imported driver blob                                     |
| `0x14000A9C` | `spifi_make_cmd`                | imported driver blob                                     |
| `0x14000AB2` | `spifi_quad_opt_bits`           | imported driver blob                                     |
| `0x14000AC2` | `spifi_cmd`                     | imported driver blob                                     |
| `0x14000AE4` | `spifi_cmd_addr`                | imported driver blob                                     |
| `0x14000AF6` | `spifi_cmd_data`                | imported driver blob                                     |
| `0x14000B22` | `spifi_write_enable`            | imported driver blob                                     |
| `0x14000B28` | `spifi_wren_then_cmd`           | imported driver blob                                     |
| `0x14000B44` | `spifi_cmd_addr_data`           | imported driver blob                                     |
| `0x14000B8E` | `spifi_exit_mem_mode`           | imported driver blob                                     |
| `0x14000BCE` | `spifi_wait_program_done`       | imported driver blob                                     |
| `0x14000CA8` | `spifi_write_status`            | imported driver blob                                     |
| `0x14000CDA` | `spifi_enter_mem_mode`          | imported driver blob                                     |
| `0x14000D6A` | `spifi_detect_4byte_addr`       | imported driver blob                                     |
| `0x14000D9C` | `spifi_sum_sfdp_params`         | imported driver blob                                     |
| `0x14000DD8` | `spifi_get_read_count`          | imported driver blob                                     |
| `0x14000DF4` | `spifi_read_sfdp`               | imported driver blob                                     |
| `0x14000E64` | `spifi_set_capacity`            | imported driver blob                                     |
| `0x14000E7A` | `spifi_configure_modes`         | imported driver blob                                     |
| `0x140011F8` | `spifi_block_protect_engine`    | imported driver blob                                     |
| `0x1400161C` | `spifi_check_range`             | imported driver blob                                     |
| `0x14001664` | `spifi_erase_cmd`               | imported driver blob                                     |
| `0x140016A6` | `spifi_program_setup`           | imported driver blob                                     |
| `0x140016BE` | `spifi_program_pages`           | imported driver blob                                     |
| `0x140017AE` | `spifi_find_nonff_word`         | imported driver blob                                     |
| `0x140017C6` | `spifi_addr_aligned`            | imported driver blob                                     |
| `0x140017DC` | `spifi_verify_equal`            | imported driver blob                                     |
| `0x1400183E` | `spifi_verify_erased`           | imported driver blob                                     |
| `0x140018BC` | `spifi_program_region`          | imported driver blob                                     |
| `0x14001ECC` | `spifi_cfg_micron`              | imported driver blob                                     |
| `0x14002360` | `spifi_dummy_from_freq_a`       | imported driver blob                                     |
| `0x14002392` | `spifi_cfg_winbond_spansion`    | imported driver blob                                     |
| `0x140024D8` | `spifi_cfg_read_cr`             | imported driver blob                                     |
| `0x1400251A` | `spifi_cfg_micron_2`            | imported driver blob                                     |
| `0x140025C4` | `spifi_cfg_sst`                 | imported driver blob                                     |
| `0x1400267C` | `spifi_dummy_from_freq_b`       | imported driver blob                                     |
| `0x1400269A` | `spifi_cfg_macronix`            | imported driver blob                                     |
| `0x14002788` | `memcpy_bytes_thunk`            | imported driver blob; branches back to flash             |

> **Rename:** the routine at `0x1400087C` reads in the listing like a third "skip-header decryptor." It is renamed here to **`stream_reencrypt_keyA_to_keyB`** — it runs two keystreams in lockstep (Key A from the mask block, Key B via `prng_seed_from_key`) and emits `out = cipher ^ ksA ^ ksB`, converting a transport image to its at-rest form. See [§15](#15-cryptographic-scheme) and the file header in `crypto_stream.c`.

The driver-blob entries (`memcpy_fast` onward, from `0x14000A28`) are imported and not reproduced as source; they relocate into RAM at `0x10010000` and are byte-identical to the sibling lineage (each address is the sibling's minus `0x1C0`). The driver's `memcpy_bytes_thunk` (in the blob, flash ~`0x14002788`) is the one that relocates into RAM but branches **back** to `memcpy_bytes` in flash (`0x14000774`).

---

## 8. Build prerequisites

- **IDE:** LPCXpresso or its successor **MCUXpresso IDE**.
- **Toolchain:** GNU Arm Embedded (`arm-none-eabi-gcc`). C project.
- **NXP LPCOpen for LPC43xx:** the chip library only — CMSIS core headers, `chip.h`, and the SCU/RGU/CREG headers. The bootloader pokes SCU/RGU/NVIC/SCB directly via fixed addresses, so a minimal subset compiles fine.
- **The bespoke SPIFI driver blob:** there is **no public source** for this driver (it is not lpcspifilib). To produce a runnable image you must supply the relocated blob bytes (flash `0x14000A28`–`0x14002798`) or a compatible reimplementation behind the three-thunk + request-struct ABI in [§13](#13-image-format)/`spifi_glue.c`.

---

## 9. Building in MCUXpresso

Create the workspace tree from [§5](#5-repository-layout):

```
SeekProFF_43X0_Bootloader/   (workspace)
├─ lpc_chip_43xx/      →  Static Library  (LPCOpen chip lib, imported)
├─ spifi_driver_blob/  →  Object/Library  (the bespoke driver, imported as a blob)
└─ bootloader/         →  Executable      (this reconstruction; references the above)
```

In the **bootloader** project properties:

- **MCU / linker** — select the LPC43xx part; turn the managed linker script **off** and point _Linker → Linker script_ at `bootloader/ld/lpc43xx_spifi_boot.ld` (the bootloader needs the custom flash-`@0x14000000` / SRAM-relocation layout).
- **Includes** — add `bootloader/inc` and the LPCOpen `inc` directories.
- **References** — add `lpc_chip_43xx` and the driver blob so they are linked.
- **Symbols** — define the LPCOpen part macros (`CORE_M4`, `CHIP_LPC43XX`).

**Toolchain flags:**

```
-mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mfpu=fpv4-sp-d16
-std=gnu11 -Os -ffunction-sections -fdata-sections -fno-strict-aliasing
# link:
-nostartfiles -Wl,--gc-sections
```

`-nostartfiles` because we supply `startup_lpc43xx.c`. `-fno-strict-aliasing` because the flash layer type-puns addresses freely. This `Reset_Handler`'s scatter-load uses explicit flash-resident calls (`scatterload_copy_words`, `memzero_words`) rather than inline loops, so there is no memcpy/memset idiom for GCC to synthesize into not-yet-relocated SRAM. See [§12](#12-boot-flow-end-to-end).

---

## 10. Memory map

### Flash (external SPI-NOR, memory-mapped at `0x14000000`)

| Region                       | Address                   | Notes                                                        |
| ---------------------------- | ------------------------- | ------------------------------------------------------------ |
| Bootloader image (this file) | `0x14000000`–`0x14010000` | 64 KB                                                        |
| SPIFI driver blob            | `0x14000A28`–`0x14002798` | `0x1D70` bytes; relocated to RAM `0x10010000`                |
| Build-info block             | `0x14002798`              | 4-byte marker + build date/time; the info pointer to the app |
| `.data` (LMA)                | `0x140027BC`              | `0x160` bytes; copied to RAM `0x10011D70`                    |
| Boot-config pointer (LMA)    | `0x140028F8`              | dword `0x14010000` (`.data+0x13C`)                           |
| Key A / mask (LMA)           | `0x140028FC`              | 16 bytes (`.data+0x140`)                                     |
| Key B fixed (LMA)            | `0x1400290C`              | 16 bytes (`.data+0x150`)                                     |
| Boot-config record           | `0x14010000`              | 16 bytes; first dword selects the slot                       |
| Firmware slot A              | `0x14050000`              | encrypted application                                        |
| Firmware slot B              | `0x14060000`              | encrypted application                                        |
| Recovery / golden slot       | `0x14070000`              | encrypted application                                        |

Slots are `0x10000` (64 KB) apart. The boot-config base (`0x14010000`) is also handed to the booted application.

### RAM (the layout `Reset_Handler` establishes)

```
0x10000000 ┐ (decrypted app vector table lands here; 0x200/204/208 = handoff block;
           │  0x20C/210 = the warm-boot update mailbox, read at reset)
           │  64 KB application region
0x10010000 ┤ SPIFI driver blob   (RAM copy of flash 0x14000A28+, 0x1D70 bytes)
0x10011D70 ┤ driver .data tail + keys/config (Key A @..EB0, Key B @..EC0; from 0x140027BC, 0x160)
0x10011ED0 ┤ BSS: SPIFI request struct (0x10011ED0) + driver context (0x10011EE4) — zeroed (0x94)
0x10018000 ┘ MSP (bootloader stack top)
```

Staging buffer: `0x20000000` (a separate AHB SRAM bank) — used for program-data marshalling.

> **Note:** unlike a textbook CRT0, this one plants **no** stack-guard canary and performs **no** `__set_MSP` — the MSP loaded from vector[0] at reset is used as-is, and there is no guard word below it.

### Flash ↔ RAM relocation formula (driver only)

`Reset_Handler` relocates flash `0x14000A28 → RAM 0x10010000`. So for any RAM address `R` in the relocated driver:

```
flash = R − 0x10010000 + 0x14000A28
```

This is how the three thunk targets (`0x100105FF`, `0x10010E95`, `0x100110EB`) and the cross-boundary `memcpy_bytes_thunk` map back to flash.

---

## 11. The build identity

This image embeds **no internal name string**. The only identity baked in is a 4-byte marker `{01,02,00,00}` immediately followed by the build date/time `Jun 26 2016` / `11:08:46`, at flash `0x14002798`. The marker is read at runtime: `prng_seed_from_key` forms the big-endian composite `0x01020000` and compares it to `0x01010000` to choose the fixed-vs-silicon Key-B path (the composite exceeds the threshold, so the fixed key is used). The repository/workspace name `SeekProFF_43X0_Bootloader` is therefore descriptive, anchored to the product code (LQ-XXX) and the build date. (`43X0` is an internal product/board designator carried over from the lineage; its precise meaning is not recoverable from the image alone.)

---

## 12. Boot flow, end to end

Traced against the analyzed dump, so the addresses and values are the real ones.

### Phase 0 — The part's boot ROM hands off

On power-up the LPC43xx internal boot ROM runs, strapped to boot from SPIFI. It maps the SPI-NOR at `0x14000000` and follows the Cortex-M reset convention:

- `MSP ← *(0x14000000) = 0x10018000`
- `PC  ← *(0x14000004) = 0x14000263` (Thumb), i.e. `Reset_Handler` at `0x14000262`, executing in place (XIP) from flash.

### Phase 1 — `Reset_Handler` runs the whole pipeline from flash

There is no separate RAM stage. `Reset_Handler` does, in order:

1. **Minimal CRT0 prologue.** `CPSID i`. **No** `M4MEMMAP` remap, **no** stack-guard canary, **no** `__set_MSP`, and **no** early `VTOR` write — VTOR is repointed at the application only at hand-off. The only register setup is: read/consume the warm-boot mailbox; pulse-reset peripherals (`RGU_RESET_CTRL0 = 0x10DF1000`, `RGU_RESET_CTRL1 = 0x01DFF7FF`); clear all NVIC pending (`ICPR0..7 = 0xFFFFFFFF`).
2. **Scatter-load** (a fixed 3-entry table at `0x14000240`): copy `.data` (driver tail + keys/config) `0x140027BC → 0x10011D70` (`0x160`); zero `.bss` `0x10011ED0` (`0x94`); copy the SPIFI driver blob `0x14000A28 → 0x10010000` (`0x1D70`). The copies go through the flash-resident helper `scatterload_copy_words`; the zero through `memzero_words` — both flash-resident, so callable before the driver exists in RAM.
3. **Warm-boot update mailbox.** Read the flag at `0x1000020C` (honored only if it is `0xAA55FF01` or `0xAA55FF02`) and the 64-bit gate at `0x10000210` (must be `≤ 0x752F`); if both valid, keep the **raw** flag value as the rollback request, then clear the mailbox. The raw flag (not a mapped 1/2 index) is what is passed to `select_boot_slot`.
4. **`spifi_init`.** Initialise the request struct, pin-mux the SPIFI bus, and bring the imported driver up into XIP through the init thunk.
5. **Key-A → Key-B migration** (every boot, all three slots, **unrolled** inline). For each slot, if it validates under **Key A** (`image_try_keys` — still transport/OTA form): copy the ciphertext (length read directly from `slot+0x204`) to the staging buffer, re-key it in place with `stream_reencrypt_keyA_to_keyB` (`out = cipher ^ ksA ^ ksB`, header window preserved), erase the slot's 64 KB block (`flash_chip_erase`), and write the Key-B image back (`flash_program`, staged-flag 0). Subsequent boots see a Key-B slot.
6. **`select_boot_slot`** with the rollback request from step 3 (`0` normally) — read the selector dword directly from `0x14010000` and resolve a slot via the Key-B gate `image_try_keys_copy2`.
7. **Segmented load (Key B).** Decrypt the `0x2C0`-byte segment table (`stream_decrypt_skip_header`); load pass-1 (4 descriptors, `stream_decrypt_segment`), zero 4 BSS regions, load pass-2 (4 descriptors). Each segment reseeds Key B and is fast-forwarded to its absolute position.
8. **Handoff.** Copy the first `0x200` bytes of the decrypted table to `0x10000000` (installs the app vector table); publish `0x10000200 = &build-info (0x14002798)`, `0x10000204 = config base (0x14010000)`, `0x10000208 = slot id (0=A,1=B,2=recovery)`; `VTOR = 0x10000000`; `CPSIE i`; jump the entry (segment-table word 132). The MSP is **not** reloaded — the app entry owns its stack. Control never returns.

**Why a flash-resident driver-only relocation:** the migration and rollback paths erase and reprogram the SPIFI flash, which means leaving memory-mapped mode and driving the controller with explicit commands — you cannot fetch _driver_ instructions from flash while doing that. So only the driver is relocated to SRAM; the boot logic stays in flash and calls into the RAM driver through the three thunks when it needs command mode.

### Failure behavior

There is no rich error handling. A null slot base or a failed segment decrypt parks the core in a `while(1)` spin. Recovery is at the slot level (the A/B/recovery ordering). A corrupt image that still checksums correctly but misbehaves is the application's problem.

### One-line summary for this dump

```
ROM      : map SPIFI @0x14000000; MSP=0x10018000; jump 0x14000262
CRT0     : CPSID; mailbox check/clear; RGU reset; clear NVIC pending (ICPR0..7)
           copy .data ->0x10011D70; zero .bss @0x10011ED0; copy driver ->0x10010000
           (NO M4MEMMAP, NO canary, NO MSP reset, NO early VTOR)
mailbox  : flag@0x1000020C / gate@0x10000210 -> RAW flag (none here)
spifi    : request-first init; pinmux P3_4..P3_8 (P3_3 unset, P3_4 written twice); driver -> XIP
migration: all slots checked under Key A -> none in transport form -> SKIPPED
select   : config selector blank -> A blank, B blank -> recovery 0x14070000
load     : seed Key B; decrypt 0x2C0 table; 2 load passes + BSS zero
           pass-1 desc[0] = { srcRef 0x14009970, dstVMA 0x10000000, len 0x240 }
handoff  : install vec @0x10000000 (MSP 0x1000A000); 0x200/204/208; VTOR=0x10000000; CPSIE
           jump 0x100806A5 (table word 132)
app      : running from SRAM, owns 0x10000000..0x10018000
```

---

## 13. Image format

An application image is a stream of 32-bit words, encrypted with the positional keystream cipher, with one cleartext landmark. **There is no monolithic path and no "CODE" footer**; the loader always takes the segmented path.

### Cleartext header window (always plaintext)

Words **128–143** (byte offset **`0x200`–`0x23F`**, 64 bytes) are stored **verbatim, not XORed** — the cleartext image header the loader reads before it knows the key:

- `header[0]` = magic `0xA1B2C3D4`
- `header[1]` = image length in bytes (must be `< 0x10000`)
- word 132 = the app entry pointer (used after the segment table is decrypted)

### Segment table

The loader decrypts a `0x2C0`-byte (176-word) table. The observed word offsets (the descriptor format is byte-confirmed for pass-1 against this dump; the BSS / pass-2 struct semantics are inferred by symmetry):

```
[128..143]  cleartext header window; word 132 = app entry pointer
[144..155]  pass-1 load descriptors: 4 × { srcRef, dstVMA, byteLen }
[156..163]  BSS descriptors:         4 × { addr, byteLen }
[164..175]  pass-2 load descriptors: 4 × { srcRef, dstVMA, byteLen }
```

`srcRef` is stored as `0x14000000 + image_offset`; the read base is the chosen slot, so `image_offset = srcRef − 0x14000000`, passed to `stream_decrypt_segment` (which fast-forwards the keystream by `image_offset/4` words and keeps the 16-word window at `[0x80..0x8F]`). On this dump the descriptor coverage tiles the whole `0xCADC`-byte recovery image: pass-2 reads offset `0x2C0`..`0x9970`, then pass-1 reads `0x9970`..`0xCADC`. The first pass-1 descriptor is `{ 0x14009970, 0x10000000, 0x240 }` — read offset `0x9970`, load to the app vector base, length `0x240`.

### SPIFI driver request ABI (program/erase)

The flash layer marshals one operation into a fixed struct (instance at `0x10011ED0`) and calls a thunk:

```
+0x00 flash_offset : target byte offset from 0x14000000
+0x04 length       : byte count
+0x08 stage_buf    : AHB staging buffer (0x20000000) for program data, or 0
+0x0C sentinel     : 0
+0x10 opcode       : 0x08 = program, 0x20 = erase
```

The request ABI carries **no source pointer**, so program data is staged through `0x20000000` first (the caller populates it); reads are plain XIP loads. `flash_program`'s `src` argument is therefore vestigial.

---

## 14. The slot system

Three firmware slots plus a 16-byte config record drive selection. `select_boot_slot(update_flag)` reads the selector dword directly from `0x14010000` and returns the chosen slot base.

### Selection logic (`update_flag == 0`, the normal call)

Each candidate is gated through the **Key-B** acceptance test `image_try_keys_copy2`:

- **blank** (`0x00000000` or `0xFFFFFFFF`) → try **A**, then **B**, then **recovery**
- **`1`** → prefer **B**, then **A**, then **recovery**
- **any other value** (e.g. `2`) → prefer **recovery**, then **A**, then **B**

On this dump the record is all `0xFF`, so the blank branch is taken; slots A and B are blank (`0xFFFFFFFF` magic), so **recovery wins**.

### Rollback path (`update_flag != 0`)

Driven by the warm-boot mailbox. It re-resolves the current slot, and if it is A or B, stamps the **raw** flag value over that slot's header magic (invalidating it), forces the config selector to `2` via the read-modify-write path (`flash_program_rmw` — both the magic stamp and the selector write), and returns recovery.

---

## 15. Cryptographic scheme

### Cipher — xorshift128 (Marsaglia)

`xorshift128_next` is the textbook 32-bit xorshift128 with shift constants **11 / 19 / 8**, state `[x, y, z, w]`:

```
t = x ^ (x << 11)
x = y;  y = z;  z = w
w = w ^ (w >> 19) ^ t ^ (t >> 8)
return w                       ; one keystream word
```

In this build it is a **standalone, shared routine** (flash `0x140007F4`) that every stream routine calls.

### Key → seed transform

The 16-byte key (`k0,k1,k2,k3` as little-endian dwords) is rotated by one word into the state, with **no whitening**:

```
x = k1
y = k2
z = k3
w = k0          ; key word 0 lands in w, not x
```

### Two keys and the migration

- **Key A** (transport/OTA): seeded directly from the 16-byte mask block. A slot in transport form validates only under Key A.
- **Key B** (at-rest device): seeded by `prng_seed_from_key`, which reads the build marker and uses either the **fixed** embedded Key B (when the composite marker `> 0x01010000`, as here) or a **silicon-derived** Key B = four device-ID dwords at `0x40045000` XORed with the mask block.
- **Migration** (`stream_reencrypt_keyA_to_keyB`): runs both generators in lockstep and emits `out = cipher ^ ksA ^ ksB`, converting a Key-A image into its Key-B form, header window preserved. Run at boot over all slots so storage settles on Key B.

### Decryption — positional XOR

Each ciphertext word is XORed with one keystream word; the PRNG is advanced for _every_ word (so keystream word N lines up with image word N), including the verbatim header window. The window test is expressed as: word `idx` is verbatim iff `(idx − 0x80) ≤ 0xF`. The stream routines read XIP flash **directly**, one word at a time — there is no RAM scratch/staging buffer. `stream_decrypt_segment` fast-forwards by `image_offset/4` words so the keystream aligns to each segment's position.

### Integrity check

`stream_checksum16` / `stream_checksum16_copy2` decrypt and sum all plaintext words into a 32-bit accumulator; the image is accepted only if that accumulator equals exactly **`0x00000000`**. Each routine returns **two** sums through out-params — a raw-ciphertext sum (vestigial) and the decrypted sum; only the decrypted sum is checked. `stream_checksum16` keys Key A; `_copy2` keys Key B. The acceptance decision is the checksum alone (no decrypted-SP range check). For this dump, the recovery slot sums to `0x40626ECA` under Key A (reject) and exactly `0x00000000` under Key B (accept) — see [§22](#22-verifying-the-reconstruction).

---

## 16. Keys and where they live

Two embedded 128-bit keys, plus an optional silicon-derived Key B.

### Key A — the mask block (transport key, and the silicon-derivation mask)

```
RAM (runtime) : 0x10011EB0      Flash : 0x140028FC      File offset : 0x28FC   (.data+0x140)
Bytes (16)    : 58 D6 AB E5 A9 4E 4D E6 50 AE 3A 84 F9 F8 F2 81
```

Seed words (little-endian dwords): `k0=0xE5ABD658`, `k1=0xE64D4EA9`, `k2=0x843AAE50`, `k3=0x81F2F8F9` → state `[k1,k2,k3,k0] = [0xE64D4EA9, 0x843AAE50, 0x81F2F8F9, 0xE5ABD658]` (no whitening).

### Key B — fixed (at-rest device key)

```
RAM (runtime) : 0x10011EC0      Flash : 0x1400290C      File offset : 0x290C   (.data+0x150)
Bytes (16)    : 6D A0 5A 33 F5 40 10 00 34 E2 C8 94 7D 05 70 8C
```

Seed words: `k0=0x335AA06D`, `k1=0x001040F5`, `k2=0x94C8E234`, `k3=0x8C70057D` → state `[k1,k2,k3,k0] = [0x001040F5, 0x94C8E234, 0x8C70057D, 0x335AA06D]`.

### Silicon-derived Key B (not used in this dump)

When the build marker does **not** exceed `0x01010000`, Key B is derived from four device-unique dwords at `0x40045000` XORed with the mask block. This dump's marker is `0x01020000`, so the fixed key above is active.

### How the keys get into RAM

`Reset_Handler` copies `0x160` bytes from flash `0x140027BC → 0x10011D70`. That blob's tail holds the config base (`0x14010000`), then the mask/Key-A block (`0x10011EB0`), then the fixed Key B (`0x10011EC0`).

To decrypt a captured image: seed xorshift128 with the matching key's `[x,y,z,w]`, keep words 128–143 verbatim, and XOR every other 32-bit word with one keystream word.

---

## 17. Module reference

### `bootloader.h`

The single shared header: flash/RAM memory map, image-format magic numbers and offsets, slot bases, the SPIFI request ABI, return-code legend, build-marker thresholds, and prototypes for every reconstructed function. Pointers are real pointers; argument order/semantics preserved; the gotcha functions carry warning comments.

### `startup_lpc43xx.c`

`g_pfnVectors[]` and the fused `Reset_Handler` — a **minimal** CRT0 (peripheral reset, NVIC clear, scatter-load via `scatterload_copy_words` + `memzero_words`; **no** remap/canary/MSP-reset), the warm-boot mailbox (raw flag), `spifi_init`, the **unrolled** Key-A→Key-B migration sweep, `select_boot_slot`, the segmented decrypt/load, and the handoff. Plus the plain-spin core-fault handlers, the `IRQ52_Handler` catch-all (no live peripheral handlers in this image), and `scatterload_copy_words`.

### `boot_main.c`

The boot leaves: `select_boot_slot`, the two single-key gates `image_try_keys` (Key A) / `image_try_keys_copy2` (Key B) — each **fused** to validate the header and run the keyed checksum inline (no separate `image_checksum_ok`) — and `memzero_words`. The acceptance test is the keystream checksum alone (`== 0`). All flash-resident.

### `crypto_stream.c`

The keystream cipher: the **standalone** `xorshift128_next`, `prng_seed_from_key` (Key-B seeder with the fixed/silicon branch), `stream_checksum16` / `_copy2`, `stream_reencrypt_keyA_to_keyB` (the dual-key migration transform), the **single** `stream_decrypt_skip_header`, and `stream_decrypt_segment`. Verified byte-for-byte against the dump (no-whitening seed, 11/19/8 shifts, verbatim window, 32-bit sentinel-0 checksum, direct XIP reads). The checksum and decrypt loops are deliberately the same loop differing only by store-vs-accumulate.

### `flash_if.c`

The flash-access layer: `spifi_init` (request-struct init first, then pin-mux, then driver bring-up), `flash_program` (4-arg, sentinel 0, **no** internal memcpy), `flash_erase_region`, `flash_program_rmw` (erase-then-program), `flash_chip_erase`. Program data is staged through `0x20000000`; reads are XIP. **No** read-back verify on program and **no** blank-verify on erase. All real flash work goes through the three thunks.

### `spifi_glue.c`

The imported-driver boundary: the request-struct ABI, the request/context BSS objects, and the three flash-resident thunks (`spifi_drv_init_thunk` / `_program_thunk` / `_op_thunk`) that branch into the relocated driver at `0x10010000`. The driver bytes themselves are imported. Documents the relocation formula and the cross-boundary `memcpy_bytes_thunk`.

### `keys.c`

The embedded key material and config base: `g_key_mask` (Key A and the silicon mask), `g_keyB` (fixed Key B), `g_boot_config_ptr` (`0x14010000`), and the flash-resident `g_build_info` block (marker + build date/time, the info pointer handed to the app).

### `util_mem.c`

`memcpy_auto` (word copy + byte tail — the workhorse for header reads, staging, the vector-table install) and `memcpy_bytes` (plain byte copy, also the branch target of the driver's relocated `memcpy_bytes_thunk`). The driver's own `memcpy_fast` lives in the imported blob.

### `ld/lpc43xx_spifi_boot.ld`

The custom layout: vectors at `0x14000000`, the flash-resident boot code, the scatter-load section table (copy `.data`, zero `.bss`, copy the driver `.text_ram`), the driver `.text_ram` relocated to `0x10010000`, `.data` to `0x10011D70`, `.bss` (NOLOAD) at `0x10011ED0`, and stack top `0x10018000`.

---

## 18. Configuration constants

The key tunables (all in `bootloader.h`):

```c
#define SPIFI_XIP_BASE        0x14000000u   /* external SPI-NOR window         */
#define BOOT_CONFIG_BASE      0x14010000u   /* 16-byte boot-config record      */
#define SLOT_A_BASE           0x14050000u
#define SLOT_B_BASE           0x14060000u
#define SLOT_RECOVERY_BASE    0x14070000u
#define SLOT_STRIDE           0x00010000u   /* 64 KB between slots             */
#define SPIFI_CTRL_BASE       0x40003000u
#define SILICON_ID_BASE       0x40045000u   /* device-ID words (silicon Key B) */
#define RAM_APP_LOAD_BASE     0x10000000u   /* decrypted app vectors land here */
#define RAM_HANDOFF_BLOCK     0x10000200u   /* +0 info ptr, +4 cfg ptr, +8 slot id */
#define RAM_UPDATE_FLAG       0x1000020Cu   /* warm-boot mailbox flag          */
#define RAM_UPDATE_MAGIC      0x10000210u   /* warm-boot mailbox 64-bit gate   */
#define RAM_DRIVER_BASE       0x10010000u   /* relocated SPIFI driver          */
#define RAM_REQUEST_STRUCT    0x10011ED0u   /* SPIFI request struct            */
#define RAM_DRIVER_CONTEXT    0x10011EE4u   /* SPIFI driver context            */
#define MSP_TOP               0x10018000u
#define STAGING_BUF_BASE      0x20000000u
#define IMG_HEADER_OFFSET     0x200u        /* cleartext header @ slot+0x200   */
#define IMG_MAGIC             0xA1B2C3D4u
#define IMG_MAX_LEN           0x10000u      /* length cap                      */
#define SEG_TABLE_BYTES       0x2C0u
#define IMG_HDR_WORD_FIRST    128u          /* 0x200 / 4 (verbatim window)     */
#define IMG_HDR_WORD_LAST     143u
#define SEG_SKIP_WORDS        0x80u
#define ACCEPT_SENTINEL       0x00000000u   /* decrypted checksum must equal this */
#define BUILD_MARKER          0x01020000u   /* this image's marker composite   */
#define BUILD_MARKER_FIXEDKEY 0x01010000u   /* > this -> fixed Key B           */
#define UPDATE_FLAG_A         0xAA55FF01u
#define UPDATE_FLAG_B         0xAA55FF02u
#define UPDATE_MAGIC_GATE     0x752Fu
#define SLOTID_A              0
#define SLOTID_B              1
#define SLOTID_RECOVERY       2
#define SPIFI_OP_PROGRAM      0x08u
#define SPIFI_OP_ERASE        0x20u
#define SPIFI_STAGE_FLAG      0x20000000u
#define FL_OK                 0
#define FL_BADARG             11            /* 0xB */
#define FL_NOTINIT            2
```

---

## 19. Naming conventions and known misnomers

The manually assigned labels are preserved verbatim so source and listing line up 1:1. Several are **misleading** and are flagged in the code:

| Function                                     | Name suggests           | Actually does                                                                                                                               |
| -------------------------------------------- | ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `stream_reencrypt_keyA_to_keyB` (0x1400087C) | a skip-header decryptor | runs **two** keystreams (`out = cipher ^ ksA ^ ksB`) to convert Key-A → Key-B form — renamed from the listing's third "skip-header" label   |
| `stream_checksum16` / `_copy2`               | a 16-bit / mod-2¹⁶ sum  | accumulates and compares the **full 32-bit** decrypted-word sum against `0x00000000`; also returns a vestigial raw sum through an out-param |
| `image_try_keys` / `_copy2`                  | "tries keys" (plural)   | each tries exactly **one** key — `image_try_keys` = Key A (transport detect), `_copy2` = Key B (boot gate) — with the checksum fused inline |
| `prng_seed_from_key`                         | a generic key seeder    | seeds **only** the Key-B / silicon-derived path; the Key-A seed is inlined at its call sites                                                |
| `flash_chip_erase`                           | erases the whole device | erases the **single 64 KB block** containing `addr` (fixed `0x10000` length, no blank-verify)                                               |
| `flash_program_rmw`                          | block read-modify-write | just **erases then programs** the range (data pre-staged by the caller); no read-back of surrounding bytes                                  |

---

## 20. Reconstruction caveats (load-bearing assumptions)

These assumptions are load-bearing — losing them will break a build or mislead an analyst. The crypto path, CRT0 register sequence, scatter tables, key/marker constants, slot geometry, and boot decision are **byte-confirmed** against the dump (see [§22](#22-verifying-the-reconstruction)); what remains inferred:

- **The SPIFI driver blob is imported, not sourced.** It is **not** lpcspifilib — it is a bespoke driver that pokes the SPIFI registers at `0x40003000` and switches per vendor by JEDEC ID. The reconstruction reproduces only its **call surface** (the three-thunk + request-struct ABI in `spifi_glue.c`/`flash_if.c`); the driver bytes (flash `0x14000A28`–`0x14002798`) must be supplied to build a runnable image.
- **The request-struct field map** (`flash_offset/length/stage_buf/sentinel/opcode` at `+0x00/04/08/0C/10`) and the three thunk entry points (`0x100105FF` init, `0x10010E95` program, `0x100110EB` op/erase) are read off the call sites; confirm them against the blob you vendor. The init thunk additionally receives three constant mode words (`3, 0xC0, 0xC`) passed in registers — also part of the driver ABI.
- **Segmented-path table offsets** — pass-1's `{srcRef, dstVMA, byteLen}` descriptor format is byte-confirmed on this dump (`{0x14009970, 0x10000000, 0x240}`); the BSS `[156..]` and pass-2 `[164..]` struct semantics are **inferred** by symmetry (and consistent with the descriptor coverage tiling the whole `0xCADC`-byte image).
- **The warm-boot mailbox** is passed through as the **raw** flag value to `select_boot_slot`; the gate predicates (`flag ∈ {0xAA55FF01, 0xAA55FF02}`, `gate ≤ 0x752F`) are confirmed.
- **The RGU reset masks** (`0x10DF1000` / `0x01DFF7FF`) are read straight from the literal pool.
- **The silicon-Key-B path** (`0x40045000` device-ID XOR mask) is reconstructed but **unexercised** by this dump (its marker selects the fixed key).

---

## 21. Intentional oddities preserved from the binary

These look like bugs or omissions but are faithfully reproduced because they are in the original machine code:

- **`spifi_init` skips P3_3 and writes P3_4 twice** — the pin-mux configures `P3_4..P3_8`, and `P3_4` is written `0xF3` then immediately overwritten with `0xD3`. Both are reproduced verbatim. The request struct is also primed (with sentinel `0xFFFFFFFF`, opcode 8) **before** the pins are touched.
- **`flash_chip_erase` erases one fixed 64 KB block** despite its name, and performs no blank-verify (and the program/erase paths perform no read-back verify).
- **`flash_program` ignores its `src` argument** — the request ABI carries no source pointer, so all program data is read from the AHB staging buffer, and the sentinel field is `0` (not `0xFFFFFFFF`).
- **The CRT0 is minimal** — no `M4MEMMAP` remap, no `0xCDCDCDCD` stack canary, no `__set_MSP`, and no early `VTOR`. VTOR is written exactly once, at hand-off.
- **The migration is unrolled** — the three slots are checked with three inline copies of the same block, not a loop over a slot-base array.
- **The checksum routines return a vestigial raw-ciphertext sum** through a second out-param that no caller reads.
- **`memcpy_bytes_thunk` is a RAM→flash cross-boundary call** — it relocates into SRAM with the driver blob but branches back to `memcpy_bytes` in flash (`0x14000774`).
- **Storage is left re-encrypted, not plaintext** — the Key-A→Key-B migration writes a _re-keyed_ image back to flash, so a migrated slot is still encrypted (under Key B), just under a different key than it arrived in.

---

## 22. Verifying the reconstruction

You do **not** need silicon to check the logic — the crypto path (`crypto_stream.c` + the seeders) is self-contained and host-compilable. It has been **verified bit-exact against this dump** (recovery slot, length `0xCADC`):

```
stream_checksum16(recovery, 0xCADC, seed(KeyA))       -> Σdec = 0x40626ECA   (Key A rejects)
stream_checksum16_copy2(recovery, 0xCADC, seed(KeyB)) -> Σdec = 0x00000000   (Key B accepts)
migration sweep: no slot validates under Key A        -> SKIPPED
select_boot_slot(0): selector blank (all 0xFF); A & B blank (0xFFFFFFFF magic) -> recovery (0x14070000)
segment-table decrypt (Key B), then load:
  installed app vector[0] (MSP) = 0x1000A000
  jump entry = segment-table word 132 = 0x100806A5
  pass-1 descriptor[0] = { srcRef 0x14009970, dstVMA 0x10000000, len 0x240 }
```

Image identity: 4,194,304 bytes, SHA-256 `195f53383fd3bdd33dd867aa53df9350d01d035b50eeed338b4b56c2e447cdbe`. Because the checksum reaches exactly `0x00000000` and the decrypted vector table yields the expected MSP/entry, the cipher reconstruction is confirmed correct; the SPIFI half is then the imported driver doing read/program/erase against `0x14000000`.

---

## 23. Security analysis

- **Software xorshift, not hardware AES.** Although LPC43**S** parts include an AES engine + OTP, this bootloader protects the application with a software xorshift128 keystream. There is no authentication — no signature or MAC — only a 32-bit additive checksum.
- **Acceptance is the checksum alone.** A slot is accepted purely on its plaintext checksum reaching `0x00000000` under a key; there is no decrypted entry/SP range check.
- **No CRT0 hardening.** There is no stack-guard canary and no MSP re-init at reset; the loader relies entirely on well-formed inputs.
- **Keys are in the clear in flash.** Both 128-bit keys are constants in the image; anyone with the dump has them. The silicon-derived Key-B path (device-ID XOR mask) is only as strong as the secrecy of the mask, which is also in the image.
- **Known-plaintext is fatal.** xorshift128 is fully invertible: ~16 bytes of known plaintext at a known offset recovers part of the state, and the cleartext 64-byte header window (offset `0x200`) plus the standard Cortex-M vector-table structure provide predictable plaintext to attack. The two-key migration does not change this — both keystreams are recoverable.
- **The checksum is trivially forgeable.** A 32-bit additive sum imposes no meaningful integrity barrier against an attacker who controls the plaintext.

**Bottom line:** this scheme is symmetric obfuscation. It raises the bar against casual cloning of an encrypted image, and nothing more. Do not adopt it where a motivated attacker has the bootloader image.

---

## 24. Relationship to sibling LPC43xx builds

This image is one of a family of closely-related LPC43xx Compact Pro boot images sharing the magic/segment-table format, the xorshift128 algorithm family, the slot/config model, and the overall boot intent. A later build in the same lineage (marker `{C9,12,03,00}`, `Jan 7 2019`, internal name `80K_43X0_Bootloader`) diverges more substantially and is useful as a structural contrast for anyone cross-referencing dumps. The differences that matter when comparing this image to that later one:

- **Runs from flash, not RAM.** This image executes its entire boot pipeline in place (XIP) inside `Reset_Handler`; the later build relocated all of its code to SRAM and ran a separate stage there. Here, only the SPIFI driver blob is relocated.
- **Bespoke SPIFI driver, not lpcspifilib.** This image carries a hand-written register-level driver (imported as a blob, reached through three thunks); the later build wrapped NXP lpcspifilib.
- **Two keys + a boot-time A→B migration.** The later build had a single key path; this image distinguishes a transport Key A from an at-rest Key B and re-encrypts slots at boot.
- **No whitening, sentinel 0.** Seeds are taken from the key words directly (the later build XORed a fixed mask); the checksum sentinel is `0x00000000` (the later build used `0x0000FFFF`).
- **Segmented-only.** No monolithic path and no "CODE" footer (the later build had both).
- **All-trap vector table.** No live peripheral IRQ handlers (the later build installed several); core faults spin with a plain `B .` rather than `WFI`.
- **Different geometry and identity.** 64 KB slots at `0x14050000`/`0x14060000`/`0x14070000`, stride `0x10000`; initial SP `0x10018000`; marker `{01,02,00,00}` / `Jun 26 2016` `11:08:46` and **no** internal name string.

This section is for cross-reference only; the rest of this document stands on its own against the present image.

---

## 25. Known issues / TODO

- **Runnability:** supply the bespoke SPIFI driver blob (or a reimplementation behind the three-thunk + request-struct ABI) before expecting program/erase on hardware.
- **Driver ABI pinning:** confirm the request-struct field offsets, the three thunk entry points, and the init mode words (`3, 0xC0, 0xC`) against the blob revision you vendor.
- **Silicon Key-B path:** unexercised by this dump (the marker selects the fixed key); validate the `0x40045000` device-ID derivation against a unit that uses it, if one is captured.
- **Segmented table layout:** pass-1 is byte-confirmed; the inferred BSS / pass-2 descriptor semantics deserve validation against a second sample.
- **Warm-boot rollback:** the raw-flag stamping path is reconstructed from the disassembly; confirm against a unit that exercises an interrupted update.

Potential follow-up artifacts:

- A companion **host-side encryptor** that round-trips images for either key, including the A→B migration transform.
- A symbolized export of the imported driver blob so the `spifi_*` device-config paths read as source.

---

## 26. Glossary

- **SPIFI** — NXP's SPI Flash Interface peripheral; supports memory-mapped (XIP) reads from external SPI-NOR.
- **XIP (eXecute In Place)** — running/reading code directly from memory-mapped flash without copying to RAM.
- **CRT0** — the C runtime startup that initializes `.data`/`.bss` and hands control to the program.
- **VTOR** — Vector Table Offset Register (`0xE000ED08`); points the core at the active exception vector table.
- **SFDP / JEDEC ID** — standard SPI-NOR discovery (`0x5A` read-SFDP) and identity (`0x9F` read-ID) commands the bespoke driver uses to detect the device and pick a vendor config path.
- **Transport vs. at-rest key** — Key A protects an image in transit/OTA; the boot-time migration re-keys it to Key B for storage on the device.
- **Verbatim window** — the 16-word (`0x200`–`0x23F`) cleartext header region the cipher copies unchanged.

---

## 27. References

- **NXP LPC43xx User Manual (UM10503)** — SPIFI, CREG, RGU, SCU, NVIC register details.
- **ARMv7-M Architecture Reference Manual** — Cortex-M4 reset behavior, vector table, VTOR, `MSP`.
- **NXP LPCOpen** — chip layer (CMSIS + peripheral drivers) for LPC43xx.
- **JEDEC JESD216 (SFDP)** — Serial Flash Discoverable Parameters, used by the bespoke driver's device detection.
- **Marsaglia, G., "Xorshift RNGs," Journal of Statistical Software (2003)** — the PRNG family used as the keystream generator (shift triple 11/19/8).

---

_This README documents a reverse-engineering reconstruction. The disassembly is the source of truth; where this document and the listing disagree, trust the listing. The embedded keys are recovered constants, not secrets._
