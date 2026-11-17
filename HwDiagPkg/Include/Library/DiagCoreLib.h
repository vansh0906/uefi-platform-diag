/** @file
  Hardware-independent logic shared by the driver and the application: QEMU edu device helpers, the IPMI KCS
  transfer state machine (IPMI v2.0 section 9), IPMI message encode/decode, and SMBIOS table parsing.

  Nothing here touches hardware or UEFI services directly. Port I/O and delays come in through KCS_PORT, so the same
  code runs in firmware and in host unit tests against a simulated BMC.

  SPDX-License-Identifier: MIT
**/

#ifndef DIAG_CORE_LIB_H_
#define DIAG_CORE_LIB_H_

#include <Base.h>

//
// QEMU "edu" educational PCI device (docs/specs/edu.rst in QEMU)
//
#define EDU_VENDOR_ID         0x1234
#define EDU_DEVICE_ID         0x11E8
#define EDU_REG_ID            0x00
#define EDU_REG_LIVENESS      0x04
#define EDU_REG_FACTORIAL     0x08
#define EDU_REG_STATUS        0x20
#define EDU_REG_DMA_SRC       0x80
#define EDU_REG_DMA_DST       0x88
#define EDU_REG_DMA_COUNT     0x90
#define EDU_REG_DMA_CMD       0x98
#define EDU_STATUS_COMPUTING  BIT0
#define EDU_DMA_CMD_START     BIT0
#define EDU_DMA_CMD_TO_RAM    BIT1        // 0: RAM -> device buffer, 1: device buffer -> RAM
#define EDU_DMA_BUFFER_ADDR   0x40000     // device-side buffer, 4 KiB
#define EDU_DMA_BUFFER_SIZE   0x1000
#define EDU_DMA_MASK          0x0FFFFFFFULL // the device only drives 28 address bits

UINT32
EFIAPI
DiagFactorial32 (
  IN UINT32  N
  );

BOOLEAN
EFIAPI
DiagEduIdValid (
  IN  UINT32  IdRegister,
  OUT UINT8   *Major,
  OUT UINT8   *Minor
  );

BOOLEAN
EFIAPI
DiagDmaAddressOk (
  IN UINT64  Address,
  IN UINTN   Length,
  IN UINT64  DmaMask
  );

UINT8
EFIAPI
DiagPatternByte (
  IN UINT32  Seed,
  IN UINTN   Index
  );

//
// IPMI KCS transport
//
#define KCS_STATUS_OBF        BIT0
#define KCS_STATUS_IBF        BIT1
#define KCS_STATE(s)          (((s) >> 6) & 0x3)
#define KCS_STATE_IDLE        0
#define KCS_STATE_READ        1
#define KCS_STATE_WRITE       2
#define KCS_STATE_ERROR       3
#define KCS_CMD_GET_STATUS    0x60
#define KCS_CMD_WRITE_START   0x61
#define KCS_CMD_WRITE_END     0x62
#define KCS_CMD_READ          0x68
#define KCS_MAX_MESSAGE       64

typedef struct {
  VOID     *Context;
  UINT8    (EFIAPI *ReadStatus)(VOID *Context);
  VOID     (EFIAPI *WriteCommand)(VOID *Context, UINT8 Value);
  UINT8    (EFIAPI *ReadData)(VOID *Context);
  VOID     (EFIAPI *WriteData)(VOID *Context, UINT8 Value);
  VOID     (EFIAPI *DelayUs)(VOID *Context, UINTN Microseconds);
  UINTN    TimeoutUs;       // per wait (IBF clear / OBF set)
  UINTN    PollUs;          // poll interval
  UINTN    Polls;           // statistics: status reads during the last transfer
} KCS_PORT;

typedef enum {
  KcsOk = 0,
  KcsNoBmc,                 // status reads 0xFF: nothing decodes the port
  KcsTimeoutIbf,            // BMC never took the byte
  KcsTimeoutObf,            // BMC never produced a byte
  KcsBadState,              // interface not in the state the protocol requires
  KcsTooLong,               // request or response longer than the buffer
  KcsBadResponse            // response NetFn/command does not match the request
} KCS_RESULT;

CONST CHAR8 *
EFIAPI
KcsResultName (
  IN KCS_RESULT  Result
  );

KCS_RESULT
EFIAPI
KcsTransfer (
  IN OUT KCS_PORT    *Port,
  IN     CONST UINT8 *Request,
  IN     UINTN       RequestLength,
  OUT    UINT8       *Response,
  IN     UINTN       ResponseMax,
  OUT    UINTN       *ResponseLength
  );

KCS_RESULT
EFIAPI
KcsRecover (
  IN OUT KCS_PORT  *Port,
  OUT    UINT8     *ErrorCode
  );

//
// IPMI messages
//
#define IPMI_NETFN_APP                0x06
#define IPMI_NETFN_STORAGE            0x0A
#define IPMI_CMD_GET_DEVICE_ID        0x01
#define IPMI_CMD_GET_SELF_TEST        0x04
#define IPMI_CC_INVALID_COMMAND       0xC1
#define IPMI_CMD_RESET_WATCHDOG       0x22
#define IPMI_CMD_SET_WATCHDOG         0x24
#define IPMI_CMD_GET_WATCHDOG         0x25
#define IPMI_CMD_GET_SEL_INFO         0x40
#define IPMI_CMD_GET_SEL_ENTRY        0x43
#define IPMI_CMD_ADD_SEL_ENTRY        0x44

#define IPMI_WDT_USE_BIOS_FRB2        1
#define IPMI_WDT_USE_BIOS_POST        2
#define IPMI_WDT_ACTION_NONE          0
#define IPMI_WDT_ACTION_HARD_RESET    1
#define IPMI_WDT_DONT_STOP            BIT6

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
  );

typedef struct {
  UINT8     DeviceId;
  UINT8     DeviceRevision;
  UINT8     FirmwareMajor;
  UINT8     FirmwareMinor;      // decoded from BCD
  UINT8     IpmiMajor;
  UINT8     IpmiMinor;
  UINT32    ManufacturerId;
  UINT16    ProductId;
} IPMI_DEVICE_ID;

BOOLEAN
EFIAPI
IpmiParseDeviceId (
  IN  CONST UINT8     *Data,
  IN  UINTN           Length,
  OUT IPMI_DEVICE_ID  *Out
  );

typedef struct {
  UINT8     TimerUse;           // includes the don't-stop bit
  UINT8     TimerActions;
  UINT8     PreTimeout;
  UINT8     ExpirationFlags;    // bit n set: a timer with use n expired
  UINT16    InitialCountdown;   // 100 ms units
  UINT16    PresentCountdown;
} IPMI_WATCHDOG;

VOID
EFIAPI
IpmiBuildSetWatchdog (
  OUT UINT8    Out[6],
  IN  UINT8    TimerUse,
  IN  BOOLEAN  DontStop,
  IN  UINT8    Action,
  IN  UINT8    ClearFlags,
  IN  UINT16   Countdown100ms
  );

BOOLEAN
EFIAPI
IpmiParseGetWatchdog (
  IN  CONST UINT8    *Data,
  IN  UINTN          Length,
  OUT IPMI_WATCHDOG  *Out
  );

typedef struct {
  UINT16    RecordId;
  UINT8     RecordType;         // 0x02: system event
  UINT32    Timestamp;
  UINT16    GeneratorId;        // 0x0001: system BIOS (software ID 0x00)
  UINT8     EvmRev;             // 0x04: IPMI 2.0
  UINT8     SensorType;
  UINT8     SensorNumber;
  UINT8     EventDirType;       // bit 7 deassert; bits 6:0 event/reading type
  UINT8     EventData[3];
} IPMI_SEL_EVENT;

#define IPMI_SENSOR_TYPE_FW_PROGRESS  0x0F  // System Firmware Progress (formerly POST error)
#define IPMI_SENSOR_TYPE_WATCHDOG2    0x23
#define IPMI_FW_OFFSET_ERROR          0x00  // event data 2: firmware error code
#define IPMI_FW_OFFSET_PROGRESS       0x02  // event data 2: progress code
#define IPMI_FW_ERR_UNSPECIFIED       0x00
#define IPMI_FW_ERR_NO_VIDEO          0x02
#define IPMI_FW_ERR_PCI_DEVICE        0x0D  // OEM-style use in this project: expected PCI device missing
#define IPMI_FW_PROG_PCI_INIT         0x03
#define IPMI_FW_PROG_OS_BOOT          0x13
#define IPMI_FW_PROG_WDT_RECOVERY     0x80  // OEM range: booted in safe mode after a watchdog reset

VOID
EFIAPI
IpmiEncodeSelEvent (
  IN  CONST IPMI_SEL_EVENT  *Event,
  OUT UINT8                 Out[16]
  );

VOID
EFIAPI
IpmiDecodeSelEvent (
  IN  CONST UINT8     In[16],
  OUT IPMI_SEL_EVENT  *Event
  );

VOID
EFIAPI
IpmiFirmwareEvent (
  OUT IPMI_SEL_EVENT  *Event,
  IN  BOOLEAN         IsError,
  IN  UINT8           Code
  );

CONST CHAR8 *
EFIAPI
IpmiSelDescribe (
  IN CONST IPMI_SEL_EVENT  *Event
  );

//
// SMBIOS
//
BOOLEAN
EFIAPI
SmbiosEntryPoint2Valid (
  IN CONST UINT8  *Eps,
  IN UINTN        Length
  );

BOOLEAN
EFIAPI
SmbiosEntryPoint3Valid (
  IN CONST UINT8  *Eps,
  IN UINTN        Length
  );

CONST UINT8 *
EFIAPI
SmbiosFind (
  IN  CONST UINT8  *Table,
  IN  UINTN        TableLength,
  IN  UINT8        Type,
  IN  UINTN        Instance,
  OUT UINTN        *TotalLength
  );

UINTN
EFIAPI
SmbiosCount (
  IN CONST UINT8  *Table,
  IN UINTN        TableLength,
  IN UINT8        Type
  );

CONST CHAR8 *
EFIAPI
SmbiosString (
  IN CONST UINT8  *Structure,
  IN UINTN        TotalLength,
  IN UINT8        Index
  );

#endif
