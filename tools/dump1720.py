# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import sys
sys.path.insert(0,'/home/claude/tools')
from pcapdump import read,parse
fn=sys.argv[1]; ports=set(map(int,sys.argv[2].split(',')))
first=None
for n,t,lt,fr in read(fn):
    r=parse(fr,lt)
    if not r or r[0] not in('tcp','udp'): continue
    if r[2] not in ports and r[4] not in ports: continue
    if first is None: first=t
    fl=r[5]; fs=''.join(c for b,c in ((2,'S'),(16,'A'),(8,'P'),(1,'F'),(4,'R')) if fl&b) if r[0]=='tcp' else ''
    print(f"#{n} t={t-first:.3f} {r[0]} {r[1]}:{r[2]} -> {r[3]}:{r[4]} {fs} len={len(r[6])}")
    if r[6]: print('   ',r[6].hex())
