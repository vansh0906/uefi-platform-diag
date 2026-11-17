/** @file
  KCS_PORT bound to the system interface's I/O ports (default 0xCA2 data, 0xCA3 status/command).

  SPDX-License-Identifier: MIT
**/

#ifndef IPMI_KCS_LIB_H_
#define IPMI_KCS_LIB_H_

#include <Library/DiagCoreLib.h>

#define IPMI_KCS_DEFAULT_BASE  0xCA2

VOID
EFIAPI
IpmiKcsPortInit (
  OUT KCS_PORT  *Port,
  IN  UINT16    *BaseStorage,
  IN  UINT16    Base,
  IN  UINTN     TimeoutUs
  );

#endif
