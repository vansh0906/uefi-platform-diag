"""pytest entry point: host unit tests always; QEMU scenarios when QEMU, OVMF and the EFI files are available."""
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import run_qemu  # noqa: E402


def test_host_unit_tests():
    out = subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "tests", "host"), "check"], capture_output=True, text=True)
    print(out.stdout, out.stderr)
    assert out.returncode == 0 and ", 0 failed" in out.stdout


@pytest.mark.parametrize("scenario", list(run_qemu.SCENARIOS))
def test_qemu_scenario(scenario):
    if not (run_qemu.find_qemu() and run_qemu.find_ovmf()[0] and run_qemu.find_efi()[0]):
        pytest.skip("QEMU / OVMF / EFI files not found")
    _, failures = run_qemu.SCENARIOS[scenario][1]()
    assert not failures, failures
