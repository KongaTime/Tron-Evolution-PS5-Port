import struct,sys
d=open(sys.argv[1],'rb').read()
magic,flags,pe_off,res,sec_off,nopt=struct.unpack_from('>4sIIIII',d,0)
print(magic,hex(flags),hex(pe_off),hex(sec_off),nopt)
for i in range(nopt):
    k,v=struct.unpack_from('>II',d,24+8*i)
    print(hex(k),hex(v))
    if k==0x3FF:
        sz,enc,comp=struct.unpack_from('>IHH',d,v); print(' fileformat enc',enc,'comp',comp,'size',sz)
