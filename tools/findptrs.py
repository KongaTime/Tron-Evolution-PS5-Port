"""Find function entries reached only through pointers: dwords in data sections
and lis/addi (or lis/ori) pairs in code that build a code address.
usage: findptrs.py image.bin generated_dir out.txt"""
import struct, sys, re, glob, collections
img = open(sys.argv[1], 'rb').read()
gen = sys.argv[2]
BASE = 0x82000000
secs = [l.split() for l in open(sys.argv[1] + '.sections.txt')]
code = [(int(a, 16), int(b, 16)) for a, b, t in secs if t == 'code'][0]
data = [(int(a, 16), int(b, 16)) for a, b, t in secs if t != 'code']
entries = set(int(x, 16) for x in re.findall(r'SetFunction\(0x([0-9A-F]+)', open(f'{gen}/tronevolution_register.cpp').read()))
labels = set()
for f in glob.glob(f'{gen}/tronevolution_recomp.*.cpp'):
    labels.update(int(x, 16) for x in re.findall(r'^loc_([0-9A-F]{8}):', open(f).read(), re.M))
w = lambda a: struct.unpack_from('>I', img, a - BASE)[0]
def starts_func(t):
    prev = w(t - 4)
    op = prev >> 26
    return prev in (0x4E800020, 0x4E800420, 0) or (op == 18 and not prev & 1)
import bisect
ent_sorted = sorted(entries); lab_sorted = sorted(labels)
def inside_body(v):
    # a label between v and the next known entry means the surrounding function's
    # code continues past v: v is dead space inside a function, not an entry
    i = bisect.bisect_right(ent_sorted, v)
    nxt = ent_sorted[i] if i < len(ent_sorted) else code[1]
    j = bisect.bisect_right(lab_sorted, v)
    return j < len(lab_sorted) and lab_sorted[j] < nxt
def ok(v):
    return (code[0] <= v < code[1] and not v & 3 and v not in entries and v not in labels
            and w(v) != 0 and starts_func(v) and not inside_body(v))
cands = collections.Counter(); refs = {}; kind = {}
def add(v, p, k):
    cands[v] += 1; refs.setdefault(v, p); kind.setdefault(v, k)
for a, b in data:
    for p in range(a, b, 4):
        v = w(p)
        if ok(v): add(v, p, 'data')
for p in range(code[0], code[1], 4):
    x = w(p)
    if x >> 26 != 15 or (x >> 16) & 0x1F != 0:  # lis rD,hi  (addis rD,0,hi)
        continue
    rd, hi = (x >> 21) & 0x1F, x & 0xFFFF
    for j in range(1, 24):
        y = w(p + 4 * j)
        op, yd, ya = y >> 26, (y >> 21) & 0x1F, (y >> 16) & 0x1F
        if op == 14 and ya == rd:  # addi
            v = ((hi << 16) + (y & 0xFFFF) - (0x10000 if y & 0x8000 else 0)) & 0xFFFFFFFF
            if ok(v): add(v, p, 'code')
            if yd == rd: break
        elif op == 24 and yd == rd:  # ori rA=rS? ori form: rS=yd, rA=ya
            pass
        if op == 24 and (y >> 21) & 0x1F == rd:  # ori rA,rS,lo with rS==rd
            v = (hi << 16) | (y & 0xFFFF)
            if ok(v): add(v, p, 'code-ori')
        if (op in (14, 15, 32, 33) and yd == rd and ya != rd) or y in (0x4E800020, 0x4E800420):
            break  # rd overwritten or function end
print('known functions', len(entries), 'labels', len(labels), 'candidates', len(cands),
      collections.Counter(kind.values()))
with open(sys.argv[3], 'w') as f:
    for v in sorted(cands):
        f.write(f'{v:08X} {cands[v]} {refs[v]:08X} {kind[v]}\n')
print('0x8290AE10 found:', 0x8290AE10 in cands)
