/** @file
  Component Name 2 for EduDxe (shown by the shell's "drivers" and "devices" commands).

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/EduDevice.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

extern EFI_DRIVER_BINDING_PROTOCOL  gEduDriverBinding;

STATIC EFI_UNICODE_STRING_TABLE  mDriverName[] = {
  { "en", L"QEMU edu PCI device driver" },
  { NULL, NULL }
};

STATIC EFI_UNICODE_STRING_TABLE  mControllerName[] = {
  { "en", L"QEMU edu device (EDU_DEVICE_PROTOCOL)" },
  { NULL, NULL }
};

STATIC
EFI_STATUS
EFIAPI
EduGetDriverName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **DriverName
  )
{
  return LookupUnicodeString2 (Language, This->SupportedLanguages, mDriverName, DriverName, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
EduGetControllerName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  EFI_HANDLE                    ControllerHandle,
  IN  EFI_HANDLE                    ChildHandle OPTIONAL,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **ControllerName
  )
{
  EFI_STATUS  Status;
  VOID        *Edu;

  if (ChildHandle != NULL) {
    return EFI_UNSUPPORTED;
  }

  // only name controllers this driver manages
  Status = gBS->OpenProtocol (ControllerHandle, &gEduDeviceProtocolGuid, &Edu, gEduDriverBinding.DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  return LookupUnicodeString2 (Language, This->SupportedLanguages, mControllerName, ControllerName, FALSE);
}

GLOBAL_REMOVE_IF_UNREFERENCED EFI_COMPONENT_NAME2_PROTOCOL  gEduComponentName2 = {
  EduGetDriverName,
  EduGetControllerName,
  "en"
};
