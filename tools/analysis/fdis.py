import glob,re,sys,os
d=sys.argv[1]; a=sys.argv[2].upper()
for f in glob.glob(os.path.join(d,'*_recomp.*.cpp')):
    s=open(f,encoding='utf-8',errors='replace').read()
    i=s.find('DEFINE_REX_FUNC(sub_%s)'%a)
    if i<0: continue
    j=s.find('\nDEFINE_REX_FUNC(',i+10)
    body=s[i:j if j>0 else len(s)]
    for line in body.split('\n'):
        m=re.match(r'\t// (.*)',line)
        if m: print(m.group(1))
        elif line.startswith('loc_'): print(line)
    break
