/** @file
  PlatformDiag: a POST-style diagnostics stage that runs as \EFI\BOOT\BOOTX64.EFI.

  1. Counts boots in a non-volatile UEFI variable.
  2. Talks to the BMC over KCS: device ID, self test, last-boot watchdog status; arms the BIOS/POST watchdog.
  3. Enumerates PCI, loads \EduDxe.efi, connects it, and tests every edu device (ID, liveness, factorial, DMA).
  4. Disconnects and reconnects the driver 25 times and checks free memory for leaks; unloads it.
  5. Reads SMBIOS (BIOS, system, memory devices).
  6. Logs firmware events to the BMC's SEL, reads them back, disarms the watchdog, prints a result and powers off.

  If the file \pd_hang exists, step 3 hangs (simulating a stuck POST stage) so the BMC watchdog resets the machine;
  on the next boot the expired-timer flag puts PlatformDiag in safe mode, which logs the recovery and skips the hang.

  Every result line starts with "PD:" so tools/run_qemu.py can parse the serial log.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <IndustryStandard/SmBios.h>
#include <Guid/SmBios.h>
#include <Guid/FileInfo.h>
#include <Protocol/PciIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/EduDevice.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/DiagCoreLib.h>
#include <Library/IpmiKcsLib.h>

#define RECONNECT_CYCLES   25
#define WDT_COUNTDOWN      100            // 10 s in 100 ms units; petted between stages
#define BOOT_VARIABLE      L"PlatformDiagBoots"

STATIC UINTN     mPass;
STATIC UINTN     mFail;
STATIC KCS_PORT  mKcs;
STATIC UINT16    mKcsBase;
STATIC BOOLEAN   mBmc;

// ------------------------------------------------------------------------------------------------ reporting

STATIC
VOID
Test (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      Ok,
  IN CONST CHAR16 *Detail
  )
{
  if (Ok) {
    mPass++;
  } else {
    mFail++;
  }

  Print (L"PD: TEST %a %a %s\n", Name, Ok ? "PASS" : "FAIL", Detail);
}

STATIC
VOID
Skip (
  IN CONST CHAR8  *Name,
  IN CONST CHAR16 *Detail
  )
{
  Print (L"PD: TEST %a SKIP %s\n", Name, Detail);
}

// ------------------------------------------------------------------------------------------------ BMC

STATIC
KCS_RESULT
Bmc (
  IN  UINT8        NetFn,
  IN  UINT8        Cmd,
  IN  CONST UINT8  *Data,
  IN  UINTN        Len,
  OUT UINT8        *Cc,
  OUT UINT8        *Rsp,
  IN  UINTN        RspMax,
  OUT UINTN        *RspLen
  )
{
  KCS_RESULT  R;
  UINT8       Err;

  R = IpmiCommand (&mKcs, NetFn, Cmd, Data, Len, Cc, Rsp, RspMax, RspLen);
  if ((R == KcsBadState) || (R == KcsTimeoutObf)) {
    Print (L"PD: BMC transfer failed (%a), recovering interface\n", KcsResultName (R));
    if (KcsRecover (&mKcs, &Err) == KcsOk) {
      R = IpmiCommand (&mKcs, NetFn, Cmd, Data, Len, Cc, Rsp, RspMax, RspLen);
    }
  }

  return R;
}

STATIC
BOOLEAN
BmcLogEvent (
  IN BOOLEAN  IsError,
  IN UINT8    Code
  )
{
  IPMI_SEL_EVENT  Event;
  UINT8           Record[16];
  UINT8           Cc;
  UINT8           Rsp[4];
  UINTN           Len;

  if (!mBmc) {
    return FALSE;
  }

  IpmiFirmwareEvent (&Event, IsError, Code);
  IpmiEncodeSelEvent (&Event, Record);
  if ((Bmc (IPMI_NETFN_STORAGE, IPMI_CMD_ADD_SEL_ENTRY, Record, 16, &Cc, Rsp, sizeof (Rsp), &Len) != KcsOk) ||
      (Cc != 0) || (Len < 2))
  {
    return FALSE;
  }

  Print (L"PD: SEL logged record 0x%04x: %a\n", Rsp[0] | (Rsp[1] << 8), IpmiSelDescribe (&Event));
  return TRUE;
}

STATIC
BOOLEAN
BmcSetWatchdog (
  IN BOOLEAN  Run,
  IN UINT8    ClearFlags
  )
{
  UINT8  Req[6];
  UINT8  Cc;
  UINT8  Rsp[8];
  UINTN  Len;

  IpmiBuildSetWatchdog (Req, IPMI_WDT_USE_BIOS_POST, Run, Run ? IPMI_WDT_ACTION_HARD_RESET : IPMI_WDT_ACTION_NONE,
                        ClearFlags, WDT_COUNTDOWN);
  if ((Bmc (IPMI_NETFN_APP, IPMI_CMD_SET_WATCHDOG, Req, 6, &Cc, Rsp, sizeof (Rsp), &Len) != KcsOk) || (Cc != 0)) {
    return FALSE;
  }

  if (!Run) {
    return TRUE;                // Set Watchdog without "don't stop" stops a running timer
  }

  return (BOOLEAN)(Bmc (IPMI_NETFN_APP, IPMI_CMD_RESET_WATCHDOG, NULL, 0, &Cc, Rsp, sizeof (Rsp), &Len) == KcsOk &&
                   Cc == 0);
}

STATIC
VOID
BmcPet (
  VOID
  )
{
  UINT8  Cc;
  UINT8  Rsp[4];
  UINTN  Len;

  if (mBmc) {
    Bmc (IPMI_NETFN_APP, IPMI_CMD_RESET_WATCHDOG, NULL, 0, &Cc, Rsp, sizeof (Rsp), &Len);
  }
}

STATIC
BOOLEAN
BmcGetWatchdog (
  OUT IPMI_WATCHDOG  *Wdt
  )
{
  UINT8  Cc;
  UINT8  Rsp[16];
  UINTN  Len;

  return (BOOLEAN)(mBmc && Bmc (IPMI_NETFN_APP, IPMI_CMD_GET_WATCHDOG, NULL, 0, &Cc, Rsp, sizeof (Rsp), &Len) == KcsOk &&
                   Cc == 0 && IpmiParseGetWatchdog (Rsp, Len, Wdt));
}

// Returns TRUE if the previous boot was ended by the BIOS/POST watchdog (safe mode for this boot).
STATIC
BOOLEAN
BmcStage (
  VOID
  )
{
  UINT8           Cc;
  UINT8           Rsp[32];
  UINTN           Len;
  KCS_RESULT      R;
  IPMI_DEVICE_ID  Id;
  IPMI_WATCHDOG   Wdt;
  BOOLEAN         SafeMode;
  CHAR16          Detail[96];

  IpmiKcsPortInit (&mKcs, &mKcsBase, IPMI_KCS_DEFAULT_BASE, 5000000);
  R    = Bmc (IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &Cc, Rsp, sizeof (Rsp), &Len);
  mBmc = (BOOLEAN)(R == KcsOk && Cc == 0 && IpmiParseDeviceId (Rsp, Len, &Id));
  if (!mBmc) {
    UnicodeSPrint (Detail, sizeof (Detail), L"(%a) - continuing without BMC", KcsResultName (R));
    Test ("bmc.present", FALSE, Detail);
    return FALSE;
  }

  UnicodeSPrint (Detail, sizeof (Detail), L"device 0x%02x rev %d, firmware %d.%02d, IPMI %d.%d, mfg 0x%05x, %d status polls",
                 Id.DeviceId, Id.DeviceRevision, Id.FirmwareMajor, Id.FirmwareMinor, Id.IpmiMajor, Id.IpmiMinor,
                 Id.ManufacturerId, mKcs.Polls);
  Test ("bmc.device_id", (BOOLEAN)(Id.IpmiMajor == 2), Detail);

  R = Bmc (IPMI_NETFN_APP, IPMI_CMD_GET_SELF_TEST, NULL, 0, &Cc, Rsp, sizeof (Rsp), &Len);
  if ((R == KcsOk) && (Cc == IPMI_CC_INVALID_COMMAND)) {
    Skip ("bmc.self_test", L"BMC does not implement Get Self Test Results (completion code 0xC1)");
  } else {
    UnicodeSPrint (Detail, sizeof (Detail), L"%a, completion 0x%02x, result 0x%02x", KcsResultName (R), Cc,
                   (R == KcsOk && Len > 0) ? Rsp[0] : 0xFF);
    Test ("bmc.self_test", (BOOLEAN)(R == KcsOk && Cc == 0 && Len >= 1 && Rsp[0] == 0x55), Detail);
  }

  SafeMode = FALSE;
  if (BmcGetWatchdog (&Wdt)) {
    SafeMode = (BOOLEAN)((Wdt.ExpirationFlags & (1 << IPMI_WDT_USE_BIOS_POST)) != 0);
    Print (L"PD: WDT last boot: expiration flags 0x%02x%s\n", Wdt.ExpirationFlags,
           SafeMode ? L" - POST watchdog expired, booting in safe mode" : L"");
  }

  if (SafeMode) {
    BmcLogEvent (FALSE, IPMI_FW_PROG_WDT_RECOVERY);
  }

  // arm the POST watchdog and clear the expired flag we just consumed
  Test ("bmc.watchdog_arm", BmcSetWatchdog (TRUE, (UINT8)(1 << IPMI_WDT_USE_BIOS_POST)), L"BIOS/POST timer, 10 s, hard reset");
  return SafeMode;
}

// ------------------------------------------------------------------------------------------------ files and boot count

STATIC
EFI_FILE_PROTOCOL *
OpenRoot (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL        *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)) ||
      EFI_ERROR (gBS->HandleProtocol (Loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
      EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
  {
    return NULL;
  }

  return Root;
}

STATIC
BOOLEAN
FileExists (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name
  )
{
  EFI_FILE_PROTOCOL  *File;

  if ((Root == NULL) || EFI_ERROR (Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0))) {
    return FALSE;
  }

  File->Close (File);
  return TRUE;
}

STATIC
UINT32
CountBoot (
  VOID
  )
{
  UINT32      Count;
  UINTN       Size;
  EFI_STATUS  Status;

  Size   = sizeof (Count);
  Status = gRT->GetVariable (BOOT_VARIABLE, &gPlatformDiagVariableGuid, NULL, &Size, &Count);
  if (EFI_ERROR (Status) || (Size != sizeof (Count))) {
    Count = 0;
  }

  Count++;
  Status = gRT->SetVariable (BOOT_VARIABLE, &gPlatformDiagVariableGuid,
                             EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                             sizeof (Count), &Count);
  Print (L"PD: BOOT count=%d variable=%r\n", Count, Status);
  return Count;
}

// ------------------------------------------------------------------------------------------------ memory accounting

STATIC EFI_MEMORY_DESCRIPTOR  *mMap;
STATIC UINTN                  mMapCapacity;

STATIC
UINT64
FreeMemoryPages (
  VOID
  )
{
  UINTN                  Size;
  UINTN                  Key;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  EFI_MEMORY_DESCRIPTOR  *D;
  UINT64                 Free;

  Size = mMapCapacity;
  if (EFI_ERROR (gBS->GetMemoryMap (&Size, mMap, &Key, &DescSize, &DescVersion))) {
    return 0;
  }

  Free = 0;
  for (D = mMap; (UINT8 *)D < (UINT8 *)mMap + Size; D = NEXT_MEMORY_DESCRIPTOR (D, DescSize)) {
    if (D->Type == EfiConventionalMemory) {
      Free += D->NumberOfPages;
    }
  }

  return Free;
}

// ------------------------------------------------------------------------------------------------ PCI and the edu driver

STATIC
UINTN
PciStage (
  VOID
  )
{
  EFI_HANDLE           *Handles;
  UINTN                Count;
  UINTN                Index;
  UINTN                EduCount;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT16               Ids[2];
  UINT8                Class[3];
  UINTN                Seg, Bus, Dev, Fn;

  EduCount = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles))) {
    Print (L"PD: PCI no devices\n");
    return 0;
  }

  for (Index = 0; Index < Count; Index++) {
    gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid, (VOID **)&PciIo);
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_VENDOR_ID_OFFSET, 2, Ids);
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, PCI_CLASSCODE_OFFSET, 3, Class);
    PciIo->GetLocation (PciIo, &Seg, &Bus, &Dev, &Fn);
    Print (L"PD: PCI %02x:%02x.%x %04x:%04x class %02x%02x%02x\n", Bus, Dev, Fn, Ids[0], Ids[1], Class[2], Class[1],
           Class[0]);
    if ((Ids[0] == EDU_VENDOR_ID) && (Ids[1] == EDU_DEVICE_ID)) {
      EduCount++;
    }
  }

  FreePool (Handles);
  Print (L"PD: PCI devices=%d edu=%d\n", Count, EduCount);
  return EduCount;
}

STATIC
EFI_STATUS
LoadDriver (
  IN  EFI_HANDLE  ImageHandle,
  OUT EFI_HANDLE  *DriverImage
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  EFI_DEVICE_PATH_PROTOCOL   *Path;
  EFI_STATUS                 Status;

  gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  Path = FileDevicePath (Loaded->DeviceHandle, L"\\EduDxe.efi");
  if (Path == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, DriverImage);
  FreePool (Path);
  if (!EFI_ERROR (Status)) {
    Status = gBS->StartImage (*DriverImage, NULL, NULL);
  }

  return Status;
}

STATIC
UINTN
ConnectAllPci (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles))) {
    return 0;
  }

  for (Index = 0; Index < Count; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
  }

  FreePool (Handles);
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEduDeviceProtocolGuid, NULL, &Count, &Handles))) {
    return 0;
  }

  FreePool (Handles);
  return Count;
}

STATIC
VOID
TestOneEdu (
  IN UINTN                Number,
  IN EDU_DEVICE_PROTOCOL  *Edu,
  IN BOOLEAN              HighMemory
  )
{
  STATIC CONST UINT32   Patterns[] = { 0x00000000, 0xFFFFFFFF, 0xA5A55A5A, 0x12345678 };
  CHAR8                 Name[32];
  CHAR16                Detail[128];
  UINT32                Id, Inv, Result;
  UINT8                 Major, Minor;
  UINTN                 Seg, Bus, Dev, Fn, Index, Bad;
  UINT8                 *Out, *Back;
  UINT64                Bus1, Bus2;
  EFI_STATUS            S1, S2;
  EFI_PHYSICAL_ADDRESS  High;

  Edu->GetInfo (Edu, &Id, &Seg, &Bus, &Dev, &Fn);
  AsciiSPrint (Name, sizeof (Name), "edu%d.id", Number);
  UnicodeSPrint (Detail, sizeof (Detail), L"%02x:%02x.%x id 0x%08x", Bus, Dev, Fn, Id);
  Test (Name, DiagEduIdValid (Id, &Major, &Minor), Detail);

  Bad = 0;
  for (Index = 0; Index < ARRAY_SIZE (Patterns); Index++) {
    Edu->Liveness (Edu, Patterns[Index], &Inv);
    Bad += (Inv != (UINT32) ~Patterns[Index]);
  }

  AsciiSPrint (Name, sizeof (Name), "edu%d.liveness", Number);
  UnicodeSPrint (Detail, sizeof (Detail), L"%d/%d patterns inverted", ARRAY_SIZE (Patterns) - Bad, ARRAY_SIZE (Patterns));
  Test (Name, (BOOLEAN)(Bad == 0), Detail);

  Bad = 0;
  for (Index = 0; Index <= 15; Index++) {
    if (EFI_ERROR (Edu->Factorial (Edu, (UINT32)Index, &Result)) || (Result != DiagFactorial32 ((UINT32)Index))) {
      Bad++;
    }
  }

  AsciiSPrint (Name, sizeof (Name), "edu%d.factorial", Number);
  UnicodeSPrint (Detail, sizeof (Detail), L"%d/16 results match (n = 0..15, 32-bit wrap)", 16 - Bad);
  Test (Name, (BOOLEAN)(Bad == 0), Detail);

  // DMA round trip through the bounce buffer: 4 KiB out to the device and back
  Out  = AllocatePool (EDU_DMA_BUFFER_SIZE);
  Back = AllocateZeroPool (EDU_DMA_BUFFER_SIZE);
  for (Index = 0; Index < EDU_DMA_BUFFER_SIZE; Index++) {
    Out[Index] = DiagPatternByte ((UINT32)(0x5EED + Number), Index);
  }

  S1  = Edu->Dma (Edu, TRUE, 0, Out, EDU_DMA_BUFFER_SIZE, EduDmaBounce, &Bus1);
  S2  = Edu->Dma (Edu, FALSE, 0, Back, EDU_DMA_BUFFER_SIZE, EduDmaBounce, &Bus2);
  Bad = (EFI_ERROR (S1) || EFI_ERROR (S2)) ? EDU_DMA_BUFFER_SIZE : 0;
  for (Index = 0; Bad == 0 && Index < EDU_DMA_BUFFER_SIZE; Index++) {
    Bad += (Out[Index] != Back[Index]);
  }

  AsciiSPrint (Name, sizeof (Name), "edu%d.dma_bounce", Number);
  UnicodeSPrint (Detail, sizeof (Detail), L"4096 B out and back, host 0x%lx, bus 0x%lx, %r/%r", (UINT64)(UINTN)Out, Bus1, S1, S2);
  Test (Name, (BOOLEAN)(Bad == 0), Detail);

  // The original direct-mapped path, with a buffer at the top of RAM: report what the device actually received.
  High = MAX_UINT32;
  if (!EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &High))) {
    CopyMem ((VOID *)(UINTN)High, Out, EFI_PAGE_SIZE);
    S1 = Edu->Dma (Edu, TRUE, 0, (VOID *)(UINTN)High, EFI_PAGE_SIZE, EduDmaDirect, &Bus1);
    ZeroMem (Back, EFI_PAGE_SIZE);
    S2  = Edu->Dma (Edu, FALSE, 0, Back, EFI_PAGE_SIZE, EduDmaBounce, &Bus2);
    Bad = 0;
    for (Index = 0; Index < EFI_PAGE_SIZE; Index++) {
      Bad += (Out[Index] != Back[Index]);
    }

    Print (L"PD: DMA direct host 0x%lx above_28bit=%d bytes_wrong=%d\n", High,
           DiagDmaAddressOk (High, EFI_PAGE_SIZE, EDU_DMA_MASK) ? 0 : 1, Bad);
    // the driver must refuse a direct device->RAM transfer it would truncate (it would overwrite other memory)
    S2 = Edu->Dma (Edu, FALSE, 0, (VOID *)(UINTN)High, EFI_PAGE_SIZE, EduDmaDirect, &Bus2);
    AsciiSPrint (Name, sizeof (Name), "edu%d.dma_guard", Number);
    UnicodeSPrint (Detail, sizeof (Detail), L"direct write to 0x%lx: %r", High, S2);
    Test (Name, DiagDmaAddressOk (High, EFI_PAGE_SIZE, EDU_DMA_MASK) ? (BOOLEAN)!EFI_ERROR (S2)
                                                                       : (BOOLEAN)(S2 == EFI_UNSUPPORTED), Detail);
    gBS->FreePages (High, 1);
  }

  FreePool (Out);
  FreePool (Back);
  (VOID)HighMemory;
}

STATIC
VOID
DriverLifecycle (
  IN EFI_HANDLE  DriverImage,
  IN UINTN       Expected
  )
{
  EFI_HANDLE  *Handles;
  EFI_HANDLE  Drivers[2];
  UINTN       Count;
  UINTN       Cycle;
  UINTN       Index;
  UINTN       Failures;
  UINT64      Before;
  UINT64      After;
  CHAR16      Detail[96];
  EFI_STATUS  Status;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEduDeviceProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  Drivers[0] = DriverImage;      // ConnectController takes a NULL-terminated list of driver images
  Drivers[1] = NULL;
  Failures   = 0;
  Before     = 0;
  for (Cycle = 0; Cycle <= RECONNECT_CYCLES; Cycle++) {
    if (Cycle == 1) {
      Before = FreeMemoryPages ();      // after one warm-up cycle, so pool growth on first use is not counted
    }

    for (Index = 0; Index < Count; Index++) {
      Failures += EFI_ERROR (gBS->DisconnectController (Handles[Index], DriverImage, NULL)) ? 1 : 0;
      Failures += EFI_ERROR (gBS->ConnectController (Handles[Index], Drivers, NULL, FALSE)) ? 1 : 0;
    }
  }

  After = FreeMemoryPages ();
  FreePool (Handles);
  UnicodeSPrint (Detail, sizeof (Detail), L"%d cycles x %d devices, %d failures", RECONNECT_CYCLES, Count, Failures);
  Test ("driver.reconnect", (BOOLEAN)(Failures == 0 && ConnectAllPci () == Expected), Detail);
  UnicodeSPrint (Detail, sizeof (Detail), L"free pages %ld -> %ld (delta %ld)", Before, After, (INT64)(After - Before));
  Test ("driver.no_leak", (BOOLEAN)(After == Before), Detail);

  Status = gBS->UnloadImage (DriverImage);
  Count  = 0;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEduDeviceProtocolGuid, NULL, &Count, &Handles))) {
    FreePool (Handles);
  }

  UnicodeSPrint (Detail, sizeof (Detail), L"UnloadImage %r, %d EDU protocols left", Status, Count);
  Test ("driver.unload", (BOOLEAN)(!EFI_ERROR (Status) && Count == 0), Detail);
}

// ------------------------------------------------------------------------------------------------ SMBIOS

STATIC
VOID
SmbiosStage (
  VOID
  )
{
  SMBIOS_TABLE_3_0_ENTRY_POINT  *Eps3;
  SMBIOS_TABLE_ENTRY_POINT      *Eps2;
  CONST UINT8                   *Table;
  UINTN                         TableLength;
  UINTN                         Len;
  CONST UINT8                   *S;
  UINTN                         Index;
  UINT64                        TotalMb;
  UINT16                        Size;
  CHAR16                        Detail[128];

  // SMBIOS 3.0 (64-bit entry point) if the firmware publishes it, otherwise the 2.x (32-bit) entry point
  if (!EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiSmbios3TableGuid, (VOID **)&Eps3))) {
    UnicodeSPrint (Detail, sizeof (Detail), L"SMBIOS 3.0 entry point, version %d.%d, table 0x%lx, up to %d bytes",
                   Eps3->MajorVersion, Eps3->MinorVersion, Eps3->TableAddress, Eps3->TableMaximumSize);
    Test ("smbios.entry_point", SmbiosEntryPoint3Valid ((CONST UINT8 *)Eps3, sizeof (*Eps3)), Detail);
    Table       = (CONST UINT8 *)(UINTN)Eps3->TableAddress;
    TableLength = Eps3->TableMaximumSize;
  } else if (!EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiSmbiosTableGuid, (VOID **)&Eps2))) {
    UnicodeSPrint (Detail, sizeof (Detail), L"SMBIOS 2.x entry point, version %d.%d, table 0x%x, %d bytes",
                   Eps2->MajorVersion, Eps2->MinorVersion, Eps2->TableAddress, Eps2->TableLength);
    Test ("smbios.entry_point", SmbiosEntryPoint2Valid ((CONST UINT8 *)Eps2, sizeof (*Eps2)), Detail);
    Table       = (CONST UINT8 *)(UINTN)Eps2->TableAddress;
    TableLength = Eps2->TableLength;
  } else {
    Test ("smbios.entry_point", FALSE, L"no SMBIOS table published");
    return;
  }

  S = SmbiosFind (Table, TableLength, 0, 0, &Len);
  if (S != NULL) {
    Print (L"PD: SMBIOS bios vendor=\"%a\" version=\"%a\" date=\"%a\"\n", SmbiosString (S, Len, S[4]),
           SmbiosString (S, Len, S[5]), SmbiosString (S, Len, S[8]));
  }

  S = SmbiosFind (Table, TableLength, 1, 0, &Len);
  if (S != NULL) {
    Print (L"PD: SMBIOS system manufacturer=\"%a\" product=\"%a\"\n", SmbiosString (S, Len, S[4]),
           SmbiosString (S, Len, S[5]));
  }

  TotalMb = 0;
  for (Index = 0; (S = SmbiosFind (Table, TableLength, 17, Index, &Len)) != NULL; Index++) {
    Size = (UINT16)(S[0x0C] | (S[0x0D] << 8));
    if ((Size == 0x7FFF) && (S[1] >= 0x20)) {
      TotalMb += (UINT32)(S[0x1C] | (S[0x1D] << 8) | (S[0x1E] << 16) | ((UINT32)S[0x1F] << 24));
    } else if (Size != 0xFFFF) {
      TotalMb += (Size & 0x8000) ? (Size & 0x7FFF) / 1024 : Size;   // bit 15 set: size in KiB
    }
  }

  Print (L"PD: SMBIOS memory devices=%d total_mb=%ld\n", Index, TotalMb);
  UnicodeSPrint (Detail, sizeof (Detail), L"%d type-17 records, %ld MiB", Index, TotalMb);
  Test ("smbios.memory", (BOOLEAN)(Index > 0 && TotalMb > 0), Detail);
}

// ------------------------------------------------------------------------------------------------ SEL readback

STATIC
VOID
SelStage (
  VOID
  )
{
  UINT8           Req[6];
  UINT8           Rsp[32];
  UINT8           Cc;
  UINTN           Len;
  UINT16          RecordId;
  UINT16          Entries;
  UINTN           Read;
  UINTN           Ours;
  IPMI_SEL_EVENT  Event;
  BOOLEAN         LastIsBoot;
  CHAR16          Detail[96];

  if (!mBmc) {
    return;
  }

  if ((Bmc (IPMI_NETFN_STORAGE, IPMI_CMD_GET_SEL_INFO, NULL, 0, &Cc, Rsp, sizeof (Rsp), &Len) != KcsOk) || (Cc != 0) ||
      (Len < 3))
  {
    Test ("sel.readback", FALSE, L"Get SEL Info failed");
    return;
  }

  Entries    = (UINT16)(Rsp[1] | (Rsp[2] << 8));
  RecordId   = 0x0000;
  Read       = 0;
  Ours       = 0;
  LastIsBoot = FALSE;
  while (Read < Entries && Read < 256) {
    Req[0] = 0;
    Req[1] = 0;
    Req[2] = (UINT8)(RecordId & 0xFF);
    Req[3] = (UINT8)(RecordId >> 8);
    Req[4] = 0;
    Req[5] = 0xFF;
    if ((Bmc (IPMI_NETFN_STORAGE, IPMI_CMD_GET_SEL_ENTRY, Req, 6, &Cc, Rsp, sizeof (Rsp), &Len) != KcsOk) ||
        (Cc != 0) || (Len < 18))
    {
      break;
    }

    IpmiDecodeSelEvent (Rsp + 2, &Event);
    Print (L"PD: SEL 0x%04x gen 0x%04x sensor 0x%02x %a\n", Event.RecordId, Event.GeneratorId, Event.SensorType,
           IpmiSelDescribe (&Event));
    if (Event.GeneratorId == 0x0001) {
      Ours++;
    }

    LastIsBoot = (BOOLEAN)(Event.SensorType == IPMI_SENSOR_TYPE_FW_PROGRESS && Event.EventData[1] == IPMI_FW_PROG_OS_BOOT);
    Read++;
    RecordId = (UINT16)(Rsp[0] | (Rsp[1] << 8));
    if (RecordId == 0xFFFF) {
      break;
    }
  }

  UnicodeSPrint (Detail, sizeof (Detail), L"%d entries read, %d from firmware, last = boot", Read, Ours);
  Test ("sel.readback", (BOOLEAN)(Read == Entries && Ours >= 2 && LastIsBoot), Detail);
}

// ------------------------------------------------------------------------------------------------ main

EFI_STATUS
EFIAPI
PlatformDiagMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_FILE_PROTOCOL    *Root;
  BOOLEAN              SafeMode;
  UINTN                EduExpected;
  UINTN                EduBound;
  EFI_HANDLE           DriverImage;
  EFI_STATUS           Status;
  EFI_HANDLE           *Handles;
  UINTN                Count;
  UINTN                Index;
  EDU_DEVICE_PROTOCOL  *Edu;
  IPMI_WATCHDOG        Wdt;
  CHAR16               Detail[96];

  gBS->SetWatchdogTimer (0, 0, 0, NULL);  // the BMC watchdog supervises this stage instead of the UEFI one
  mMapCapacity = 64 * 1024;
  mMap         = AllocatePool (mMapCapacity);

  Print (L"\nPD: START PlatformDiag 1.0 firmware=\"%s\" rev 0x%x uefi %d.%d\n", gST->FirmwareVendor,
         gST->FirmwareRevision, gST->Hdr.Revision >> 16, (gST->Hdr.Revision & 0xFFFF) / 10);
  CountBoot ();
  Root     = OpenRoot (ImageHandle);
  SafeMode = BmcStage ();

  // ---- PCI enumeration and driver
  BmcLogEvent (FALSE, IPMI_FW_PROG_PCI_INIT);
  EduExpected = PciStage ();
  if (FileExists (Root, L"\\pd_hang")) {
    if (!SafeMode) {
      Print (L"PD: HANG simulating a stuck PCI init stage (POST watchdog armed)\n");
      for ( ; ;) {
        CpuPause ();
      }
    }

    Print (L"PD: SAFE MODE skipping the stage that hung on the previous boot\n");
  }

  BmcPet ();
  if (EduExpected == 0) {
    BmcLogEvent (TRUE, IPMI_FW_ERR_PCI_DEVICE);
    Test ("edu.present", FALSE, L"no edu device on the PCI bus (logged to SEL)");
  }

  Status = LoadDriver (ImageHandle, &DriverImage);
  UnicodeSPrint (Detail, sizeof (Detail), L"LoadImage/StartImage \\EduDxe.efi: %r", Status);
  Test ("driver.load", (BOOLEAN)!EFI_ERROR (Status), Detail);
  if (!EFI_ERROR (Status)) {
    EduBound = ConnectAllPci ();
    UnicodeSPrint (Detail, sizeof (Detail), L"%d of %d edu devices bound", EduBound, EduExpected);
    Test ("driver.bind", (BOOLEAN)(EduBound == EduExpected), Detail);
    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEduDeviceProtocolGuid, NULL, &Count, &Handles))) {
      for (Index = 0; Index < Count; Index++) {
        gBS->HandleProtocol (Handles[Index], &gEduDeviceProtocolGuid, (VOID **)&Edu);
        TestOneEdu (Index, Edu, FALSE);
        BmcPet ();
      }

      FreePool (Handles);
    }

    if (EduBound > 0) {
      DriverLifecycle (DriverImage, EduBound);
    }

    BmcPet ();
  }

  // ---- SMBIOS
  SmbiosStage ();

  // ---- hand-off: log, read back, disarm
  BmcLogEvent (FALSE, IPMI_FW_PROG_OS_BOOT);
  SelStage ();
  if (mBmc) {
    Status = BmcSetWatchdog (FALSE, 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    ZeroMem (&Wdt, sizeof (Wdt));
    BmcGetWatchdog (&Wdt);
    UnicodeSPrint (Detail, sizeof (Detail), L"%r, timer use 0x%02x, countdown left %d", Status, Wdt.TimerUse,
                   Wdt.PresentCountdown);
    Test ("bmc.watchdog_disarm",
          (BOOLEAN)(!EFI_ERROR (Status) && (Wdt.TimerUse & IPMI_WDT_DONT_STOP) == 0 && Wdt.PresentCountdown == 0), Detail);
  }

  Print (L"PD: RESULT %a %d/%d%s\n", mFail == 0 ? "PASS" : "FAIL", mPass, mPass + mFail, SafeMode ? L" (safe mode)" : L"");
  if (Root != NULL) {
    Root->Close (Root);
  }

  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  return EFI_SUCCESS;
}
