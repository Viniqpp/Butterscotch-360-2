# Lists the functions each script calls, straight from a data.win (no decompiler needed).
# Usage: python tools/xdk/list_calls.py data.win "<script name regex>"
import struct,sys,re,collections,bisect
d=open(sys.argv[1],'rb').read(); pat=re.compile(sys.argv[2])
ch={}; i=8
while i<len(d):
    n=d[i:i+4].decode(); l=struct.unpack_from('<I',d,i+4)[0]; ch[n]=(i+8,l); i+=8+l
s=lambda p:d[p:d.index(b'\0',p)].decode('utf8','replace')
co,_=ch['CODE']; cnt=struct.unpack_from('<I',d,co)[0]
codes=[]
for p in struct.unpack_from('<%dI'%cnt,d,co+4):
    name=s(struct.unpack_from('<I',d,p)[0]); ln=struct.unpack_from('<I',d,p+4)[0]
    rel=struct.unpack_from('<i',d,p+12)[0]; start=p+12+rel
    codes.append((start,start+ln,name))
codes.sort(); starts=[c[0] for c in codes]
fo,_=ch['FUNC']; fc=struct.unpack_from('<I',d,fo)[0]
calls=collections.defaultdict(collections.Counter)
for k in range(fc):
    np,occ,addr=struct.unpack_from('<III',d,fo+4+k*12); fn=s(np); addr-=4
    for _ in range(occ):
        j=bisect.bisect_right(starts,addr)-1
        if j>=0 and addr<codes[j][1]: calls[codes[j][2]][fn]+=1
        op=struct.unpack_from('<I',d,addr+4)[0]; ins=struct.unpack_from('<I',d,addr)[0]
        nxt=op&(0x00FFFFFF if (ins>>24)==0xFF else 0x07FFFFFF)
        addr+=nxt
for cn in sorted(calls):
    if pat.search(cn): print(cn, dict(calls[cn].most_common()))
