import sys
img=open(sys.argv[1],'rb').read();B=0x82000000
for a in sys.argv[2:]:
    o=int(a,16)-B; s=img[o:o+160]
    if s[1:2]==b'\0': # utf-16be?
        t=s.decode('utf-16-be','replace').split('\0')[0]
    elif s[0:1]==b'\0' and s[1:2]!=b'\0':
        t=s.decode('utf-16-be','replace').split('\0')[0]
    else: t=s.split(b'\0')[0].decode('latin1')
    print(a, repr(t[:80]))
