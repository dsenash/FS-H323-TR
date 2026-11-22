# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import struct, sys, collections
def read(fn):
    d=open(fn,'rb').read()
    magic=d[:4]
    if magic in (b'\xd4\xc3\xb2\xa1',b'\xa1\xb2\xc3\xd4'):
        e='<' if magic==b'\xd4\xc3\xb2\xa1' else '>'
        ver=struct.unpack(e+'HH',d[4:8]); lt=struct.unpack(e+'I',d[20:24])[0]
        off=24; n=0
        while off+16<=len(d):
            ts,tu,cl,ol=struct.unpack(e+'IIII',d[off:off+16]); off+=16
            yield n,ts+tu/1e6,lt,d[off:off+cl]; off+=cl; n+=1
    elif magic==b'\n\r\r\n':
        off=0; n=0; lts={}; ifc=0; tsr={}
        while off+12<=len(d):
            bt,bl=struct.unpack('<II',d[off:off+8]); body=d[off+8:off+bl-4]
            if bt==1:
                lts[ifc]=struct.unpack('<H',body[:2])[0]; res=1e6; o=8
                while o+4<=len(body):
                    oc,ol=struct.unpack('<HH',body[o:o+4])
                    if oc==0: break
                    if oc==9:
                        v=body[o+4]; res=(2**(v&0x7f)) if v&0x80 else 10**(v&0x7f)
                    o+=4+((ol+3)&~3)
                tsr[ifc]=res; ifc+=1
            elif bt==6:
                i,th,tl,cl,ol=struct.unpack('<IIIII',body[:20]); t=((th<<32)|tl)/tsr.get(i,1e6)
                yield n,t,lts.get(i,1),body[20:20+cl]; n+=1
            elif bt==3:
                ol=struct.unpack('<I',body[:4])[0]; yield n,0,lts.get(0,1),body[4:4+ol]; n+=1
            off+=bl
    else:
        print('unknown', magic); return
def parse(fr,lt):
    if lt==1: 
        et=struct.unpack('>H',fr[12:14])[0]; p=14
        if et==0x8100: et=struct.unpack('>H',fr[16:18])[0]; p=18
        if et!=0x0800: return None
    elif lt==113: p=16
    elif lt==276: p=20
    elif lt==101: p=0
    else: return None
    ip=fr[p:]
    if ip[0]>>4!=4: return None
    ihl=(ip[0]&15)*4; proto=ip[9]; src='.'.join(map(str,ip[12:16])); dst='.'.join(map(str,ip[16:20]))
    tl=struct.unpack('>H',ip[2:4])[0]; pl=ip[ihl:tl]
    if proto==6:
        sp,dp,seq,ack,off,fl=struct.unpack('>HHIIBB',pl[:14]); h=(off>>4)*4
        return ('tcp',src,sp,dst,dp,fl,pl[h:],seq)
    if proto==17:
        sp,dp=struct.unpack('>HH',pl[:4]); return ('udp',src,sp,dst,dp,0,pl[8:],0)
    return ('ip%d'%proto,src,0,dst,0,0,pl,0)
if __name__=='__main__':
    fn=sys.argv[1]; flows=collections.OrderedDict(); first=None
    for n,t,lt,fr in read(fn):
        r=parse(fr,lt)
        if not r: continue
        if first is None: first=t
        k=(r[0],r[1],r[2],r[3],r[4]); 
        f=flows.setdefault(k,[0,0,t-first]); f[0]+=1; f[1]+=len(r[6])
    for k,v in flows.items(): print(k,v)
