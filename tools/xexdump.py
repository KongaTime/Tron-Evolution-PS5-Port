"""Decrypt + decompress an XEX2 (AES retail, basic compression) into a flat image.
usage: xexdump.py default.xex out.bin   -> also prints the section map (sections.txt beside out)"""
import struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
RETAIL = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')
d = open(sys.argv[1], 'rb').read()
_, flags, pe_off, _, sec, nopt = struct.unpack_from('>4sIIIII', d, 0)
opts = dict(struct.unpack_from('>II', d, 24 + 8 * i) for i in range(nopt))
ff = opts[0x3FF]
ffsize, enc, comp = struct.unpack_from('>IHH', d, ff)
image_size = struct.unpack_from('>I', d, sec + 4)[0]
image_flags = struct.unpack_from('>I', d, sec + 0x10C)[0]
load = struct.unpack_from('>I', d, sec + 0x110)[0]
key = d[sec + 0x150:sec + 0x160]
npages = struct.unpack_from('>I', d, sec + 0x180)[0]
dec = lambda k, b, m: Cipher(algorithms.AES(k), m).decryptor().update(b)
session = dec(RETAIL, key, modes.ECB())
body = d[pe_off:]
body = body[:len(body) // 16 * 16]
if enc == 1:
    body = dec(session, body, modes.CBC(b'\0' * 16))
assert comp == 1, comp
out = bytearray()
p = 0
for i in range((ffsize - 8) // 8):
    ds, zs = struct.unpack_from('>II', d, ff + 8 + 8 * i)
    out += body[p:p + ds]; p += ds
    out += b'\0' * zs
out = out[:image_size] + b'\0' * max(0, image_size - len(out))
assert out[:2] == b'MZ', out[:4]
open(sys.argv[2], 'wb').write(out)
page = 0x1000 if image_flags & 0x10000000 else 0x10000
addr = load
secs = []
for i in range(npages):
    info = struct.unpack_from('>I', d, sec + 0x184 + 24 * i)[0]
    n, t = info >> 4, info & 0xF
    secs.append((addr, addr + n * page, {1: 'code', 2: 'data', 3: 'rodata'}.get(t, str(t))))
    addr += n * page
# merge runs
merged = []
for s in secs:
    if merged and merged[-1][2] == s[2] and merged[-1][1] == s[0]:
        merged[-1] = (merged[-1][0], s[1], s[2])
    else:
        merged.append(s)
with open(sys.argv[2] + '.sections.txt', 'w') as f:
    for a, b, t in merged:
        f.write(f'{a:08X} {b:08X} {t}\n'); print(f'{a:08X}-{b:08X} {t}')
print('image', hex(load), hex(image_size), 'entry', hex(opts.get(0x10100, 0)))
