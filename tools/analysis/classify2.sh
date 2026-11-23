E=$HOME/PS5_ReXGlueTemplate/build/title-tron-native/link/llvm-pie.elf
N=$(command -v llvm-nm-21 || command -v llvm-nm)
[ -f /tmp/syms_sorted.txt ] && [ /tmp/syms_sorted.txt -nt $E ] || $N -n --defined-only -C $E | awk '{a=$1; $1=""; $2=""; print a"\t"substr($0,3)}' > /tmp/syms_sorted.txt
python3 - "$1" <<'PY'
import sys,bisect,collections
syms=[]
for l in open('/tmp/syms_sorted.txt'):
    p=l.rstrip('\n').split('\t',1)
    if len(p)==2:
        try: syms.append((int(p[0],16),p[1]))
        except: pass
syms.sort(); keys=[s[0] for s in syms]
cls=collections.Counter(); fn=collections.Counter()
for l in open(sys.argv[1]):
    a,n=l.split(); a=int(a,16); n=int(n)
    if a>=0x800000000: cls['kernel']+=n; continue
    i=bisect.bisect_right(keys,a)-1; name=syms[i][1] if i>=0 else '?'
    if name.startswith('sub_8') or name.startswith('__imp__sub_') or 'savegpr' in name or 'restgpr' in name: k='guest code'
    elif name.startswith('renderer::'): k='native renderer'
    elif any(x in name for x in ('Diligent','radv','ac_','vk_','nir','aco','si_','volk')): k='vulkan/diligent/radv'
    elif 'rex::' in name: k='runtime'
    else: k='other'
    cls[k]+=n; fn[name[:90]]+=n
for k,v in cls.most_common(): print(v,k)
print('--- top guest/other functions')
for k,v in fn.most_common(25): print(v,k)
PY
