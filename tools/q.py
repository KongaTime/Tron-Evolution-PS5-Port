import struct,sys
img=open(sys.argv[1],'rb').read();B=0x82000000
t=int(sys.argv[2],16)
w=lambda a: struct.unpack_from('>I',img,a-B)[0]
print('prev %08X at %08X %08X'%(w(t-4),w(t),w(t+4)))
hits=[B+i for i in range(0,len(img)-3,4) if struct.unpack_from('>I',img,i)[0]==t]
print('data refs',[hex(h) for h in hits[:10]])
# lis/addi pairs in code referencing t
hi=(t+0x8000)>>16 &0xFFFF; lo=t&0xFFFF
n=0
for i in range(0x2B0000,0x1380000,4):
    x=struct.unpack_from('>I',img,i)[0]
    if x>>26==15 and (x&0xFFFF)==hi and (x>>16&0x1F)==0:
        for j in range(1,12):
            y=struct.unpack_from('>I',img,i+4*j)[0]
            if (y>>26) in (14,) and (y&0xFFFF)==lo: print('lis/addi at %08X'%(B+i)); n+=1;break
print('code refs',n)
