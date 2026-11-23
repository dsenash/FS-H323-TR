#!/usr/bin/env python3
# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""gen_tr.py — генератор шаблонов сообщений транка из дампов (как gen_*.py в mod_aa_h323).

Берёт эталонные сообщения из captures/, выделяет в каждом UUIE «точки подстановки» (GUID, IP/порт RTP,
сигнальный адрес) и пишет:
  aa_tr_tpl.h        — шаблоны (префикс/суффикс UUIE, смещения), H.245 PDU
  tests/test_tr_vec.h — все кадры всех дампов для побайтной проверки parse -> build
Запуск: python3 -I tools/gen_tr.py   (из корня проекта)
"""
LICENSE_HDR = (
    "/*\n"
    " * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)\n"
    " * Copyright (c) 2026: Сенашенко Дмитрий.\n"
    " * Разработано в Российской Федерации.\n"
    " * This source code is licensed under the MIT license found in the\n"
    " * LICENSE file in the root directory of this source tree.\n"
    " */\n\n")

import sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from pcapdump import read, parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPS = ['baeb083d-h323_out_101_102.pcap', '9b20d04c-h323_inc_101_102.pcap',
        '0c0bfc86-h323_out_b_o.pcap', 'aeeb35f5-h323_out_pn4.pcap',
        '2111-Al.pcap', '2111-Ml.pcap', '2111-AMl.pcap']
P_GUID, P_IP, P_RTP, P_RTCP, P_SIGIP, P_SIGPORT, P_X2, P_CODEC = 1, 2, 3, 4, 5, 6, 7, 8


def frames(fn):
    """[(connkey, src_last_octet, frame_bytes)] в порядке следования."""
    out = []
    for n, t, lt, fr in read(os.path.join(os.environ.get('AA_TR_CAPTURES', os.path.join(ROOT, 'captures')), fn)):
        r = parse(fr, lt)
        if not r or r[0] != 'tcp' or not r[6] or 1720 not in (r[2], r[4]):
            continue
        if {r[1], r[3]} != {'192.168.0.193', '192.168.0.6'}:
            continue          # только обмен АТС<->АТС (в дампах есть чужой трафик)
        key = (r[1], r[2]) if r[4] == 1720 else (r[3], r[4])
        b = r[6]
        i = 0
        while i + 5 <= len(b) and b[i] == 3:
            ln = (b[i + 2] << 8) | b[i + 3]
            out.append((key, int(r[1].split('.')[-1]), bytes(b[i:i + ln])))
            i += ln
    return out


def q931(f):
    m = f[4:]
    cl = m[1]
    mt = m[2 + cl]
    p = 3 + cl
    ies = []
    while p < len(m):
        ie = m[p]
        if ie & 0x80:
            ies.append((ie, b'')); p += 1; continue
        if ie == 0x7e:
            l = (m[p + 1] << 8) | m[p + 2]; v = m[p + 3:p + 3 + l]; p += 3 + l
        else:
            l = m[p + 1]; v = m[p + 2:p + 2 + l]; p += 2 + l
        ies.append((ie, v))
    return mt, m[2:2 + cl], ies


def uuie(f):
    mt, cr, ies = q931(f)
    for ie, v in ies:
        if ie == 0x7e:
            assert v[0] == 5
            return mt, v[1:]
    return mt, None


def split_vendor(u):
    p = u.find(b'\x00\x4c\x54')
    if p < 0:
        return None
    lp = u[p + 3] + 1
    lv = u[p + 4 + lp] + 1
    vend = p + 4 + lp + 1 + lv
    return p + 3, vend   # prefix = u[:p+3], suffix = u[vend:]


def find_all(hay, needle):
    r, i = [], 0
    while True:
        i = hay.find(needle, i)
        if i < 0:
            return r
        r.append(i); i += 1


def media_pair(u):
    """(ip4, rtp_port) по шаблону 00 ip p1 00 ip p1+1."""
    for i in range(len(u) - 13):
        if u[i] == 0 and u[i + 7] == 0 and u[i + 1:i + 5] == u[i + 8:i + 12] and u[i + 1:i + 5] != b'\0\0\0\0':
            p1 = (u[i + 5] << 8) | u[i + 6]
            p2 = (u[i + 12] << 8) | u[i + 13]
            if p2 == p1 + 1 and p1 >= 1024:
                return u[i + 1:i + 5], p1
    return None


def hexarr(name, b):
    s = ', '.join('0x%02x' % x for x in b)
    return 'static const uint8_t %s[%d] = {%s};\n' % (name, max(len(b), 1), s if b else '0')


def codec_offsets(suf):
    """смещения байта кодека (0x20 = G.711A, 0x60 = G.711u) в OLC: шаблон (0c|4c) cc 13 80."""
    return [i + 1 for i in range(len(suf) - 3) if suf[i] in (0x0c, 0x4c) and suf[i + 1] in (0x20, 0x60) and suf[i + 2:i + 4] == b'\x13\x80']


def make_class(name, sample_u, guid, sig=None, x2=False, codec=False):
    """шаблон одного UUIE. patches: (off_in_suffix, kind)."""
    v = split_vendor(sample_u)
    if v:
        pre, vend = sample_u[:v[0]], v[1]
        suf = sample_u[vend:]
    else:
        pre, suf = b'', sample_u
    patches = []
    for o in find_all(suf, guid):
        patches.append((o, P_GUID))
    if v:
        mp = media_pair(suf)
        if mp:
            ip, rtp = mp
            for o in find_all(suf, ip):
                if sig and o in sig:
                    continue
                patches.append((o, P_IP))
            for o in find_all(suf, bytes([rtp >> 8, rtp & 255])):
                patches.append((o, P_RTP))
            for o in find_all(suf, bytes([(rtp + 1) >> 8, (rtp + 1) & 255])):
                patches.append((o, P_RTCP))
        if sig:
            for o in sig:
                patches.append((o, P_SIGIP))
                patches.append((o + 4, P_SIGPORT))
    if x2:
        patches.append((find_all(suf, guid)[0] - 2, P_X2))   # 2 неопознанных байта перед conferenceID (4350 или ccc0)
    if codec:
        co = codec_offsets(suf)
        assert co and all(suf[o] == 0x20 for o in co), (name, co)    # шаблон — G.711A
        for o in co:
            patches.append((o, P_CODEC))
    patches.sort()
    # проверка: все патчи внутри суффикса, не пересекаются
    last = -1
    for o, k in patches:
        ln = {P_GUID: 16, P_IP: 4, P_RTP: 2, P_RTCP: 2, P_SIGIP: 4, P_SIGPORT: 2, P_X2: 2, P_CODEC: 1}[k]
        assert o >= last, (name, o, last)
        last = o + ln
        assert o + ln <= len(suf)
    return dict(name=name, vendor=bool(v), pre=pre, suf=suf, patches=patches)


def main():
    allf = {c: frames(c) for c in CAPS}
    pn4 = allf['aeeb35f5-h323_out_pn4.pcap']
    conns = collections.OrderedDict()
    for key, src, f in pn4:
        conns.setdefault(key, []).append((src, f))
    keys = list(conns)
    call1 = conns[keys[0]]            # дозвон, DTMF, отбой звонящим
    # --- GUID вызова: из SETUP (UUIE: 16 байт после 11 00 перед 'GUID' — берём по шаблону «06 b8 11 00»)
    setup_f = call1[0][1]
    mt, su = uuie(setup_f)
    assert mt == 5
    k = su.find(b'\x06\xb8\x11\x00')
    guid = su[k + 4:k + 20]
    sigip_in_suf = None
    cls = {}
    # SETUP: сигнальный адрес (ip4, порт 2) перед '11 00 GUID'
    v = split_vendor(su)
    suf_off = v[1]
    sig_off = [k - 4 - suf_off]      # ip4 в суффиксе (порт — +4)
    cls['setup'] = make_class('setup', su, guid, sig=sig_off, x2=True)
    by = {}
    for src, f in call1:
        mt, u = uuie(f)
        by.setdefault((src, mt), []).append((f, u))
    # callee = 6, caller = 193
    def one(src, mt, i=0):
        return by[(src, mt)][i]
    # --- одиночный кодек (дампы 2111-Al): шаблон G.711A, байт кодека подставляется
    a_f = [f for key, src, f in allf['2111-Al.pcap'] if q931(f)[0] == 5][0]
    _, au = uuie(a_f)
    ak = au.find(b'\x06\xb8\x11\x00')
    aguid = au[ak + 4:ak + 20]
    av = split_vendor(au)
    cls['setup1'] = make_class('setup1', au, aguid, sig=[ak - 4 - av[1]], x2=True, codec=True)
    cls['callproc'] = make_class('callproc', one(6, 2)[1], guid)
    a_al = [uuie(f)[1] for key, src, f in allf['2111-Al.pcap'] if q931(f)[0] == 1][0]
    cls['alerting'] = make_class('alerting', a_al, aguid, codec=True)
    cls['connect'] = make_class('connect', one(6, 7)[1], guid)
    cls['rc'] = make_class('rc', one(193, 0x5a)[1], guid)
    # STATUS (глобальный): свой GUID
    st = [f for (key, src, f) in allf['0c0bfc86-h323_out_b_o.pcap'] if q931(f)[0] == 0x7d][0]
    _, stu = uuie(st)
    cls['status'] = make_class('status', stu, stu[12:28])
    # --- FACILITY: общий префикс + GUID + 10 c0 01 80 LL + pdu
    facs = [(src, uuie(f)[1]) for src, f in call1 if q931(f)[0] == 0x62]
    fac_pre = facs[0][1][:17]
    for src, u in facs:
        assert u[:17] == fac_pre and u[17:33] == guid and u[33:37] == b'\x10\xc0\x01\x80', u.hex()
        assert u[37] == len(u) - 38
    pdus = [(src, u[38:]) for src, u in facs]
    # классификация PDU
    def pk(p):
        return p[2]
    kinds = collections.defaultdict(list)
    for src, p in pdus:
        kinds[(src, pk(p), len(p))].append(p)
    for kk, v2 in sorted(kinds.items()):
        print('PDU', kk, len(v2), v2[0].hex())
    tcs_callee = [p for s, p in pdus if s == 6 and pk(p) == 0x02][0]
    tcs_caller = [p for s, p in pdus if s == 193 and pk(p) == 0x02][0]
    msd = [p for s, p in pdus if pk(p) == 0x01][0]
    tcsack = [p for s, p in pdus if pk(p) == 0x21][0]
    msdack = [p for s, p in pdus if pk(p) == 0x20][0]
    endsess = [p for s, p in pdus if pk(p) == 0x4a][0]
    uii = [p for s, p in pdus if pk(p) == 0x6d][0]
    xs = [p for s, p in pdus if pk(p) not in (1, 2, 0x20, 0x21, 0x4a, 0x6d)]
    print('other pdus', [x.hex() for x in xs])
    x1 = xs[0]
    # TCS одиночного кодека (дамп 2111-Al, сторона 6): байт кодека 0x40 = G.711A, 0xc0 = G.711u
    al = [(src, uuie(f)[1]) for key, src, f in allf['2111-Al.pcap'] if q931(f)[0] == 0x62]
    tcs1 = [u[38:] for src, u in al if u[40] == 0x02 and src == 6][0]
    tcs1_off = tcs1.find(b'\x30\x40\x13\x80') + 1
    assert tcs1_off > 0 and tcs1.count(b'\x30\x40\x13\x80') == 1
    # --- вывод
    o = [LICENSE_HDR, '/* aa_tr_tpl.h — СГЕНЕРИРОВАНО tools/gen_tr.py из captures/ (pcap). Не править вручную. */\n',
         '#ifndef AA_TR_TPL_H\n#define AA_TR_TPL_H\n#include <stdint.h>\n#include <stddef.h>\n',
         '#define AA_TP_GUID 1\n#define AA_TP_IP 2\n#define AA_TP_RTP 3\n#define AA_TP_RTCP 4\n#define AA_TP_SIGIP 5\n#define AA_TP_SIGPORT 6\n#define AA_TP_X2 7\n#define AA_TP_CODEC 8\n',
         'typedef struct { uint16_t off; uint8_t kind; } aa_tp_patch_t;\n',
         'typedef struct { int vendor; const uint8_t *pre; size_t npre; const uint8_t *suf; size_t nsuf;\n'
         '                 const aa_tp_patch_t *p; size_t np; } aa_tp_uuie_t;\n']
    for n, c in cls.items():
        o.append(hexarr('tp_%s_pre' % n, c['pre']))
        o.append(hexarr('tp_%s_suf' % n, c['suf']))
        o.append('static const aa_tp_patch_t tp_%s_p[] = {%s};\n' % (n, ', '.join('{%d,%d}' % x for x in c['patches']) or '{0,0}'))
        o.append('static const aa_tp_uuie_t TP_%s = {%d, tp_%s_pre, %d, tp_%s_suf, %d, tp_%s_p, %d};\n' % (
            n.upper(), 1 if c['vendor'] else 0, n, len(c['pre']), n, len(c['suf']), n, len(c['patches'])))
    o.append(hexarr('tp_fac_pre', fac_pre))
    for n, b in (('tcs_dual', tcs_caller), ('msd', msd), ('tcsack', tcsack),
                 ('msdack', msdack), ('endsess', endsess), ('uii', uii), ('x1', x1), ('tcs1', tcs1)):
        o.append(hexarr('tp_pdu_' + n, b))
    o.append('#define TP_TCS1_CODEC_OFF %d\n' % tcs1_off)
    o.append('#endif\n')
    open(os.path.join(ROOT, 'aa_tr_tpl.h'), 'w', encoding='utf-8').write(''.join(o))
    # --- векторы
    v = [LICENSE_HDR, '/* СГЕНЕРИРОВАНО tools/gen_tr.py: все кадры H.225 из дампов */\n',
         'typedef struct { const char *cap; int src; size_t n; const uint8_t *d; } tr_vec_t;\n']
    items = []
    idx = 0
    for c in CAPS:
        for key, src, f in allf[c]:
            v.append(hexarr('v%d' % idx, f))
            items.append('{"%s", %d, %d, v%d}' % (c[:8], src, len(f), idx))
            idx += 1
    v.append('static const tr_vec_t TR_VECS[] = {\n  %s\n};\n#define TR_NVECS %d\n' % (',\n  '.join(items), idx))
    open(os.path.join(ROOT, 'tests', 'test_tr_vec.h'), 'w', encoding='utf-8').write(''.join(v))
    print('vectors', idx)
    for n, c in cls.items():
        print(n, 'pre', len(c['pre']), 'suf', len(c['suf']), c['patches'])


main()
