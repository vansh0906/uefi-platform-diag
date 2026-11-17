/** @file
  EDU_DEVICE_PROTOCOL: produced by EduDxe on each QEMU edu PCI device it manages.

  SPDX-License-Identifier: MIT
**/

#ifndef EDU_DEVICE_PROTOCOL_H_
#define EDU_DEVICE_PROTOCOL_H_

#define EDU_DEVICE_PROTOCOL_GUID \
  { 0x8fafd8f3, 0x12ba, 0x4729, { 0xaf, 0x45, 0x01, 0xcc, 0x3c, 0x5e, 0xaa, 0x1f } }

typedef struct _EDU_DEVICE_PROTOCOL EDU_DEVICE_PROTOCOL;

typedef enum {
  EduDmaBounce = 0,   // copy through a buffer the driver keeps below the device's 28-bit DMA limit (default)
  EduDmaDirect = 1    // map the caller's buffer as-is (the first version; kept for the regression test)
} EDU_DMA_MODE;

typedef
EFI_STATUS
(EFIAPI *EDU_GET_INFO)(
  IN  EDU_DEVICE_PROTOCOL  *This,
  OUT UINT32               *IdRegister,
  OUT UINTN                *Segment,
  OUT UINTN                *Bus,
  OUT UINTN                *Device,
  OUT UINTN                *Function
  );

typedef
EFI_STATUS
(EFIAPI *EDU_LIVENESS)(
  IN  EDU_DEVICE_PROTOCOL  *This,
  IN  UINT32               Value,
  OUT UINT32               *Inverted
  );

typedef
EFI_STATUS
(EFIAPI *EDU_FACTORIAL)(
  IN  EDU_DEVICE_PROTOCOL  *This,
  IN  UINT32               N,
  OUT UINT32               *Result
  );

/**
  Move Length bytes between Buffer (host memory) and the device's 4 KiB DMA buffer at DeviceOffset.
  ToDevice TRUE: host -> device. BusAddress receives the address the device was programmed with.
**/
typedef
EFI_STATUS
(EFIAPI *EDU_DMA)(
  IN  EDU_DEVICE_PROTOCOL  *This,
  IN  BOOLEAN              ToDevice,
  IN  UINTN                DeviceOffset,
  IN  VOID                 *Buffer,
  IN  UINTN                Length,
  IN  EDU_DMA_MODE         Mode,
  OUT UINT64               *BusAddress
  );

struct _EDU_DEVICE_PROTOCOL {
  UINT32           Revision;
  EDU_GET_INFO     GetInfo;
  EDU_LIVENESS     Liveness;
  EDU_FACTORIAL    Factorial;
  EDU_DMA          Dma;
};

extern EFI_GUID  gEduDeviceProtocolGuid;

#endif
