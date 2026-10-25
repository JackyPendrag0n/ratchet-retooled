import sys,capstone
d=open(sys.argv[1],'rb').read()
cs=capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_64|capstone.CS_MODE_BIG_ENDIAN); cs.skipdata=True
a=int(sys.argv[2],16); n=int(sys.argv[3])
o=a-0x10000
for i in cs.disasm(d[o:o+4*n],a): print('%x  %08x  %s %s'%(i.address,int.from_bytes(i.bytes,'big'),i.mnemonic,i.op_str))
