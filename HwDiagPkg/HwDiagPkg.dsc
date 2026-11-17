## @file
#  Builds EduDxe.efi and PlatformDiag.efi for X64.
#
#  SPDX-License-Identifier: MIT
##

[Defines]
  PLATFORM_NAME           = HwDiagPkg
  PLATFORM_GUID           = 5C2A8E91-3D74-4B0F-9E6A-1B8D2F4C7A30
  PLATFORM_VERSION        = 1.0
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/HwDiagPkg
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS           = DEBUG|RELEASE
  SKUID_IDENTIFIER        = DEFAULT

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  DiagCoreLib|HwDiagPkg/Library/DiagCoreLib/DiagCoreLib.inf
  IpmiKcsLib|HwDiagPkg/Library/IpmiKcsLib/IpmiKcsLib.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2F
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000002

[Components]
  HwDiagPkg/Drivers/EduDxe/EduDxe.inf
  HwDiagPkg/Application/PlatformDiag/PlatformDiag.inf

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -Werror
