#!/bin/bash
# Generate the expected values used by test_hub.c, from independent oracles
# (rhash for Tiger, Python for base32) rather than from our own code.
set -euo pipefail
command -v rhash >/dev/null || { echo "need rhash (brew install rhash)" >&2; exit 1; }

python3 - <<'PY'
import base64, subprocess

def tiger(b):
    out = subprocess.run(['rhash','--tiger','-'], input=b, capture_output=True)
    return bytes.fromhex(out.stdout.decode().split()[0])

def b32(b):
    return base64.b32encode(b).decode().rstrip('=')

pid = bytes(range(24))
print("PID  bytes 0x00..0x17")
print("  pid_b32 =", b32(pid))
print("  cid     =", b32(tiger(pid)))
print()
# The IGPA challenge used in the tests is the same byte pattern, base32'd.
random = pid
print("challenge b32 =", b32(random))
for pw in ("secret", ""):
    resp = b32(tiger(pw.encode('utf-8') + random))
    print(f'  password {pw!r:10} -> {resp}')
PY
