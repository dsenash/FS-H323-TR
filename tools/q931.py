# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import sys
sys.path.insert(0,'/home/claude/tools')
from pcapdump import read,parse
MT={0x05:'SETUP',0x02:'CALL_PROC',0x01:'ALERTING',0x07:'CONNECT',0x0f:'CONNECT_ACK',0x03:'PROGRESS',0x0d:'SETUP_ACK',0x5a:'RELEASE_COMPLETE',0x62:'FACILITY',0x7d:'STATUS',0x75:'STATUS_ENQ',0x6e:'NOTIFY',0x7b:'INFORMATION',0x45:'DISCONNECT',0x4d:'RELEASE'}
UU=['setup','callProceeding','connect','alerting','information','releaseComplete','facility','progress','empty','status','statusInquiry','setupAck','notify']
def q931(b):
    out=[]; 
    # TPKT
    i=0
    while i+4<=len(b) and b[i]==3:
        ln=(b[i+2]<<8)|b[i+3]; m=b[i+4:i+ln]; i+=ln
        if len(m)<5: out.append('short'); continue
        cl=m[1]; cr=m[2:2+cl]; mt=m[2+cl]; p=3+cl
        ies=[]
        while p<len(m):
            ie=m[p]
            if ie&0x80: ies.append('%02x'%ie); p+=1; continue
            if ie==0x7e: l=(m[p+1]<<8)|m[p+2]; v=m[p+3:p+3+l]; p+=3+l
            else: l=m[p+1]; v=m[p+2:p+2+l]; p+=2+l
            if ie==0x7e:
                # user-user: first byte protocol disc 5, then PER
                per=v[1:]; 
                ies.append('UUIE[%s]'%per.hex())
            else: ies.append('%02x:%s'%(ie,v.hex()))
        out.append('%s cr=%s dir=%s %s'%(MT.get(mt,hex(mt)),cr.hex(),'orig' if cr[0]&0x80==0 else 'dest',' '.join(ies)))
    return out
if __name__=='__main__':
    fn=sys.argv[1]; first=None
    for n,t,lt,fr in read(fn):
        r=parse(fr,lt)
        if not r or r[0]!='tcp' or not r[6]: continue
        if 1720 not in (r[2],r[4]): continue
        if first is None: first=t
        for s in q931(r[6]): print('t=%.2f %s:%d>%s:%d %s'%(t-first,r[1],r[2],r[3],r[4],s))
