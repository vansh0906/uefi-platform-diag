/** @file
  Hardware-independent logic: edu helpers, KCS transfer state machine, IPMI encode/decode, SMBIOS parsing.

  SPDX-License-Identifier: MIT
**/

#include <Library/DiagCoreLib.h>

// ------------------------------------------------------------------------------------------------ edu device

UINT32
EFIAPI
DiagFactorial32 (
  IN UINT32  N
  )
{
  UINT32  Result;

  Result = 1;
  while (N > 1) {
    Result *= N;                  // wraps modulo 2^32, exactly like the device's 32-bit register
    N--;
  }

  return Result;
}

BOOLEAN
EFIAPI
DiagEduIdValid (
  IN  UINT32  IdRegister,
  OUT UINT8   *Major,
  OUT UINT8   *Minor
  )
{
  *Major = (UINT8)(IdRegister >> 24);
  *Minor = (UINT8)(IdRegister >> 16);
  return (BOOLEAN)((IdRegister & 0xFFFF) == 0x00ED);
}

BOOLEAN
EFIAPI
DiagDmaAddressOk (
  IN UINT64  Address,
  IN UINTN   Length,
  IN UINT64  DmaMask
  )
{
  UINT64  Last;

  if (Length == 0) {
    return TRUE;
  }

  Last = Address + Length - 1;
  if (Last < Address) {
    return FALSE;                 // wraps
  }

  return (BOOLEAN)(((Address & ~DmaMask) == 0) && ((Last & ~DmaMask) == 0));
}

UINT8
EFIAPI
DiagPatternByte (
  IN UINT32  Seed,
  IN UINTN   Index
  )
{
  UINT32  X;

  X  = Seed ^ (UINT32)(Index * 0x9E3779B1u);
  X ^= X >> 15;
  X *= 0x2C1B3C6Du;
  X ^= X >> 12;
  return (UINT8)X;
}

// ------------------------------------------------------------------------------------------------ KCS

CONST CHAR8 *
EFIAPI
KcsResultName (
  IN KCS_RESULT  Result
  )
{
  switch (Result) {
    case KcsOk:          return "ok";
    case KcsNoBmc:       return "no BMC on the KCS port";
    case KcsTimeoutIbf:  return "timeout waiting for IBF to clear";
    case KcsTimeoutObf:  return "timeout waiting for OBF";
    case KcsBadState:    return "KCS interface in an unexpected state";
    case KcsTooLong:     return "message too long";
    case KcsBadResponse: return "response does not match request";
    default:             return "unknown";
  }
}

STATIC
KCS_RESULT
KcsWaitIbfClear (
  IN OUT KCS_PORT  *Port,
  OUT    UINT8     *Status
  )
{
  UINTN  Waited;
  UINT8  S;

  for (Waited = 0; ; Waited += Port->PollUs) {
    S = Port->ReadStatus (Port->Context);
    Port->Polls++;
    if (S == 0xFF) {
      return KcsNoBmc;
    }

    if ((S & KCS_STATUS_IBF) == 0) {
      *Status = S;
      return KcsOk;
    }

    if (Waited >= Port->TimeoutUs) {
      return KcsTimeoutIbf;
    }

    Port->DelayUs (Port->Context, Port->PollUs);
  }
}

STATIC
KCS_RESULT
KcsWaitObfSet (
  IN OUT KCS_PORT  *Port
  )
{
  UINTN  Waited;
  UINT8  S;

  for (Waited = 0; ; Waited += Port->PollUs) {
    S = Port->ReadStatus (Port->Context);
    Port->Polls++;
    if (S == 0xFF) {
      return KcsNoBmc;
    }

    if ((S & KCS_STATUS_OBF) != 0) {
      return KcsOk;
    }

    if (Waited >= Port->TimeoutUs) {
      return KcsTimeoutObf;
    }

    Port->DelayUs (Port->Context, Port->PollUs);
  }
}

STATIC
VOID
KcsClearObf (
  IN OUT KCS_PORT  *Port
  )
{
  if ((Port->ReadStatus (Port->Context) & KCS_STATUS_OBF) != 0) {
    (VOID)Port->ReadData (Port->Context);
  }
}

// Wait for IBF to clear, require the given state, then clear OBF (write-phase handshake).
STATIC
KCS_RESULT
KcsWriteStep (
  IN OUT KCS_PORT  *Port,
  IN     UINT8     ExpectedState
  )
{
  KCS_RESULT  R;
  UINT8       S;

  R = KcsWaitIbfClear (Port, &S);
  if (R != KcsOk) {
    return R;
  }

  if (KCS_STATE (S) != ExpectedState) {
    return KcsBadState;
  }

  KcsClearObf (Port);
  return KcsOk;
}

KCS_RESULT
EFIAPI
KcsTransfer (
  IN OUT KCS_PORT    *Port,
  IN     CONST UINT8 *Request,
  IN     UINTN       RequestLength,
  OUT    UINT8       *Response,
  IN     UINTN       ResponseMax,
  OUT    UINTN       *ResponseLength
  )
{
  KCS_RESULT  R;
  UINT8       S;
  UINTN       Index;
  UINTN       Count;

  *ResponseLength = 0;
  Port->Polls     = 0;
  if ((RequestLength < 2) || (RequestLength > KCS_MAX_MESSAGE)) {
    return KcsTooLong;
  }

  if (Port->ReadStatus (Port->Context) == 0xFF) {
    return KcsNoBmc;              // fail fast: an undecoded ISA port floats high
  }

  //
  // Write phase
  //
  R = KcsWaitIbfClear (Port, &S);
  if (R != KcsOk) {
    return R;
  }

  KcsClearObf (Port);
  Port->WriteCommand (Port->Context, KCS_CMD_WRITE_START);
  R = KcsWriteStep (Port, KCS_STATE_WRITE);
  if (R != KcsOk) {
    return R;
  }

  for (Index = 0; Index + 1 < RequestLength; Index++) {
    Port->WriteData (Port->Context, Request[Index]);
    R = KcsWriteStep (Port, KCS_STATE_WRITE);
    if (R != KcsOk) {
      return R;
    }
  }

  Port->WriteCommand (Port->Context, KCS_CMD_WRITE_END);
  R = KcsWriteStep (Port, KCS_STATE_WRITE);
  if (R != KcsOk) {
    return R;
  }

  Port->WriteData (Port->Context, Request[RequestLength - 1]);

  //
  // Read phase: the BMC holds IBF until its response is ready
  //
  Count = 0;
  for ( ; ;) {
    R = KcsWaitIbfClear (Port, &S);
    if (R != KcsOk) {
      return R;
    }

    if (KCS_STATE (S) == KCS_STATE_READ) {
      R = KcsWaitObfSet (Port);
      if (R != KcsOk) {
        return R;
      }

      if (Count >= ResponseMax) {
        (VOID)Port->ReadData (Port->Context);
        return KcsTooLong;
      }

      Response[Count++] = Port->ReadData (Port->Context);
      Port->WriteData (Port->Context, KCS_CMD_READ);
    } else if (KCS_STATE (S) == KCS_STATE_IDLE) {
      R = KcsWaitObfSet (Port);
      if (R != KcsOk) {
        return R;
      }

      (VOID)Port->ReadData (Port->Context);   // dummy byte that ends the transfer
      break;
    } else {
      return KcsBadState;
    }
  }

  *ResponseLength = Count;
  return KcsOk;
}

KCS_RESULT
EFIAPI
KcsRecover (
  IN OUT KCS_PORT  *Port,
  OUT    UINT8     *ErrorCode
  )
{
  KCS_RESULT  R;
  UINT8       S;

  *ErrorCode = 0xFF;
  R          = KcsWaitIbfClear (Port, &S);
  if (R != KcsOk) {
    return R;
  }

  Port->WriteCommand (Port->Context, KCS_CMD_GET_STATUS);
  R = KcsWaitIbfClear (Port, &S);
  if (R != KcsOk) {
    return R;
  }

  KcsClearObf (Port);
  Port->WriteData (Port->Context, 0x00);
  R = KcsWaitIbfClear (Port, &S);
  if (R != KcsOk) {
    return R;
  }

  if (KCS_STATE (S) != KCS_STATE_READ) {
    return KcsBadState;
  }

  R = KcsWaitObfSet (Port);
  if (R != KcsOk) {
    return R;
  }

  *ErrorCode = Port->ReadData (Port->Context);
  Port->WriteData (Port->Context, KCS_CMD_READ);
  R = KcsWaitIbfClear (Port, &S);
  if (R != KcsOk) {
    return R;
  }

  if (KCS_STATE (S) != KCS_STATE_IDLE) {
    return KcsBadState;
  }

  R = KcsWaitObfSet (Port);
  if (R == KcsOk) {
    (VOID)Port->ReadData (Port->Context);
  }

  return R;
}

// ------------------------------------------------------------------------------------------------ IPMI

KCS_RESULT
EFIAPI
IpmiCommand (
  IN OUT KCS_PORT     *Port,
  IN     UINT8        NetFn,
  IN     UINT8        Command,
  IN     CONST UINT8  *Data,
  IN     UINTN        DataLength,
  OUT    UINT8        *CompletionCode,
  OUT    UINT8        *ResponseData,
  IN     UINTN        ResponseMax,
  OUT    UINTN        *ResponseDataLength
  )
{
  UINT8       Req[KCS_MAX_MESSAGE];
  UINT8       Rsp[KCS_MAX_MESSAGE];
  UINTN       RspLen;
  UINTN       Index;
  KCS_RESULT  R;

  *CompletionCode     = 0xFF;
  *ResponseDataLength = 0;
  if (DataLength + 2 > KCS_MAX_MESSAGE) {
    return KcsTooLong;
  }

  Req[0] = (UINT8)(NetFn << 2);     // LUN 0
  Req[1] = Command;
  for (Index = 0; Index < DataLength; Index++) {
    Req[2 + Index] = Data[Index];
  }

  R = KcsTransfer (Port, Req, DataLength + 2, Rsp, sizeof (Rsp), &RspLen);
  if (R != KcsOk) {
    return R;
  }

  if ((RspLen < 3) || ((Rsp[0] >> 2) != (UINT8)(NetFn | 1)) || (Rsp[1] != Command)) {
    return KcsBadResponse;
  }

  *CompletionCode = Rsp[2];
  for (Index = 3; Index < RspLen; Index++) {
    if (Index - 3 >= ResponseMax) {
      return KcsTooLong;
    }

    ResponseData[Index - 3] = Rsp[Index];
  }

  *ResponseDataLength = RspLen - 3;
  return KcsOk;
}

STATIC
UINT8
Bcd (
  IN UINT8  Value
  )
{
  return (UINT8)((Value >> 4) * 10 + (Value & 0x0F));
}

BOOLEAN
EFIAPI
IpmiParseDeviceId (
  IN  CONST UINT8     *Data,
  IN  UINTN           Length,
  OUT IPMI_DEVICE_ID  *Out
  )
{
  if (Length < 11) {
    return FALSE;
  }

  Out->DeviceId       = Data[0];
  Out->DeviceRevision = (UINT8)(Data[1] & 0x0F);
  Out->FirmwareMajor  = (UINT8)(Data[2] & 0x7F);
  Out->FirmwareMinor  = Bcd (Data[3]);
  Out->IpmiMajor      = (UINT8)(Data[4] & 0x0F);
  Out->IpmiMinor      = (UINT8)(Data[4] >> 4);
  Out->ManufacturerId = (UINT32)Data[6] | ((UINT32)Data[7] << 8) | (((UINT32)Data[8] & 0x0F) << 16);
  Out->ProductId      = (UINT16)(Data[9] | (Data[10] << 8));
  return TRUE;
}

VOID
EFIAPI
IpmiBuildSetWatchdog (
  OUT UINT8    Out[6],
  IN  UINT8    TimerUse,
  IN  BOOLEAN  DontStop,
  IN  UINT8    Action,
  IN  UINT8    ClearFlags,
  IN  UINT16   Countdown100ms
  )
{
  Out[0] = (UINT8)((TimerUse & 0x07) | (DontStop ? IPMI_WDT_DONT_STOP : 0));
  Out[1] = (UINT8)(Action & 0x07);        // no pre-timeout interrupt
  Out[2] = 0;
  Out[3] = ClearFlags;
  Out[4] = (UINT8)(Countdown100ms & 0xFF);
  Out[5] = (UINT8)(Countdown100ms >> 8);
}

BOOLEAN
EFIAPI
IpmiParseGetWatchdog (
  IN  CONST UINT8    *Data,
  IN  UINTN          Length,
  OUT IPMI_WATCHDOG  *Out
  )
{
  if (Length < 8) {
    return FALSE;
  }

  Out->TimerUse         = Data[0];
  Out->TimerActions     = Data[1];
  Out->PreTimeout       = Data[2];
  Out->ExpirationFlags  = Data[3];
  Out->InitialCountdown = (UINT16)(Data[4] | (Data[5] << 8));
  Out->PresentCountdown = (UINT16)(Data[6] | (Data[7] << 8));
  return TRUE;
}

VOID
EFIAPI
IpmiEncodeSelEvent (
  IN  CONST IPMI_SEL_EVENT  *Event,
  OUT UINT8                 Out[16]
  )
{
  Out[0]  = (UINT8)(Event->RecordId & 0xFF);
  Out[1]  = (UINT8)(Event->RecordId >> 8);
  Out[2]  = Event->RecordType;
  Out[3]  = (UINT8)(Event->Timestamp);
  Out[4]  = (UINT8)(Event->Timestamp >> 8);
  Out[5]  = (UINT8)(Event->Timestamp >> 16);
  Out[6]  = (UINT8)(Event->Timestamp >> 24);
  Out[7]  = (UINT8)(Event->GeneratorId & 0xFF);
  Out[8]  = (UINT8)(Event->GeneratorId >> 8);
  Out[9]  = Event->EvmRev;
  Out[10] = Event->SensorType;
  Out[11] = Event->SensorNumber;
  Out[12] = Event->EventDirType;
  Out[13] = Event->EventData[0];
  Out[14] = Event->EventData[1];
  Out[15] = Event->EventData[2];
}

VOID
EFIAPI
IpmiDecodeSelEvent (
  IN  CONST UINT8     In[16],
  OUT IPMI_SEL_EVENT  *Event
  )
{
  Event->RecordId     = (UINT16)(In[0] | (In[1] << 8));
  Event->RecordType   = In[2];
  Event->Timestamp    = (UINT32)In[3] | ((UINT32)In[4] << 8) | ((UINT32)In[5] << 16) | ((UINT32)In[6] << 24);
  Event->GeneratorId  = (UINT16)(In[7] | (In[8] << 8));
  Event->EvmRev       = In[9];
  Event->SensorType   = In[10];
  Event->SensorNumber = In[11];
  Event->EventDirType = In[12];
  Event->EventData[0] = In[13];
  Event->EventData[1] = In[14];
  Event->EventData[2] = In[15];
}

VOID
EFIAPI
IpmiFirmwareEvent (
  OUT IPMI_SEL_EVENT  *Event,
  IN  BOOLEAN         IsError,
  IN  UINT8           Code
  )
{
  Event->RecordId     = 0;          // assigned by the BMC
  Event->RecordType   = 0x02;
  Event->Timestamp    = 0;          // assigned by the BMC
  Event->GeneratorId  = 0x0001;     // system software, software ID 0 (BIOS)
  Event->EvmRev       = 0x04;
  Event->SensorType   = IPMI_SENSOR_TYPE_FW_PROGRESS;
  Event->SensorNumber = 0x01;
  Event->EventDirType = 0x6F;       // assertion, sensor-specific
  Event->EventData[0] = (UINT8)(0xA0 | (IsError ? IPMI_FW_OFFSET_ERROR : IPMI_FW_OFFSET_PROGRESS)); // data 2 is OEM/code
  Event->EventData[1] = Code;
  Event->EventData[2] = 0xFF;
}

CONST CHAR8 *
EFIAPI
IpmiSelDescribe (
  IN CONST IPMI_SEL_EVENT  *Event
  )
{
  UINT8  Offset;

  if (Event->RecordType != 0x02) {
    return "OEM record";
  }

  Offset = (UINT8)(Event->EventData[0] & 0x0F);
  if (Event->SensorType == IPMI_SENSOR_TYPE_FW_PROGRESS) {
    if (Offset == IPMI_FW_OFFSET_ERROR) {
      return (Event->EventData[1] == IPMI_FW_ERR_PCI_DEVICE) ? "Firmware error: expected PCI device missing"
                                                              : "Firmware error";
    }

    if (Offset == IPMI_FW_OFFSET_PROGRESS) {
      switch (Event->EventData[1]) {
        case IPMI_FW_PROG_PCI_INIT:     return "Firmware progress: PCI initialization";
        case IPMI_FW_PROG_OS_BOOT:      return "Firmware progress: diagnostics complete, starting OS boot";
        case IPMI_FW_PROG_WDT_RECOVERY: return "Firmware progress: safe-mode boot after watchdog reset";
        default:                        return "Firmware progress";
      }
    }
  }

  if (Event->SensorType == IPMI_SENSOR_TYPE_WATCHDOG2) {
    switch (Offset) {
      case 0x00: return "Watchdog: timer expired";
      case 0x01: return "Watchdog: hard reset";
      case 0x02: return "Watchdog: power down";
      case 0x03: return "Watchdog: power cycle";
      default:   return "Watchdog event";
    }
  }

  return "System event";
}

// ------------------------------------------------------------------------------------------------ SMBIOS

STATIC
BOOLEAN
ChecksumZero (
  IN CONST UINT8  *Bytes,
  IN UINTN        Length
  )
{
  UINT8  Sum;
  UINTN  Index;

  Sum = 0;
  for (Index = 0; Index < Length; Index++) {
    Sum = (UINT8)(Sum + Bytes[Index]);
  }

  return (BOOLEAN)(Sum == 0);
}

// SMBIOS 2.1 (32-bit) entry point: "_SM_" anchor + checksum, then "_DMI_" intermediate anchor + checksum.
BOOLEAN
EFIAPI
SmbiosEntryPoint2Valid (
  IN CONST UINT8  *Eps,
  IN UINTN        Length
  )
{
  UINTN  EpsLength;

  if ((Length < 0x1F) || (Eps[0] != '_') || (Eps[1] != 'S') || (Eps[2] != 'M') || (Eps[3] != '_')) {
    return FALSE;
  }

  EpsLength = Eps[5];
  if ((EpsLength < 0x1F) || (EpsLength > Length)) {
    return FALSE;
  }

  if ((Eps[0x10] != '_') || (Eps[0x11] != 'D') || (Eps[0x12] != 'M') || (Eps[0x13] != 'I') || (Eps[0x14] != '_')) {
    return FALSE;
  }

  return (BOOLEAN)(ChecksumZero (Eps, EpsLength) && ChecksumZero (Eps + 0x10, 0x0F));
}

BOOLEAN
EFIAPI
SmbiosEntryPoint3Valid (
  IN CONST UINT8  *Eps,
  IN UINTN        Length
  )
{
  UINTN  EpsLength;

  if ((Length < 0x18) || (Eps[0] != '_') || (Eps[1] != 'S') || (Eps[2] != 'M') || (Eps[3] != '3') || (Eps[4] != '_')) {
    return FALSE;
  }

  EpsLength = Eps[6];
  if ((EpsLength < 0x18) || (EpsLength > Length)) {
    return FALSE;
  }

  return ChecksumZero (Eps, EpsLength);
}

// Length of one structure including its string set, or 0 if it runs past the end of the table.
STATIC
UINTN
SmbiosStructureLength (
  IN CONST UINT8  *Table,
  IN UINTN        TableLength,
  IN UINTN        Offset
  )
{
  UINTN  FormattedLength;
  UINTN  Cursor;

  if (Offset + 4 > TableLength) {
    return 0;
  }

  FormattedLength = Table[Offset + 1];
  if ((FormattedLength < 4) || (Offset + FormattedLength + 2 > TableLength)) {
    return 0;
  }

  // the string set ends with two NUL bytes (a structure without strings has exactly two)
  for (Cursor = Offset + FormattedLength; Cursor + 1 < TableLength; Cursor++) {
    if ((Table[Cursor] == 0) && (Table[Cursor + 1] == 0)) {
      return Cursor + 2 - Offset;
    }
  }

  return 0;
}

CONST UINT8 *
EFIAPI
SmbiosFind (
  IN  CONST UINT8  *Table,
  IN  UINTN        TableLength,
  IN  UINT8        Type,
  IN  UINTN        Instance,
  OUT UINTN        *TotalLength
  )
{
  UINTN  Offset;
  UINTN  Len;
  UINTN  Seen;

  Seen = 0;
  for (Offset = 0; Offset < TableLength; Offset += Len) {
    Len = SmbiosStructureLength (Table, TableLength, Offset);
    if (Len == 0) {
      return NULL;                // malformed or truncated: stop, never read past the buffer
    }

    if (Table[Offset] == Type) {
      if (Seen == Instance) {
        *TotalLength = Len;
        return Table + Offset;
      }

      Seen++;
    }

    if (Table[Offset] == 127) {   // end-of-table
      break;
    }
  }

  return NULL;
}

UINTN
EFIAPI
SmbiosCount (
  IN CONST UINT8  *Table,
  IN UINTN        TableLength,
  IN UINT8        Type
  )
{
  UINTN  Count;
  UINTN  Len;

  Count = 0;
  while (SmbiosFind (Table, TableLength, Type, Count, &Len) != NULL) {
    Count++;
  }

  return Count;
}

CONST CHAR8 *
EFIAPI
SmbiosString (
  IN CONST UINT8  *Structure,
  IN UINTN        TotalLength,
  IN UINT8        Index
  )
{
  UINTN  Cursor;
  UINT8  Current;

  if ((Index == 0) || (TotalLength < 4) || (Structure[1] > TotalLength)) {
    return NULL;
  }

  Cursor  = Structure[1];
  Current = 1;
  while (Cursor < TotalLength && Structure[Cursor] != 0) {
    if (Current == Index) {
      return (CONST CHAR8 *)(Structure + Cursor);
    }

    while (Cursor < TotalLength && Structure[Cursor] != 0) {
      Cursor++;
    }

    Cursor++;
    Current++;
  }

  return NULL;
}
