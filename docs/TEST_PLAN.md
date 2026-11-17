# Test plan

Three layers. Each check has an ID that appears in the serial log (`PD: TEST <id> PASS|FAIL|SKIP <detail>`).

## 1. Host unit tests (`make -C tests/host check`)

DiagCoreLib compiled with gcc under AddressSanitizer + UBSan, against a simulated KCS BMC that follows QEMU's
state machine.

| Area | Cases |
|---|---|
| KCS transfer | normal transfer; interface left idle afterwards; unsupported command returns a completion code |
| KCS faults | no BMC (0xFF): fails with no polling; stuck IBF: fails after the timeout; BMC error on a data byte: bad state, then GET_STATUS/ABORT recovery, then success; abort from idle; mismatched response; slow BMC (5 ms) still inside the timeout; response longer than the caller's buffer; request too long |
| IPMI encoding | SEL record encode/decode round trip; descriptions; Set Watchdog request bytes; Get Watchdog and Get Device ID parsing (including short responses) |
| edu helpers | factorial with 32-bit wrap (13!); ID register; DMA-mask check incl. a buffer crossing 256 MiB and address wrap |
| SMBIOS | structure walk, strings, structures without strings, counts; every truncation of a table; 20,000 random mutations: never a pointer outside the table; 2.x and 3.0 entry-point checksums |

## 2. Firmware checks (PlatformDiag.efi on OVMF)

| ID | Pass condition |
|---|---|
| bmc.present / bmc.device_id | Get Device ID answers with IPMI 2.0 |
| bmc.self_test | 0x55 (no error); SKIP if the BMC returns 0xC1 |
| bmc.watchdog_arm | Set + Reset Watchdog accepted (BIOS/POST use, 10 s, hard reset) |
| edu.present | an edu device on the PCI bus (otherwise logged to the SEL as a firmware error) |
| driver.load / driver.bind | LoadImage + StartImage succeed; every edu device gets EDU_DEVICE_PROTOCOL |
| eduN.id / liveness / factorial | ID register 0x010000ED; 4 patterns inverted; n = 0..15 match |
| eduN.dma_bounce | 4 KiB pattern to the device and back, byte-exact |
| eduN.dma_guard | a direct device-to-RAM transfer above the DMA limit is refused |
| driver.reconnect / driver.no_leak / driver.unload | 25 cycles with 0 failures; free pages unchanged; 0 protocols after UnloadImage |
| smbios.entry_point / smbios.memory | valid 3.0 or 2.x entry point; type-17 memory present |
| sel.readback | every SEL entry read; at least 2 from firmware; the last is the OS-boot progress event |
| bmc.watchdog_disarm | timer stopped (no countdown left) |

## 3. Scenarios (`python3 tools/run_qemu.py`)

| Scenario | QEMU setup | Expected |
|---|---|---|
| baseline | q35, 256 MiB, 1 edu, BMC | all pass; self test SKIP; SMBIOS reports 256 MiB; 0 pages leaked |
| high_memory | 1 GiB | all pass; direct DMA from above 256 MiB corrupts data; guard refuses the direct write; SMBIOS 1024 MiB |
| two_devices | 2 edu | 2 of 2 bound and tested; reconnect covers both |
| no_device | 0 edu | edu.present FAIL; SEL shows "expected PCI device missing"; driver still loads |
| no_bmc | no BMC | bmc.present FAIL ("no BMC"); every other check passes |
| hang | `\pd_hang` on the ESP | two boots; first hangs; reset 8–25 s later; second in safe mode, SEL has the recovery event, all pass |
| persistence | same NV store, two power-ons | boot count 1 then 2 |
| smbios3 | `smbios-entry-point-type=64` | 3.0 entry-point path used; all pass |
