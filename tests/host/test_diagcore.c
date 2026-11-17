/*
 * Host unit tests for DiagCoreLib: the same C file that runs in firmware, compiled with gcc and run against a
 * simulated KCS BMC (with fault injection) and hand-built / mutated SMBIOS tables. Build: make -C tests/host
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/DiagCoreLib.h>

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------------------------------------------------ simulated KCS BMC (mirrors QEMU ipmi_kcs.c) */
enum { F_NONE, F_NO_BMC, F_STUCK_IBF, F_ERROR_AT_BYTE, F_WRONG_CMD, F_SLOW };
typedef struct {
  UINT8 state, ibf, obf, data_out;
  int cmd, din;
  UINT8 in[64]; unsigned inlen; int write_end;
  UINT8 out[64]; unsigned outlen, outpos;
  int pending, latency, fault, fault_arg, bytes_written;
  unsigned long delays_us;
} SIM;

static void sim_respond(SIM *s) {             /* the "BMC" executes the request */
  UINT8 netfn = s->in[0] >> 2, cmd = s->in[1];
  s->out[0] = (UINT8)((netfn | 1) << 2); s->out[1] = (s->fault == F_WRONG_CMD) ? (UINT8)(cmd + 1) : cmd; s->outlen = 3;
  if (netfn == IPMI_NETFN_APP && cmd == IPMI_CMD_GET_DEVICE_ID) {
    UINT8 id[11] = {0x20, 0x01, 0x02, 0x34, 0x02, 0xBF, 0x57, 0x01, 0x00, 0x34, 0x12};
    s->out[2] = 0; memcpy(s->out + 3, id, 11); s->outlen = 14;
  } else if (netfn == IPMI_NETFN_APP && cmd == IPMI_CMD_GET_SELF_TEST) {
    s->out[2] = 0; s->out[3] = 0x55; s->out[4] = 0x00; s->outlen = 5;
  } else {
    s->out[2] = 0xC1; /* invalid command */
  }
}
static void sim_handle(SIM *s) {
  if (s->cmd == KCS_CMD_GET_STATUS) {
    if (s->state != KCS_STATE_ERROR) { s->out[0] = 0x01; s->outlen = 1; s->outpos = 0; s->state = KCS_STATE_ERROR; s->obf = 1; }
    goto out;
  }
  switch (s->state) {
  case KCS_STATE_IDLE:
    if (s->cmd == KCS_CMD_WRITE_START) { s->state = KCS_STATE_WRITE; s->cmd = -1; s->write_end = 0; s->inlen = 0; s->obf = 1; }
    break;
  case KCS_STATE_READ:
  handle_read:
    if (s->outpos >= s->outlen) { s->state = KCS_STATE_IDLE; s->obf = 1; }
    else if (s->din == KCS_CMD_READ) { s->data_out = s->out[s->outpos++]; s->obf = 1; }
    else { s->out[0] = 0x02; s->outlen = 1; s->outpos = 0; s->state = KCS_STATE_ERROR; s->obf = 1; goto out; }
    break;
  case KCS_STATE_WRITE:
    if (s->din != -1) {
      if (s->fault == F_ERROR_AT_BYTE && ++s->bytes_written == s->fault_arg) {
        s->out[0] = 0x06; s->outlen = 1; s->outpos = 0; s->state = KCS_STATE_ERROR; s->obf = 1; s->fault = F_NONE; goto out;
      }
      if (s->inlen < sizeof s->in) s->in[s->inlen] = (UINT8)s->din;
      s->inlen++;
    }
    if (s->write_end) { s->write_end = 0; s->outpos = 0; sim_respond(s); s->pending = s->latency + 1; return; /* IBF stays set */ }
    else if (s->cmd == KCS_CMD_WRITE_END) { s->cmd = -1; s->write_end = 1; }
    s->obf = 1;
    break;
  case KCS_STATE_ERROR:
    if (s->din != -1) { s->state = KCS_STATE_READ; s->din = KCS_CMD_READ; goto handle_read; }
    break;
  }
  if (s->cmd != -1) { s->out[0] = 0x02; s->outlen = 1; s->outpos = 0; s->state = KCS_STATE_ERROR; }
out:
  s->cmd = -1; s->din = -1; s->ibf = 0;
}
static UINT8 EFIAPI sim_status(VOID *c) {
  SIM *s = c;
  if (s->fault == F_NO_BMC) return 0xFF;
  if (s->pending && --s->pending == 0) {          /* response arrives */
    s->state = KCS_STATE_READ; s->data_out = s->out[0]; s->outpos = 1; s->obf = 1; s->ibf = 0;
  }
  if (s->fault == F_STUCK_IBF && s->bytes_written++ > 3) s->ibf = 1;
  return (UINT8)((s->state << 6) | (s->ibf << 1) | s->obf);
}
static void EFIAPI sim_cmd(VOID *c, UINT8 v) { SIM *s = c; s->ibf = 1; s->cmd = v; s->din = -1; if (s->fault != F_STUCK_IBF) sim_handle(s); }
static void EFIAPI sim_wdata(VOID *c, UINT8 v) { SIM *s = c; s->ibf = 1; s->din = v; s->cmd = -1; if (s->fault != F_STUCK_IBF) sim_handle(s); }
static UINT8 EFIAPI sim_rdata(VOID *c) { SIM *s = c; s->obf = 0; return s->data_out; }
static void EFIAPI sim_delay(VOID *c, UINTN us) { ((SIM *)c)->delays_us += us; }

static KCS_PORT make_port(SIM *s) {
  memset(s, 0, sizeof *s); s->cmd = s->din = -1; s->latency = 3;
  KCS_PORT p = { s, sim_status, sim_cmd, sim_rdata, sim_wdata, sim_delay, 100000, 10, 0 };
  return p;
}

static void test_kcs(void) {
  SIM s; KCS_PORT p; UINT8 cc, data[32]; UINTN n; IPMI_DEVICE_ID id; KCS_RESULT r; UINT8 err;

  p = make_port(&s);
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsOk); CHECK(cc == 0); CHECK(n == 11);
  CHECK(IpmiParseDeviceId(data, n, &id));
  CHECK(id.DeviceId == 0x20); CHECK(id.FirmwareMajor == 2); CHECK(id.FirmwareMinor == 34);
  CHECK(id.IpmiMajor == 2 && id.IpmiMinor == 0); CHECK(id.ManufacturerId == 0x157); CHECK(id.ProductId == 0x1234);
  CHECK(s.state == KCS_STATE_IDLE && !s.obf && !s.ibf);           /* interface left clean */

  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_SELF_TEST, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsOk && cc == 0 && n == 2 && data[0] == 0x55);
  r = IpmiCommand(&p, 0x2C, 0x99, data, 3, &cc, data, sizeof data, &n);
  CHECK(r == KcsOk && cc == 0xC1);                                 /* unsupported command: completion code, not a transport error */

  p = make_port(&s); s.fault = F_NO_BMC;
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsNoBmc); CHECK(s.delays_us == 0);                   /* fails fast, no polling */

  p = make_port(&s); s.fault = F_STUCK_IBF;
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsTimeoutIbf); CHECK(s.delays_us >= 100000 && s.delays_us <= 101000);  /* bounded by the timeout */

  p = make_port(&s); s.fault = F_ERROR_AT_BYTE; s.fault_arg = 1;  /* BMC errors on the first data byte */
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsBadState);
  r = KcsRecover(&p, &err);
  CHECK(r == KcsOk); CHECK(err == 0x06); CHECK(s.state == KCS_STATE_IDLE);
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsOk && cc == 0);                                    /* recovered without a reset */

  p = make_port(&s);                                               /* abort from idle also recovers */
  r = KcsRecover(&p, &err);
  CHECK(r == KcsOk && err == 0x01 && s.state == KCS_STATE_IDLE);

  p = make_port(&s); s.fault = F_WRONG_CMD;
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsBadResponse);

  p = make_port(&s); s.latency = 500;                              /* slow BMC: 5 ms, still inside the timeout */
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, sizeof data, &n);
  CHECK(r == KcsOk && cc == 0);

  p = make_port(&s);
  r = IpmiCommand(&p, IPMI_NETFN_APP, IPMI_CMD_GET_DEVICE_ID, NULL, 0, &cc, data, 4, &n);
  CHECK(r == KcsTooLong);                                          /* never writes past the caller's buffer */
  UINT8 big[80] = {0};
  r = IpmiCommand(&p, IPMI_NETFN_APP, 1, big, sizeof big, &cc, data, sizeof data, &n);
  CHECK(r == KcsTooLong);
}

static void test_ipmi_encoding(void) {
  UINT8 b[16], w[6]; IPMI_SEL_EVENT e, d; IPMI_WATCHDOG wd;
  memset(&e, 0, sizeof e); memset(&d, 0, sizeof d);   /* struct padding must match for the memcmp below */
  IpmiFirmwareEvent(&e, FALSE, IPMI_FW_PROG_OS_BOOT);
  e.RecordId = 0x1234; e.Timestamp = 0xA1B2C3D4;
  IpmiEncodeSelEvent(&e, b);
  CHECK(b[0] == 0x34 && b[1] == 0x12 && b[2] == 0x02 && b[3] == 0xD4 && b[6] == 0xA1);
  CHECK(b[7] == 0x01 && b[9] == 0x04 && b[10] == 0x0F && b[12] == 0x6F && (b[13] & 0x0F) == 0x02 && b[14] == 0x13);
  IpmiDecodeSelEvent(b, &d);
  CHECK(memcmp(&e, &d, sizeof e) == 0);
  CHECK(strstr(IpmiSelDescribe(&d), "starting OS boot") != NULL);
  IpmiFirmwareEvent(&e, TRUE, IPMI_FW_ERR_PCI_DEVICE);
  CHECK(strstr(IpmiSelDescribe(&e), "PCI device missing") != NULL);
  e.SensorType = IPMI_SENSOR_TYPE_WATCHDOG2; e.EventData[0] = 0x01;
  CHECK(strstr(IpmiSelDescribe(&e), "hard reset") != NULL);

  IpmiBuildSetWatchdog(w, IPMI_WDT_USE_BIOS_POST, TRUE, IPMI_WDT_ACTION_HARD_RESET, 0x04, 600);
  CHECK(w[0] == 0x42 && w[1] == 0x01 && w[2] == 0 && w[3] == 0x04 && w[4] == 0x58 && w[5] == 0x02);
  UINT8 g[8] = {0x42, 0x01, 0x00, 0x04, 0x58, 0x02, 0x10, 0x00};
  CHECK(IpmiParseGetWatchdog(g, 8, &wd) && wd.ExpirationFlags == 0x04 && wd.InitialCountdown == 600 && wd.PresentCountdown == 16);
  CHECK(!IpmiParseGetWatchdog(g, 7, &wd));
  IPMI_DEVICE_ID id; CHECK(!IpmiParseDeviceId(g, 8, &id));
}

static void test_edu(void) {
  UINT8 ma, mi;
  CHECK(DiagFactorial32(0) == 1); CHECK(DiagFactorial32(1) == 1); CHECK(DiagFactorial32(5) == 120);
  CHECK(DiagFactorial32(12) == 479001600u); CHECK(DiagFactorial32(13) == 1932053504u);  /* 13! mod 2^32 */
  CHECK(DiagEduIdValid(0x010000ED, &ma, &mi) && ma == 1 && mi == 0);
  CHECK(!DiagEduIdValid(0xFFFFFFFF, &ma, &mi));
  CHECK(DiagDmaAddressOk(0x0FFFF000, 0x1000, EDU_DMA_MASK));
  CHECK(!DiagDmaAddressOk(0x0FFFF001, 0x1000, EDU_DMA_MASK));     /* last byte crosses 256 MiB */
  CHECK(!DiagDmaAddressOk(0x3E000000, 16, EDU_DMA_MASK));
  CHECK(!DiagDmaAddressOk(0xFFFFFFFFFFFFFFF0ull, 0x100, ~0ull)); /* wraps */
  int same = 0; for (int i = 0; i < 4096; i++) same += DiagPatternByte(7, i) == DiagPatternByte(8, i);
  CHECK(same < 64);                                                /* different seeds give different data */
}

/* ------------------------------------------------------------------ SMBIOS */
static size_t add(UINT8 *t, size_t at, UINT8 type, UINT8 len, const char *const *strs, int n) {
  t[at] = type; t[at + 1] = len; t[at + 2] = (UINT8)at; t[at + 3] = 0;
  for (int i = 4; i < len; i++) t[at + i] = (i == 4 || i == 5) ? 1 + (i - 4) : 0;
  size_t p = at + len;
  for (int i = 0; i < n; i++) { strcpy((char *)t + p, strs[i]); p += strlen(strs[i]) + 1; }
  if (n == 0) t[p++] = 0;
  t[p++] = 0;
  return p;
}
static void test_smbios(void) {
  UINT8 t[512] = {0}; size_t n = 0; UINTN len; const UINT8 *s;
  const char *bios[] = {"EFI Development Kit II / OVMF", "0.0.0"};
  const char *sys[] = {"QEMU", "Standard PC (Q35 + ICH9, 2009)"};
  n = add(t, n, 0, 0x1A, bios, 2);
  n = add(t, n, 1, 0x1B, sys, 2);
  n = add(t, n, 17, 0x28, NULL, 0);
  n = add(t, n, 17, 0x28, NULL, 0);
  n = add(t, n, 127, 4, NULL, 0);
  s = SmbiosFind(t, n, 0, 0, &len);
  CHECK(s && strcmp(SmbiosString(s, len, 1), "EFI Development Kit II / OVMF") == 0);
  CHECK(s && strcmp(SmbiosString(s, len, 2), "0.0.0") == 0);
  CHECK(s && SmbiosString(s, len, 3) == NULL && SmbiosString(s, len, 0) == NULL);
  s = SmbiosFind(t, n, 1, 0, &len);
  CHECK(s && strcmp(SmbiosString(s, len, 2), "Standard PC (Q35 + ICH9, 2009)") == 0);
  CHECK(SmbiosCount(t, n, 17) == 2); CHECK(SmbiosFind(t, n, 4, 0, &len) == NULL);
  s = SmbiosFind(t, n, 17, 0, &len); CHECK(s && SmbiosString(s, len, 1) == NULL);  /* no strings */

  /* truncated tables must never be read past the end (run under AddressSanitizer) */
  for (size_t cut = 0; cut < n; cut++) {
    UINT8 *c = malloc(cut ? cut : 1); memcpy(c, t, cut);
    (void)SmbiosCount(c, cut, 17); (void)SmbiosFind(c, cut, 1, 0, &len);
    free(c);
  }
  /* random mutations: no crash, no out-of-bounds read */
  srand(1);
  int found = 0;
  for (int it = 0; it < 20000; it++) {
    UINT8 *c = malloc(n); memcpy(c, t, n);
    for (int k = 0; k < 3; k++) c[rand() % n] = (UINT8)rand();
    s = SmbiosFind(c, n, 1, 0, &len);
    if (s) { found++; for (UINT8 i = 1; i < 4; i++) { const CHAR8 *x = SmbiosString(s, len, i); if (x && !(x >= (const CHAR8 *)c && x < (const CHAR8 *)c + n)) { g_failed++; printf("  FAIL fuzz pointer out of table\n"); } } }
    free(c);
  }
  CHECK(found > 0);

  UINT8 eps[0x18] = {'_', 'S', 'M', '3', '_', 0, 0x18, 3, 6, 0, 1, 0, 0x34, 0x12, 0, 0};
  UINT8 sum = 0; for (int i = 0; i < 0x18; i++) sum += eps[i]; eps[5] = (UINT8)(0 - sum);
  CHECK(SmbiosEntryPoint3Valid(eps, sizeof eps));
  eps[8] ^= 1; CHECK(!SmbiosEntryPoint3Valid(eps, sizeof eps));
  CHECK(!SmbiosEntryPoint3Valid(eps, 10));

  UINT8 e2[0x1F] = {'_', 'S', 'M', '_', 0, 0x1F, 2, 8, 0x40, 0, 0, 0, 0, 0, 0, 0, '_', 'D', 'M', 'I', '_', 0, 0x20, 0x01, 0, 0, 0x0E, 0, 3, 0, 0x28};
  sum = 0; for (int i = 0x10; i < 0x1F; i++) sum += e2[i]; e2[0x15] = (UINT8)(0 - sum);
  sum = 0; for (int i = 0; i < 0x1F; i++) sum += e2[i]; e2[4] = (UINT8)(0 - sum);
  CHECK(SmbiosEntryPoint2Valid(e2, sizeof e2));
  e2[0x18] ^= 0x10; CHECK(!SmbiosEntryPoint2Valid(e2, sizeof e2));        /* table address corrupted */
  e2[0x18] ^= 0x10; e2[0x12] = 'X'; CHECK(!SmbiosEntryPoint2Valid(e2, sizeof e2));
}

int main(void) {
  test_edu(); test_ipmi_encoding(); test_kcs(); test_smbios();
  printf("%d checks + 20000 mutated SMBIOS tables + every truncation, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
