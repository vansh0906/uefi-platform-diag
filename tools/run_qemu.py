#!/usr/bin/env python3
"""Boot PlatformDiag on OVMF in QEMU under a set of hardware scenarios and check the serial output.

    python3 tools/run_qemu.py                 # all scenarios, prebuilt/ or build/ EFI files
    python3 tools/run_qemu.py baseline hang   # just these

Finds QEMU and OVMF from $QEMU, $OVMF_CODE, $OVMF_VARS, or the usual Homebrew / Ubuntu locations.
Writes results/scenarios.json, results/RESULTS.md and one serial log per scenario in results/logs/.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")


# ----------------------------------------------------------------------------------------------- environment

def find_qemu():
    for c in (os.environ.get("QEMU"), shutil.which("qemu-system-x86_64")):
        if c and os.path.exists(c):
            return c
    return None


def find_ovmf():
    code, vars_ = os.environ.get("OVMF_CODE"), os.environ.get("OVMF_VARS")
    if code and vars_:
        return code, vars_
    prefixes = []
    if shutil.which("brew"):
        try:
            prefixes.append(subprocess.check_output(["brew", "--prefix"], text=True).strip())
        except Exception:
            pass
    prefixes += ["/opt/homebrew", "/usr/local", "/usr"]
    for p in prefixes:
        c, v = f"{p}/share/qemu/edk2-x86_64-code.fd", f"{p}/share/qemu/edk2-i386-vars.fd"
        if os.path.exists(c) and os.path.exists(v):
            return c, v
    for c, v in (("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
                 ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd")):
        if os.path.exists(c) and os.path.exists(v):
            return c, v
    return None, None


def find_efi():
    for d in (os.path.join(ROOT, "build"), os.path.join(ROOT, "prebuilt")):
        a, b = os.path.join(d, "PlatformDiag.efi"), os.path.join(d, "EduDxe.efi")
        if os.path.exists(a) and os.path.exists(b):
            return a, b
    return None, None


# ----------------------------------------------------------------------------------------------- one boot

def boot(name, edu=1, bmc=True, mem=256, hang=False, smbios64=False, vars_file=None, timeout=240):
    qemu = find_qemu()
    code, vars_tmpl = find_ovmf()
    app, drv = find_efi()
    work = tempfile.mkdtemp(prefix=f"pd-{name}-")
    esp = os.path.join(work, "esp")
    os.makedirs(os.path.join(esp, "EFI", "BOOT"))
    shutil.copy(app, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
    shutil.copy(drv, os.path.join(esp, "EduDxe.efi"))
    if hang:
        open(os.path.join(esp, "pd_hang"), "w").close()
    if vars_file is None:
        vars_file = os.path.join(work, "vars.fd")
        shutil.copy(vars_tmpl, vars_file)
    machine = "q35,smbios-entry-point-type=64" if smbios64 else "q35"
    cmd = [qemu, "-machine", machine, "-m", str(mem), "-display", "none", "-serial", "stdio", "-monitor", "none",
           "-net", "none",
           "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
           "-drive", f"if=pflash,format=raw,file={vars_file}",
           "-drive", f"file=fat:rw:{esp},format=raw,if=ide"]
    if os.environ.get("QEMU_DATA_DIR"):
        cmd += ["-L", os.environ["QEMU_DATA_DIR"]]
    for _ in range(edu):
        cmd += ["-device", "edu"]
    if bmc:
        cmd += ["-device", "ipmi-bmc-sim,id=bmc0", "-device", "isa-ipmi-kcs,bmc=bmc0"]
    if not hang:
        cmd += ["-no-reboot"]

    lines, t0 = [], time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def reader():
        buf = b""
        while True:
            chunk = proc.stdout.read1(4096) if hasattr(proc.stdout, "read1") else proc.stdout.read(1)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = re.sub(r"\x1b\[[0-9;=?]*[A-Za-z]", "", raw.decode("latin-1")).replace("\r", "")
                lines.append((round(time.monotonic() - t0, 2), text))

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    try:
        proc.wait(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        proc.kill()
        timed_out = True
    th.join(5)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(RESULTS, "logs"), exist_ok=True)
    with open(os.path.join(RESULTS, "logs", f"{name}.log"), "w") as f:
        f.writelines(f"[{t:7.2f}] {l}\n" for t, l in lines)
    return parse(lines, timed_out, round(time.monotonic() - t0, 1))


def parse(lines, timed_out, seconds):
    pd = [(t, l[l.index("PD: ") + 4:]) for t, l in lines if "PD: " in l]
    tests, starts = {}, []
    out = {"timed_out": timed_out, "seconds": seconds, "lines": [l for _, l in pd]}
    for t, l in pd:
        m = re.match(r"TEST (\S+) (PASS|FAIL|SKIP) ?(.*)", l)
        if m:
            tests[m.group(1)] = {"result": m.group(2), "detail": m.group(3)}
        if l.startswith("START"):
            starts.append(t)
        if l.startswith("HANG"):
            out["hang_at"] = t
        m = re.match(r"RESULT (PASS|FAIL) (\d+)/(\d+)", l)
        if m:
            out["result"], out["passed"], out["total"] = m.group(1), int(m.group(2)), int(m.group(3))
        m = re.match(r"BOOT count=(\d+)", l)
        if m:
            out["boot_count"] = int(m.group(1))
        m = re.match(r"DMA direct host (0x[0-9A-Fa-f]+) above_28bit=(\d) bytes_wrong=(\d+)", l)
        if m:
            out["dma_direct"] = {"host": m.group(1), "above_28bit": m.group(2) == "1", "bytes_wrong": int(m.group(3))}
        m = re.match(r"SMBIOS memory devices=(\d+) total_mb=(\d+)", l)
        if m:
            out["smbios_mb"] = int(m.group(2))
    out["tests"], out["starts"] = tests, starts
    out["sel"] = [l[4:] for l in out["lines"] if l.startswith("SEL 0x")]
    return out


# ----------------------------------------------------------------------------------------------- scenarios

def expect(cond, what, failures):
    if not cond:
        failures.append(what)


def s_baseline():
    r = boot("baseline")
    f = []
    expect(r.get("result") == "PASS", "all checks pass", f)
    expect(r["tests"].get("bmc.self_test", {}).get("result") == "SKIP", "self test reported as unsupported, not failed", f)
    expect(r.get("smbios_mb") == 256, "SMBIOS memory matches -m 256", f)
    expect(r["tests"].get("driver.no_leak", {}).get("result") == "PASS", "no pages leaked over 25 reconnects", f)
    return r, f


def s_high_memory():
    r = boot("high_memory", mem=1024)
    f = []
    d = r.get("dma_direct", {})
    expect(r.get("result") == "PASS", "all checks pass with 1 GiB", f)
    expect(d.get("above_28bit") and d.get("bytes_wrong", 0) > 0, "direct DMA from above 256 MiB corrupts data", f)
    expect("Unsupported" in r["tests"].get("edu0.dma_guard", {}).get("detail", ""), "driver refuses unsafe direct write", f)
    expect(r.get("smbios_mb") == 1024, "SMBIOS memory matches -m 1024", f)
    return r, f


def s_two_devices():
    r = boot("two_devices", edu=2)
    f = []
    expect(r.get("result") == "PASS", "all checks pass", f)
    expect("2 of 2" in r["tests"].get("driver.bind", {}).get("detail", ""), "driver binds both devices", f)
    expect("edu1.dma_bounce" in r["tests"], "second device tested", f)
    expect("x 2 devices" in r["tests"].get("driver.reconnect", {}).get("detail", ""), "reconnect covers both", f)
    return r, f


def s_no_device():
    r = boot("no_device", edu=0)
    f = []
    expect(r.get("result") == "FAIL", "run reports failure", f)
    expect(r["tests"].get("edu.present", {}).get("result") == "FAIL", "missing device detected", f)
    expect(any("expected PCI device missing" in s for s in r["sel"]), "missing device logged to the BMC SEL", f)
    expect(r["tests"].get("driver.load", {}).get("result") == "PASS", "driver still loads cleanly", f)
    return r, f


def s_no_bmc():
    r = boot("no_bmc", bmc=False)
    f = []
    t = r["tests"].get("bmc.present", {})
    expect(t.get("result") == "FAIL" and "no BMC" in t.get("detail", ""), "absent BMC detected", f)
    others = {k: v for k, v in r["tests"].items() if k != "bmc.present"}
    expect(others and all(v["result"] == "PASS" for v in others.values()), "everything else still runs and passes", f)
    return r, f


def s_hang():
    r = boot("hang", hang=True)
    f = []
    expect(len(r["starts"]) == 2, "machine reset once by the BMC watchdog", f)
    expect("hang_at" in r, "first boot hung", f)
    if len(r["starts"]) == 2 and "hang_at" in r:
        r["recovery_s"] = round(r["starts"][1] - r["hang_at"], 1)
        expect(8.0 <= r["recovery_s"] <= 25.0, "reset arrives after the 10 s watchdog period", f)
    expect(any("safe-mode boot after watchdog reset" in s for s in r["sel"]), "recovery logged to the SEL", f)
    expect(r.get("result") == "PASS", "second boot passes in safe mode", f)
    return r, f


def s_persistence():
    work = tempfile.mkdtemp(prefix="pd-vars-")
    vars_file = os.path.join(work, "vars.fd")
    shutil.copy(find_ovmf()[1], vars_file)
    a = boot("persist_1", vars_file=vars_file)
    b = boot("persist_2", vars_file=vars_file)
    shutil.rmtree(work, ignore_errors=True)
    f = []
    expect(a.get("boot_count") == 1 and b.get("boot_count") == 2, "boot counter survives power-off in NV variable", f)
    b["boot_counts"] = [a.get("boot_count"), b.get("boot_count")]
    return b, f


def s_smbios3():
    r = boot("smbios3", smbios64=True)
    f = []
    expect("3.0 entry point" in r["tests"].get("smbios.entry_point", {}).get("detail", ""), "SMBIOS 3.0 path used", f)
    expect(r.get("result") == "PASS", "all checks pass", f)
    return r, f


SCENARIOS = {
    "baseline": ("1 edu device, BMC, 256 MiB", s_baseline),
    "high_memory": ("1 GiB RAM: DMA buffers above the device's 28-bit limit", s_high_memory),
    "two_devices": ("2 edu devices", s_two_devices),
    "no_device": ("edu device missing", s_no_device),
    "no_bmc": ("no BMC on the KCS port", s_no_bmc),
    "hang": ("POST stage hangs; BMC watchdog must reset and the next boot recover", s_hang),
    "persistence": ("two power-ons with the same NV variable store", s_persistence),
    "smbios3": ("SMBIOS 3.0 (64-bit) entry point", s_smbios3),
}


def main(argv):
    missing = [n for n, v in (("qemu-system-x86_64", find_qemu()), ("OVMF", find_ovmf()[0]),
                              ("PlatformDiag.efi/EduDxe.efi", find_efi()[0])) if not v]
    if missing:
        print("missing: " + ", ".join(missing) + " (see README)")
        return 2
    names = argv or list(SCENARIOS)
    rows, ok = [], True
    for n in names:
        desc, fn = SCENARIOS[n]
        r, failures = fn()
        r.pop("lines", None)
        row = {"scenario": n, "description": desc, "ok": not failures, "failures": failures, **r}
        rows.append(row)
        ok &= not failures
        print(f"{'PASS' if not failures else 'FAIL'}  {n:12s} {r.get('result', '-'):4s} "
              f"{r.get('passed', '-')}/{r.get('total', '-')} checks  {r['seconds']:5.1f} s  {desc}"
              + ("" if not failures else "  <- " + "; ".join(failures)), flush=True)
    os.makedirs(RESULTS, exist_ok=True)
    with open(os.path.join(RESULTS, "scenarios.json"), "w") as f:
        json.dump(rows, f, indent=2)
    write_md(rows)
    return 0 if ok else 1


def write_md(rows):
    L = ["# Scenario results", "", "Generated by `python3 tools/run_qemu.py` (QEMU, OVMF, PlatformDiag + EduDxe). "
         "Logs: `results/logs/`.", "",
         "| Scenario | Setup | Expectations met | PlatformDiag result | Key numbers |", "|---|---|---|---|---|"]
    for r in rows:
        key = []
        if "dma_direct" in r and r["scenario"] in ("baseline", "high_memory"):
            d = r["dma_direct"]
            key.append(f"direct DMA from {d['host']}: {d['bytes_wrong']} of 4096 bytes wrong")
        if "recovery_s" in r:
            key.append(f"hang to next boot {r['recovery_s']} s")
        if "boot_counts" in r:
            key.append(f"boot count {r['boot_counts'][0]} then {r['boot_counts'][1]}")
        nl = r["tests"].get("driver.no_leak", {}).get("detail")
        if nl and r["scenario"] in ("baseline", "two_devices"):
            key.append(nl)
        L.append(f"| {r['scenario']} | {r['description']} | {'yes' if r['ok'] else 'NO: ' + '; '.join(r['failures'])} | "
                 f"{r.get('result', '-')} {r.get('passed', '-')}/{r.get('total', '-')} | {'; '.join(key)} |")
    with open(os.path.join(RESULTS, "RESULTS.md"), "w") as f:
        f.write("\n".join(L) + "\n")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
