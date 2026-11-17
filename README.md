# UEFI platform diagnostics: an EDK2 PCI driver, a BMC client and a POST stage, booted on OVMF

[![ci](https://github.com/vansh0906/uefi-platform-diag/actions/workflows/ci.yml/badge.svg)](https://github.com/vansh0906/uefi-platform-diag/actions/workflows/ci.yml)

Server-style firmware work in C with EDK2, run on real UEFI firmware (OVMF) in QEMU:

- **EduDxe.efi**: a UEFI Driver Model driver (Supported/Start/Stop, Component Name 2, unload) for a PCI device
  (QEMU's `edu`): MMIO through `EFI_PCI_IO_PROTOCOL`, bus-master DMA with a bounce buffer, its own protocol.
- **IPMI over KCS**: a KCS system-interface driver (IPMI v2.0 section 9) to the BMC: device ID, watchdog,
  System Event Log. Error recovery (GET_STATUS/ABORT) and timeouts are tested against a simulated BMC with
  injected faults.
- **PlatformDiag.efi**: a POST-style stage that boots as `BOOTX64.EFI`. It arms the BMC's BIOS/POST watchdog,
  enumerates PCI, loads and tests the driver, checks for memory leaks over 25 reconnect cycles, reads SMBIOS,
  keeps a boot counter in an NV variable, logs firmware events to the SEL and reads them back.
- **Test plan**: 8 hardware scenarios (memory size, device count, missing device, no BMC, POST hang, power
  cycles, SMBIOS 3.0) run automatically, plus host unit tests under AddressSanitizer.

**Emulated:** the hardware (QEMU's q35 machine, `edu` PCI device and `ipmi-bmc-sim` BMC). **Real:** the UEFI
firmware (OVMF, built from EDK2), the EDK2 build, the driver and application binaries, and every number below.

## Results

| Scenario | What it checks | Result |
|---|---|---|
| baseline (256 MiB) | BMC, PCI, driver bind, ID/liveness/factorial, 4 KiB DMA both ways, 25 reconnects, unload, SMBIOS, SEL | **16/16**; **0 pages leaked** |
| high memory (1 GiB) | DMA buffers above the device's 28-bit (256 MiB) limit | naive direct DMA: **4,084 of 4,096 bytes wrong**; bounce buffer: correct; unsafe direct write **refused** |
| two devices | driver binds and tests both | **21/21** |
| device missing | POST error detected and **logged to the BMC SEL** | detected |
| no BMC | KCS port floats 0xFF: fail fast, finish POST without the BMC | detected, 12 other checks pass |
| POST hang | stage hangs → BMC watchdog **hard-resets after 10 s** → next boot reads the expired flag, logs recovery, **safe mode** | recovered, **16/16**; next boot 12.3 s after the hang |
| power cycles | boot counter in a non-volatile UEFI variable | **1 then 2** |
| SMBIOS 3.0 | 64-bit entry point path (default is 2.x) | 16/16 |

Host unit tests: 63 checks of the KCS state machine (no BMC, stuck IBF, BMC error mid-transfer with recovery,
wrong response, slow BMC, buffer limits), IPMI encoding and SMBIOS parsing, plus **20,000 mutated SMBIOS tables**
and every truncation under AddressSanitizer + UBSan: 0 failures.

Full table: [results/RESULTS.md](results/RESULTS.md) · serial logs: [results/logs/](results/logs/) ·
[docs/TEST_PLAN.md](docs/TEST_PLAN.md) · [docs/FINDINGS.md](docs/FINDINGS.md)

```
[   2.28] PD: HANG simulating a stuck PCI init stage (POST watchdog armed)
[  14.67] PD: START PlatformDiag 1.0 firmware="EDK II" rev 0x10000 uefi 2.7
[  14.68] PD: WDT last boot: expiration flags 0x04 - POST watchdog expired, booting in safe mode
[  14.68] PD: SEL logged record 0x0001: Firmware progress: safe-mode boot after watchdog reset
          ...
[  15.29] PD: RESULT PASS 16/16 (safe mode)
```

## Run it

On a Mac (uses the committed `prebuilt/` EFI files; Homebrew's QEMU ships OVMF):

```
brew install qemu
python3 tools/run_qemu.py            # 8 scenarios, about 40 s
make -C tests/host check             # host unit tests
```

Build the firmware from source (Linux, as CI does):

```
sudo apt-get install -y qemu-system-x86 ovmf nasm uuid-dev build-essential
git clone --depth 1 --branch edk2-stable202411 https://github.com/tianocore/edk2.git ../edk2
git -C ../edk2 submodule update --init --depth 1 BaseTools/Source/C/BrotliCompress/brotli \
    MdeModulePkg/Library/BrotliCustomDecompressLib/brotli MdePkg/Library/MipiSysTLib/mipisyst
tools/build.sh ../edk2               # -> build/EduDxe.efi, build/PlatformDiag.efi
python3 tools/run_qemu.py
```

## Layout

```
HwDiagPkg/HwDiagPkg.dec, .dsc               EDK2 package and platform description (X64, GCC5, -Werror)
HwDiagPkg/Drivers/EduDxe/                   UEFI Driver Model PCI driver + Component Name 2
HwDiagPkg/Include/Protocol/EduDevice.h      the protocol the driver produces
HwDiagPkg/Library/DiagCoreLib/              hardware-independent logic: KCS state machine, IPMI, SMBIOS, edu helpers
HwDiagPkg/Library/IpmiKcsLib/               KCS port I/O (0xCA2/0xCA3) for the state machine
HwDiagPkg/Application/PlatformDiag/         POST-style diagnostics application
tests/host/                                 DiagCoreLib on the host: simulated BMC with fault injection, SMBIOS fuzzing
tools/build.sh                              EDK2 build
tools/run_qemu.py                           boots each scenario in QEMU + OVMF and checks the serial output
prebuilt/                                   EduDxe.efi and PlatformDiag.efi (DEBUG build)
```

## Limits

The hardware is emulated: `edu` is QEMU's teaching device and `ipmi-bmc-sim` is a simulated BMC. PlatformDiag
runs as a boot application on top of OVMF, not as a DXE phase inside a custom firmware image. Next steps are in
[docs/FINDINGS.md](docs/FINDINGS.md).
