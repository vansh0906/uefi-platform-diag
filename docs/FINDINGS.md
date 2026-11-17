# Findings

Numbers come from `tools/run_qemu.py` (logs in `results/logs/`) and `make -C tests/host check`.

| ID | Finding | Evidence | Change |
|---|---|---|---|
| F-01 | **The device silently truncates DMA addresses above 256 MiB.** The `edu` device drives only 28 address bits, and UEFI allocates from the top of RAM, so with 1 GiB the buffers land above its reach. Nothing fails: the device reads the wrong memory. | 1 GiB, buffer at 0x3DE24000, direct mapping: **4,084 of 4,096 bytes wrong**. At 256 MiB the same path is correct. | The driver owns a bounce buffer allocated with `AllocateMaxAddress` at 0x0FFFFFFF and checks every mapped bus address against the mask before programming the device. A direct device-to-RAM transfer it would truncate is refused (`EFI_UNSUPPORTED`), because it would overwrite other memory. |
| F-02 | **A hung POST stage needs the BMC to recover the machine, and the next boot needs to know why it restarted.** | POST hang: BMC hard reset, next boot **12.3 s** after the hang (10 s timer + about 2.3 s of firmware boot). It read expiration flag 0x04 (BIOS/POST), logged "safe-mode boot after watchdog reset" to the SEL and skipped the hung stage: **16/16**. | Arm the BIOS/POST watchdog early, pet it between stages, disarm it at hand-off. Clear the expired flag when arming, so one timeout causes one safe-mode boot. |
| F-03 | **"Command not supported" is not a failure.** QEMU's BMC does not implement Get Self Test Results. | Completion code 0xC1 on every boot. | Transport errors (timeouts, bad state) and IPMI completion codes are separate results; 0xC1 on an optional command is reported as SKIP. |
| F-04 | **The firmware published SMBIOS 2.x, not 3.0.** The first version looked only for the 3.0 (64-bit) entry point and failed on the first boot. | `smbios.entry_point` FAIL on the first QEMU run; the q35 machine defaults to the 32-bit "_SM_" entry point. | Both paths, each with its checksums (2.x has two: "_SM_" and "_DMI_"). The `smbios3` scenario forces the 3.0 path. Memory from type-17 records matches the configured RAM (256 and 1024 MiB). |
| F-05 | **The driver's Stop() must undo everything Start() did.** | 25 disconnect/connect cycles: free pages **52,826 → 52,826**; two devices: 52,824 → 52,824. `UnloadImage` leaves 0 protocol instances. | Start() saves and Stop() restores the PCI attributes, frees the bounce pages and the private context, and closes PCI I/O. The check counts free pages from the memory map after one warm-up cycle, so pool growth on first use isn't counted as a leak. |
| F-06 | **Missing hardware must be reported, not hidden.** | No BMC: the KCS status port reads 0xFF, so it fails at once with no 5 s timeout, and the other 12 checks still pass. No edu device: "expected PCI device missing" logged to the SEL. | Fail fast on an undecoded port; log POST errors to the BMC so they're visible out of band. |
| F-07 | **KCS transfers must survive a BMC that errors or stalls.** | Host tests against a simulated BMC: error on the first data byte → GET_STATUS/ABORT recovery → the next command succeeds. Stuck IBF → bounded timeout. Wrong response → rejected. 20,000 mutated SMBIOS tables and every truncation: no out-of-bounds read under AddressSanitizer. | Every wait is bounded; on a bad state the platform code runs the recovery sequence once and retries. |
| F-08 | **One of my unit tests was wrong.** It compared two structs with `memcmp`, which also compares their padding. GCC passed it, clang failed it. | `test_diagcore.c:151` failed under clang. | Zero both structs first. The host tests now run under GCC with sanitizers and under clang. |

## Next steps

- Build PlatformDiag's checks into a DXE driver inside a custom OVMF image, instead of a boot application.
- Use the IPMI SSIF or BT interfaces as well as KCS, and the real Get Self Test Results on a BMC that supports it.
- Use an interrupt-driven DMA completion (the device can raise MSI) instead of polling.
