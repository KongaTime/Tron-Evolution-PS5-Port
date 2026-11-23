"""Match functions between two ReXGlue codegen outputs by their PPC disassembly comments.

Usage: fnmatch.py <dante-gen-dir> <tron-gen-dir> <addr-list-file> [out]
addr-list-file: one Dante address (hex) per line, optional name after a space.
Each function's instruction list is normalised (branch targets and absolute
addresses dropped) and fingerprinted. Exact fingerprint matches are reported;
otherwise the best fuzzy match by instruction-sequence similarity.
"""
import difflib, glob, os, pickle, re, sys

FUNC_RE = re.compile(r'^DEFINE_REX_FUNC\((?:__imp__)?sub_([0-9A-F]{8})\)')
COMMENT_RE = re.compile(r'^\t// (.*)$')


def norm(ins):
    ins = ins.strip()
    m = ins.split(' ', 1)
    op = m[0]
    args = m[1] if len(m) > 1 else ''
    # branch targets / absolute addresses
    args = re.sub(r'0x8[0-9a-f]{7}', 'A', args)
    if op in ('lis',):
        args = re.sub(r',-?\d+$', ',K', args)  # high halves of addresses
    if op in ('addi', 'lwz', 'stw', 'lfs', 'stfs', 'lhz', 'lbz', 'stb', 'sth', 'ld', 'std', 'lfd', 'stfd') and re.search(r'r(?:1[1-2]|10|9|8|7)\b', args):
        pass
    return op + ' ' + args


def load(dirname, cache):
    if os.path.exists(cache):
        return pickle.load(open(cache, 'rb'))
    funcs = {}
    for f in sorted(glob.glob(os.path.join(dirname, '*_recomp.*.cpp'))):
        cur = None
        for line in open(f, encoding='utf-8', errors='replace'):
            m = FUNC_RE.match(line)
            if m:
                cur = []
                funcs[int(m.group(1), 16)] = cur
                continue
            if cur is not None:
                c = COMMENT_RE.match(line)
                if c:
                    cur.append(norm(c.group(1)))
    pickle.dump(funcs, open(cache, 'wb'))
    return funcs


def main():
    dg, tg, lst = sys.argv[1:4]
    D = load(dg, dg.rstrip('/\\') + '.pkl')
    T = load(tg, tg.rstrip('/\\') + '.pkl')
    print(f'dante {len(D)} funcs, tron {len(T)} funcs', file=sys.stderr)
    tindex = {}
    for a, ins in T.items():
        tindex.setdefault(tuple(ins), []).append(a)
    # coarse buckets by length for fuzzy search
    tlist = sorted(T.items())
    for line in open(lst):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(None, 1)
        a = int(parts[0], 16)
        name = parts[1] if len(parts) > 1 else ''
        ins = D.get(a)
        if ins is None:
            print(f'{a:08X} {name}: not in dante codegen')
            continue
        exact = tindex.get(tuple(ins), [])
        if len(exact) == 1:
            print(f'{a:08X} -> {exact[0]:08X} exact   n={len(ins):4d} {name}')
            continue
        best = []
        n = len(ins)
        for ta, tins in tlist:
            if abs(len(tins) - n) > max(8, n // 5):
                continue
            sm = difflib.SequenceMatcher(None, ins, tins, autojunk=False)
            if sm.real_quick_ratio() < 0.6 or sm.quick_ratio() < 0.6:
                continue
            r = sm.ratio()
            if r >= 0.6:
                best.append((r, ta))
        best.sort(reverse=True)
        tag = 'multi' if len(exact) > 1 else 'fuzzy'
        cands = ' '.join(f'{ta:08X}:{r:.2f}' for r, ta in best[:3])
        print(f'{a:08X} -> {tag:5s} n={n:4d} {name}  {cands}  {"exact:" + ",".join("%08X" % x for x in exact[:4]) if exact else ""}')


if __name__ == '__main__':
    main()
