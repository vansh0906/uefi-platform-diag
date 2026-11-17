/** @file
  UEFI Driver Model driver for the QEMU "edu" PCI device.

  Supported() matches vendor 0x1234 / device 0x11E8 through EFI_PCI_IO_PROTOCOL. Start() enables memory decode and
  bus mastering, checks the ID register, sets up a DMA bounce buffer below the device's 28-bit limit and installs
  EDU_DEVICE_PROTOCOL. Stop() undoes all of it, so the driver can be disconnected, reconnected and unloaded.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Protocol/PciIo.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/EduDevice.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DiagCoreLib.h>

#define EDU_PRIVATE_SIGNATURE  SIGNATURE_32 ('E', 'D', 'U', 'p')
#define EDU_BAR                0
#define EDU_BOUNCE_PAGES       EFI_SIZE_TO_PAGES (EDU_DMA_BUFFER_SIZE)
#define EDU_TIMEOUT_US         2000000      // device DMA completes on a 100 ms timer; factorial is immediate

typedef struct {
  UINT32                  Signature;
  EFI_HANDLE              Controller;
  EFI_PCI_IO_PROTOCOL     *PciIo;
  UINT64                  OriginalAttributes;
  EFI_PHYSICAL_ADDRESS    Bounce;           // below 256 MiB, EDU_BOUNCE_PAGES pages
  EDU_DEVICE_PROTOCOL     Edu;
} EDU_PRIVATE;

#define EDU_PRIVATE_FROM_PROTOCOL(a)  CR (a, EDU_PRIVATE, Edu, EDU_PRIVATE_SIGNATURE)

extern EFI_COMPONENT_NAME2_PROTOCOL  gEduComponentName2;
EFI_DRIVER_BINDING_PROTOCOL          gEduDriverBinding;

// ------------------------------------------------------------------------------------------------ register access

STATIC
UINT32
EduRead32 (
  IN EDU_PRIVATE  *P,
  IN UINT64       Offset
  )
{
  UINT32  Value;

  Value = 0xFFFFFFFF;
  P->PciIo->Mem.Read (P->PciIo, EfiPciIoWidthUint32, EDU_BAR, Offset, 1, &Value);
  return Value;
}

STATIC
VOID
EduWrite32 (
  IN EDU_PRIVATE  *P,
  IN UINT64       Offset,
  IN UINT32       Value
  )
{
  P->PciIo->Mem.Write (P->PciIo, EfiPciIoWidthUint32, EDU_BAR, Offset, 1, &Value);
}

STATIC
VOID
EduWrite64 (
  IN EDU_PRIVATE  *P,
  IN UINT64       Offset,
  IN UINT64       Value
  )
{
  P->PciIo->Mem.Write (P->PciIo, EfiPciIoWidthUint64, EDU_BAR, Offset, 1, &Value);
}

STATIC
EFI_STATUS
EduWaitClear (
  IN EDU_PRIVATE  *P,
  IN UINT64       Offset,
  IN UINT32       Mask
  )
{
  UINTN  Waited;

  for (Waited = 0; Waited < EDU_TIMEOUT_US; Waited += 50) {
    if ((EduRead32 (P, Offset) & Mask) == 0) {
      return EFI_SUCCESS;
    }

    gBS->Stall (50);
  }

  return EFI_TIMEOUT;
}

// ------------------------------------------------------------------------------------------------ protocol

STATIC
EFI_STATUS
EFIAPI
EduGetInfo (
  IN  EDU_DEVICE_PROTOCOL  *This,
  OUT UINT32               *IdRegister,
  OUT UINTN                *Segment,
  OUT UINTN                *Bus,
  OUT UINTN                *Device,
  OUT UINTN                *Function
  )
{
  EDU_PRIVATE  *P;

  P           = EDU_PRIVATE_FROM_PROTOCOL (This);
  *IdRegister = EduRead32 (P, EDU_REG_ID);
  return P->PciIo->GetLocation (P->PciIo, Segment, Bus, Device, Function);
}

STATIC
EFI_STATUS
EFIAPI
EduLiveness (
  IN  EDU_DEVICE_PROTOCOL  *This,
  IN  UINT32               Value,
  OUT UINT32               *Inverted
  )
{
  EDU_PRIVATE  *P;

  P = EDU_PRIVATE_FROM_PROTOCOL (This);
  EduWrite32 (P, EDU_REG_LIVENESS, Value);
  *Inverted = EduRead32 (P, EDU_REG_LIVENESS);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
EduFactorial (
  IN  EDU_DEVICE_PROTOCOL  *This,
  IN  UINT32               N,
  OUT UINT32               *Result
  )
{
  EDU_PRIVATE  *P;
  EFI_STATUS   Status;

  P = EDU_PRIVATE_FROM_PROTOCOL (This);
  EduWrite32 (P, EDU_REG_FACTORIAL, N);
  Status = EduWaitClear (P, EDU_REG_STATUS, EDU_STATUS_COMPUTING);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Result = EduRead32 (P, EDU_REG_FACTORIAL);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EduRunDma (
  IN EDU_PRIVATE  *P,
  IN BOOLEAN      ToDevice,
  IN UINT64       HostBusAddress,
  IN UINTN        DeviceOffset,
  IN UINTN        Length
  )
{
  UINT64  DeviceAddress;

  DeviceAddress = EDU_DMA_BUFFER_ADDR + DeviceOffset;
  EduWrite64 (P, EDU_REG_DMA_SRC, ToDevice ? HostBusAddress : DeviceAddress);
  EduWrite64 (P, EDU_REG_DMA_DST, ToDevice ? DeviceAddress : HostBusAddress);
  EduWrite64 (P, EDU_REG_DMA_COUNT, Length);
  EduWrite64 (P, EDU_REG_DMA_CMD, EDU_DMA_CMD_START | (ToDevice ? 0 : EDU_DMA_CMD_TO_RAM));
  return EduWaitClear (P, EDU_REG_DMA_CMD, EDU_DMA_CMD_START);
}

STATIC
EFI_STATUS
EFIAPI
EduDma (
  IN  EDU_DEVICE_PROTOCOL  *This,
  IN  BOOLEAN              ToDevice,
  IN  UINTN                DeviceOffset,
  IN  VOID                 *Buffer,
  IN  UINTN                Length,
  IN  EDU_DMA_MODE         Mode,
  OUT UINT64               *BusAddress
  )
{
  EDU_PRIVATE                    *P;
  EFI_STATUS                     Status;
  EFI_STATUS                     DmaStatus;
  EFI_PCI_IO_PROTOCOL_OPERATION  Op;
  VOID                           *Host;
  UINTN                          Mapped;
  EFI_PHYSICAL_ADDRESS           Device;
  VOID                           *Mapping;

  P = EDU_PRIVATE_FROM_PROTOCOL (This);
  if ((Buffer == NULL) || (Length == 0) || (DeviceOffset > EDU_DMA_BUFFER_SIZE) ||
      (Length > EDU_DMA_BUFFER_SIZE - DeviceOffset))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Mode == EduDmaBounce) {
    Host = (VOID *)(UINTN)P->Bounce;
    if (ToDevice) {
      CopyMem (Host, Buffer, Length);
    }
  } else {
    Host = Buffer;
  }

  Op     = ToDevice ? EfiPciIoOperationBusMasterRead : EfiPciIoOperationBusMasterWrite;
  Mapped = Length;
  Status = P->PciIo->Map (P->PciIo, Op, Host, &Mapped, &Device, &Mapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *BusAddress = Device;
  //
  // Bounce mode always checks the 28-bit limit. Direct mode is the original naive path and only skips the check for
  // host -> device transfers, where a truncated address reads the wrong data but can never overwrite memory.
  //
  if ((Mapped != Length) ||
      (((Mode == EduDmaBounce) || !ToDevice) && !DiagDmaAddressOk (Device, Length, EDU_DMA_MASK)))
  {
    P->PciIo->Unmap (P->PciIo, Mapping);
    return EFI_UNSUPPORTED;           // never program an address the device would silently truncate
  }

  DmaStatus = EduRunDma (P, ToDevice, Device, DeviceOffset, Length);
  Status    = P->PciIo->Unmap (P->PciIo, Mapping);
  if (EFI_ERROR (DmaStatus)) {
    return DmaStatus;
  }

  if (!ToDevice && (Mode == EduDmaBounce)) {
    CopyMem (Buffer, Host, Length);
  }

  return Status;
}

// ------------------------------------------------------------------------------------------------ driver binding

STATIC
EFI_STATUS
EFIAPI
EduSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT16               Ids[2];

  Status = gBS->OpenProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&PciIo, This->DriverBindingHandle,
                              Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_VENDOR_ID_OFFSET, 2, Ids);
  if (!EFI_ERROR (Status) && ((Ids[0] != EDU_VENDOR_ID) || (Ids[1] != EDU_DEVICE_ID))) {
    Status = EFI_UNSUPPORTED;
  }

  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
EduStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS   Status;
  EDU_PRIVATE  *P;
  UINT64       Supports;
  UINT8        Major;
  UINT8        Minor;
  BOOLEAN      AttributesSet;

  AttributesSet = FALSE;
  P             = AllocateZeroPool (sizeof (*P));
  if (P == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  P->Signature  = EDU_PRIVATE_SIGNATURE;
  P->Controller = Controller;
  Status        = gBS->OpenProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&P->PciIo,
                                     This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    FreePool (P);
    return Status;
  }

  Status = P->PciIo->Attributes (P->PciIo, EfiPciIoAttributeOperationGet, 0, &P->OriginalAttributes);
  if (!EFI_ERROR (Status)) {
    Status = P->PciIo->Attributes (P->PciIo, EfiPciIoAttributeOperationSupported, 0, &Supports);
  }

  if (!EFI_ERROR (Status)) {
    Supports &= (UINT64)(EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
    Status    = P->PciIo->Attributes (P->PciIo, EfiPciIoAttributeOperationEnable, Supports, NULL);
    AttributesSet = !EFI_ERROR (Status);
  }

  if (!EFI_ERROR (Status) && !DiagEduIdValid (EduRead32 (P, EDU_REG_ID), &Major, &Minor)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    P->Bounce = EDU_DMA_MASK;         // highest address allowed: the device drives only 28 address bits
    Status    = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, EDU_BOUNCE_PAGES, &P->Bounce);
  }

  if (!EFI_ERROR (Status)) {
    P->Edu.Revision  = 0x00010000;
    P->Edu.GetInfo   = EduGetInfo;
    P->Edu.Liveness  = EduLiveness;
    P->Edu.Factorial = EduFactorial;
    P->Edu.Dma       = EduDma;
    Status           = gBS->InstallMultipleProtocolInterfaces (&Controller, &gEduDeviceProtocolGuid, &P->Edu, NULL);
    if (EFI_ERROR (Status)) {
      gBS->FreePages (P->Bounce, EDU_BOUNCE_PAGES);
    }
  }

  if (EFI_ERROR (Status)) {
    if (AttributesSet) {
      P->PciIo->Attributes (P->PciIo, EfiPciIoAttributeOperationSet, P->OriginalAttributes, NULL);
    }

    gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
    FreePool (P);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "EduDxe: started on controller %p, edu v%u.%u\n", Controller, Major, Minor));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
EduStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS           Status;
  EDU_DEVICE_PROTOCOL  *Edu;
  EDU_PRIVATE          *P;

  Status = gBS->OpenProtocol (Controller, &gEduDeviceProtocolGuid, (VOID **)&Edu, This->DriverBindingHandle,
                              Controller, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  P      = EDU_PRIVATE_FROM_PROTOCOL (Edu);
  Status = gBS->UninstallMultipleProtocolInterfaces (Controller, &gEduDeviceProtocolGuid, &P->Edu, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  P->PciIo->Attributes (P->PciIo, EfiPciIoAttributeOperationSet, P->OriginalAttributes, NULL);
  gBS->FreePages (P->Bounce, EDU_BOUNCE_PAGES);
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  FreePool (P);
  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  gEduDriverBinding = {
  EduSupported,
  EduStart,
  EduStop,
  0x10,
  NULL,
  NULL
};

// ------------------------------------------------------------------------------------------------ entry / unload

EFI_STATUS
EFIAPI
EduDxeUnload (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEduDeviceProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      Status = gBS->DisconnectController (Handles[Index], ImageHandle, NULL);
      if (EFI_ERROR (Status)) {
        FreePool (Handles);
        return Status;
      }
    }

    FreePool (Handles);
  }

  return EfiLibUninstallDriverBindingComponentName2 (&gEduDriverBinding, NULL, &gEduComponentName2);
}

EFI_STATUS
EFIAPI
EduDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (ImageHandle, SystemTable, &gEduDriverBinding, ImageHandle,
                                                   NULL, &gEduComponentName2);
}
