"""Checks the claim "nothing is sent anywhere": the .exe must not link or name any Windows networking library.

Usage: python tests/check_imports.py build/music-visualizer.exe
"""
import struct
import sys

NETWORK = {"ws2_32", "wsock32", "mswsock", "wininet", "winhttp", "urlmon", "webio", "iphlpapi", "dnsapi",
           "netapi32", "rasapi32", "httpapi", "websocket", "bits", "wlanapi"}

data = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe:pe + 4] == b"PE\0\0", "not a Windows program"
nsec, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
opt = pe + 24
assert struct.unpack_from("<H", data, opt)[0] == 0x20B, "expected a 64-bit program"
import_rva = struct.unpack_from("<I", data, opt + 112 + 8)[0]
sections = [struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8) for i in range(nsec)]  # vsize, rva, rawsize, rawptr


def offset(rva):
    for vsize, start, rawsize, rawptr in sections:
        if start <= rva < start + max(vsize, rawsize):
            return rawptr + rva - start
    raise ValueError(f"address {rva:#x} is outside every section")


def text(off):
    return data[off:data.index(b"\0", off)].decode("ascii")


linked, pos = [], offset(import_rva)
while True:
    name_rva = struct.unpack_from("<I", data, pos + 12)[0]
    if not name_rva:
        break
    linked.append(text(offset(name_rva)).lower())
    pos += 20

print("linked libraries:", ", ".join(sorted(linked)))
lowered = data.lower()
problems = [f"links {dll}" for dll in linked if dll.rsplit(".", 1)[0] in NETWORK]
for lib in sorted(NETWORK):  # libraries loaded by name at run time would show up as text
    for needle in ((lib + ".dll").encode("ascii"), (lib + ".dll").encode("utf-16-le")):
        if needle in lowered:
            problems.append(f"mentions {lib}.dll")
if problems:
    print("FAIL  networking found:", "; ".join(sorted(set(problems))))
    sys.exit(1)
print("PASS  no networking library is linked or named")
