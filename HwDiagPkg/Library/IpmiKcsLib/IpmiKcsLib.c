/** @file
  Port I/O and delay callbacks for the KCS state machine in DiagCoreLib.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IpmiKcsLib.h>

STATIC UINT8 EFIAPI KcsReadStatus (VOID *Ctx)            { return IoRead8 ((UINTN)*(UINT16 *)Ctx + 1); }
STATIC VOID  EFIAPI KcsWriteCommand (VOID *Ctx, UINT8 V)  { IoWrite8 ((UINTN)*(UINT16 *)Ctx + 1, V); }
STATIC UINT8 EFIAPI KcsReadData (VOID *Ctx)              { return IoRead8 ((UINTN)*(UINT16 *)Ctx); }
STATIC VOID  EFIAPI KcsWriteData (VOID *Ctx, UINT8 V)     { IoWrite8 ((UINTN)*(UINT16 *)Ctx, V); }
STATIC VOID  EFIAPI KcsDelay (VOID *Ctx, UINTN Us)        { gBS->Stall (Us); }

VOID
EFIAPI
IpmiKcsPortInit (
  OUT KCS_PORT  *Port,
  IN  UINT16    *BaseStorage,
  IN  UINT16    Base,
  IN  UINTN     TimeoutUs
  )
{
  *BaseStorage       = Base;
  Port->Context      = BaseStorage;
  Port->ReadStatus   = KcsReadStatus;
  Port->WriteCommand = KcsWriteCommand;
  Port->ReadData     = KcsReadData;
  Port->WriteData    = KcsWriteData;
  Port->DelayUs      = KcsDelay;
  Port->TimeoutUs    = TimeoutUs;
  Port->PollUs       = 10;
  Port->Polls        = 0;
}
