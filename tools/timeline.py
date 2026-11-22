# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import sys
sys.path.insert(0,'/home/claude/mod_aa_h323_tr/tools')
from pcapdump import read,parse
from q931 import MT
fn=sys.argv[1]; first=None; hexmode=len(sys.argv)>2
for n,t,lt,fr in read(fn):
    r=parse(fr,lt)
    if not r or r[0]!='tcp' or not r[6] or 1720 not in (r[2],r[4]): continue
    if first is None: first=t
    b=r[6]; i=0
    while i+5<=len(b) and b[i]==3:
        ln=(b[i+2]<<8)|b[i+3]; m=b[i+4:i+ln]; i+=ln
        cl=m[1]; mt=m[2+cl]; p=3+cl; info=[]
        while p<len(m):
            ie=m[p]
            if ie&0x80: p+=1; continue
            if ie==0x7e: l=(m[p+1]<<8)|m[p+2]; v=m[p+3:p+3+l]; p+=3+l; info.append('uu%d'%l)
            else:
                l=m[p+1]; v=m[p+2:p+2+l]; p+=2+l
                if ie==0x08: info.append('cause=%d'%(v[1]&0x7f))
                elif ie==0x14: info.append('callstate=%d'%v[0])
                elif ie==0x70: info.append('called=%s'%v[1:].decode())
                elif ie==0x6c: info.append('calling=%s'%v[2:].decode(errors='replace'))
                elif ie==0x28: info.append('disp=%s'%v.decode(errors='replace'))
                elif ie==0x1e: info.append('prog=%s'%v.hex())
        print('%9.2f %s:%d>%s:%d cr=%s %s %s'%(t-first,r[1].split('.')[-1],r[2],r[3].split('.')[-1],r[4],m[2:2+cl].hex(),MT.get(mt,hex(mt)),' '.join(info)))
        if hexmode and mt not in (0x62,): print('      ',m.hex())
