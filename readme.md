# Seek Thermal Bootloader Reconstructions

**A repository of reverse-engineered, behaviorally faithful C reconstructions of the first-stage secure bootloaders for Seek Thermal cameras (LPC43xx / ARM Cortex-M4).**

This repository contains two complete from-scratch C reconstructions of Seek Thermal boot images recovered by disassembling 4 MB SPI-NOR flash dumps. It also includes a host-side decryption tool capable of automatically locating keys and decrypting firmware payloads from raw flash dumps.

---

## 1. Overview

The bootloaders in this repository live in external SPIFI flash (mapped at `0x14000000`). Their primary job is to initialize the core and SPI-NOR flash controller, select a firmware "slot" based on a boot configuration record, verify and decrypt the application into internal SRAM, and jump to it.

The reconstructions are highly faithful to the original machine code, intended to be byte-for-byte accurate on the cryptographic paths and structurally accurate elsewhere.

### Security Notice

> **The encryption keys in this repository are recovered constants, not secrets.** > Both 128-bit keys are embedded in cleartext in the original flash images. The protection scheme relies on a software **xorshift128 stream cipher**, which provides symmetric obfuscation rather than authenticated encryption. There is no signature or MAC—only a 16-bit additive checksum. A known-plaintext attack trivially recovers the keystream. **Do not reuse this design for anything that requires tamper resistance.**

---

## 2. Repository Structure

- **`Bootloader 2019.01.07 15.56.20 Compact Pro FF/`**
  The bootloader for the Seek Thermal Compact Pro FF. Reserves a 112 KB application region in SRAM and relies purely on a checksum for image validation.
- **`Bootloader 2021.08.12 08.20.23 Nano 300/`**
  The bootloader for the Seek Thermal Nano 300. Reserves an 80 KB application region and adds an initial Stack Pointer (SP) range check on top of the checksum validation.
- **`decrypt_firmware.js`**
  A standalone Node.js script that scans a full SPI-NOR flash dump, automatically discovers the embedded obfuscation keys (regardless of offset), and extracts/decrypts all valid firmware application images into standard binaries.

---

## 3. Bootloader Comparison

Both bootloaders share the same core architecture (NXP LPC43xx, ARMv7-M, external SPI-NOR via lpcspifilib) and cryptographic primitives, but feature distinct memory layouts and verification logic.

| Feature                  | Compact Pro FF                     | Nano 300                          |
| ------------------------ | ---------------------------------- | --------------------------------- |
| **Build Timestamp**      | Jan 7, 2019                        | Aug 12, 2021                      |
| **App Region Size**      | 112 KB (`0x10000000`–`0x1001C000`) | 80 KB (`0x10000000`–`0x10014000`) |
| **Bootloader SRAM Base** | `0x1001C000`                       | `0x10014000`                      |
| **Initial SP (MSP)**     | `0x10020000`                       | `0x10018000`                      |
| **Acceptance Test**      | Full 32-bit sum `== 0x0000FFFF`    | Checksum + SP boundary validation |
| **Target Architecture**  | LPC4337-class, Cortex-M4           | LPC4337-class, Cortex-M4          |

_Note: The "80K" present in the internal build names of both bootloaders is a legacy lineage tag; the Compact Pro FF actually allocates 112 KB for the application despite its `80K_43X0_Bootloader` identifier._

---

## 4. Cryptographic Scheme

Both bootloaders protect application firmware using a software **xorshift128 (Marsaglia)** keystream cipher.

- **Initialization:** The 16-byte key is whitened with a constant (`0x13579BDF`) and rotated into the PRNG state.
- **Decryption:** Operates on 32-bit words. The keystream advances for every word.
- **Verbatim Window:** Words 128–143 (the 64-byte cleartext header at offset `0x200`) are passed through unencrypted so the loader can inspect the magic number and length prior to decryption.
- **Formats:** The bootloaders support a **Monolithic** image path (straight decrypt to SRAM) and a **Segmented** fallback path (decrypts a table of scatter-load descriptors and BSS zero-fills).

---

## 5. Firmware Decryption Tool

The included `decrypt_firmware.js` allows you to extract payloads directly from an LPC43xx SPI-NOR flash dump. It works across bootloader generations by attempting different cipher profiles.

### Prerequisites

- Node.js installed on your host machine.

### Usage

```bash
node decrypt_firmware.js <full_dump.bin> [output_directory]

```

**How it works:**

1. Pre-filters candidate key windows by validating that the resulting decrypted initial SP and reset vector are well-formed.
2. Checks the 32-bit accumulator target against the cipher profile.
3. Automatically writes decrypted `slot_*.bin` payloads and a detailed decryption report (`.txt`) for every valid firmware image it finds.

---

## 6. Build Instructions (Reconstructions)

To build either bootloader reconstruction, you will need **MCUXpresso IDE** (or LPCXpresso) and the **GNU Arm Embedded Toolchain**.

1. Create a workspace encompassing the bootloader folder, the imported `lpcspifilib`, and the `lpc_chip_43xx` LPCOpen library.
2. Link the project using the custom scatter-load script provided in `ld/lpc43xx_spifi_boot.ld`.
3. Use the `-fno-strict-aliasing`, `-Os`, and `-nostartfiles` flags to ensure the inline `Reset_Handler` routines compile precisely without jumping into non-relocated SRAM.

_For detailed compilation notes, consult the dedicated `readme.md` inside each bootloader's respective folder._
