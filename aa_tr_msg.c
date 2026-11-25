/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * aa_tr_msg.c — разбор и сборка сообщений H.225/H.245 транка по шаблонам (см. aa_tr_msg.h).
 * Свободный пакет. Автор: Сенашенко Д.В., 2026.
 */
#include "aa_tr_msg.h"
#include "aa_tr_tpl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ */
/* codecs: AA_TR_CODEC_A | AA_TR_CODEC_U. SETUP с обоими кодеками — отдельный шаблон (ещё 2 OLC). */
static const aa_tp_uuie_t *tpl_for(aa_tr_type_t t, unsigned codecs)
{
    switch (t) {
    case AA_TR_SETUP: return codecs == (AA_TR_CODEC_A | AA_TR_CODEC_U) ? &TP_SETUP : &TP_SETUP1;
    case AA_TR_CALL_PROC: return &TP_CALLPROC;
    case AA_TR_ALERTING: return &TP_ALERTING;
    case AA_TR_CONNECT: return &TP_CONNECT;
    case AA_TR_RELEASE_COMPLETE: return &TP_RC;
    case AA_TR_STATUS: return &TP_STATUS;
    default: return NULL;
    }
}

static aa_tr_type_t type_for_q931(unsigned mt)
{
    switch (mt) {
    case 0x05: return AA_TR_SETUP;
    case 0x02: return AA_TR_CALL_PROC;
    case 0x01: return AA_TR_ALERTING;
    case 0x07: return AA_TR_CONNECT;
    case 0x5a: return AA_TR_RELEASE_COMPLETE;
    case 0x7d: return AA_TR_STATUS;
    case 0x62: return AA_TR_FACILITY;
    default: return AA_TR_UNKNOWN;
    }
}

static unsigned q931_for_type(aa_tr_type_t t)
{
    switch (t) {
    case AA_TR_SETUP: return 0x05;
    case AA_TR_CALL_PROC: return 0x02;
    case AA_TR_ALERTING: return 0x01;
    case AA_TR_CONNECT: return 0x07;
    case AA_TR_RELEASE_COMPLETE: return 0x5a;
    case AA_TR_STATUS: return 0x7d;
    case AA_TR_FACILITY: return 0x62;
    default: return 0;
    }
}

void aa_tr_msg_init(aa_tr_msg_t *m, aa_tr_type_t t)
{
    memset(m, 0, sizeof(*m));
    m->type = t;
    m->cause = -1;
}

void aa_tr_guid_random(uint8_t g[16])
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n = 0;
    if (f) { n = fread(g, 1, 16, f); fclose(f); }
    if (n != 16) for (int i = 0; i < 16; i++) g[i] = (uint8_t) rand();
}

/* ------------------------------------------------------------------------------------------------ */
int aa_tr_tpkt_len(const uint8_t *b, size_t n)
{
    if (n < 4) return 0;
    if (b[0] != 3 || b[1] != 0) return -1;
    unsigned l = ((unsigned) b[2] << 8) | b[3];
    if (l < 7) return -1;
    return (n >= l) ? (int) l : 0;
}

static void cpstr(char *dst, const uint8_t *s, size_t n)
{
    if (n >= AA_TR_STR) n = AA_TR_STR - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static const aa_tp_patch_t *first_patch(const aa_tp_uuie_t *t, int kind)
{
    for (size_t i = 0; i < t->np; i++) if (t->p[i].kind == kind) return &t->p[i];
    return NULL;
}

static int parse_h245(const uint8_t *u, size_t n, aa_tr_msg_t *m)
{
    size_t pre = sizeof(tp_fac_pre);
    if (n < pre + 16 + 5 || memcmp(u, tp_fac_pre, pre) != 0) return -1;
    memcpy(m->guid, u + pre, 16);
    const uint8_t *h = u + pre + 16;
    if (h[0] != 0x10 || h[1] != 0xc0 || h[2] != 0x01 || h[3] != 0x80) return -1;
    size_t pl = h[4];
    if (5 + pl != n - pre - 16 || pl > AA_TR_PDU_MAX || pl < 3) return -1;
    memcpy(m->pdu, h + 5, pl);
    m->pdu_n = pl;
    const uint8_t *p = m->pdu;
    switch (p[2]) {
    case 0x02: m->h245 = AA_H245_TCS;
        if (pl == sizeof(tp_pdu_tcs1)) m->codecs = p[TP_TCS1_CODEC_OFF] == 0xc0 ? AA_TR_CODEC_U : AA_TR_CODEC_A;
        else m->codecs = AA_TR_CODEC_A | AA_TR_CODEC_U;
        break;
    case 0x01: m->h245 = AA_H245_MSD; if (pl >= 9) m->msd_num = ((uint32_t) p[pl - 3] << 16) | ((uint32_t) p[pl - 2] << 8) | p[pl - 1]; break;
    case 0x21: m->h245 = AA_H245_TCSACK; break;
    case 0x20: m->h245 = AA_H245_MSDACK; m->msd_master = (pl >= 4 && p[3] == 0x80); break;
    case 0x4a: m->h245 = AA_H245_ENDSESSION; break;
    case 0x6d: m->h245 = AA_H245_UII;
        if (pl >= 7) m->uii = (char) ((((unsigned) p[5] << 8 | p[6]) >> 5) & 0x7f);
        break;
    default: m->h245 = AA_H245_OTHER; break;
    }
    return 0;
}

static int parse_uuie_t(const aa_tp_uuie_t *t, int dual, const uint8_t *u, size_t n, aa_tr_msg_t *m);

/*
 * Запасной разбор входящего SETUP, если UUIE не совпал с шаблоном байт-в-байт.
 * Разные релизы CM кодируют необязательные поля PER по-разному (например, CM R015x: лишний 0x00 перед conferenceID,
 * UUIE на 1 байт длиннее шаблона). Поэтому нужные поля ищутся по устойчивым якорям, а не по фиксированным смещениям:
 *   vendor:        00 4c 54 <len-1> product <len-1> version
 *   sourceCallSignalAddress: d5 1d 80 00 07 00 <ip4> <port2>; conferenceID - 16 байт перед "00 d5"
 *   fastStart OLC: 4c <cc> 13 80 12 15 00 01 00 <ip4> <rtp2> ; cc: 0x20 = G.711A, 0x60 = G.711u
 */
static int parse_setup_loose(const uint8_t *u, size_t n, aa_tr_msg_t *m)
{
    size_t p = 0, vend;
    for (; p + 4 < n; p++) if (u[p] == 0x00 && u[p + 1] == 0x4c && u[p + 2] == 0x54) break;
    if (p + 4 >= n) return -1;
    size_t lp = (size_t) u[p + 3] + 1;
    if (p + 4 + lp + 1 >= n) return -1;
    size_t lv = (size_t) u[p + 4 + lp] + 1;
    vend = p + 4 + lp + 1 + lv;
    if (vend > n) return -1;

    static const uint8_t anchor[6] = { 0xd5, 0x1d, 0x80, 0x00, 0x07, 0x00 };
    size_t a = 0;
    int found = 0;
    for (size_t i = vend + 17; i + sizeof(anchor) + 6 <= n; i++) {
        if (memcmp(u + i, anchor, sizeof(anchor)) == 0) { a = i; found = 1; break; }
    }
    if (!found) return -1;

    unsigned codecs = 0;
    int have_media = 0;
    uint8_t ip[4] = { 0 };
    unsigned port = 0;
    for (size_t i = a; i + 15 <= n; i++) {
        if (u[i] == 0x4c && (u[i + 1] == 0x20 || u[i + 1] == 0x60) && u[i + 2] == 0x13 && u[i + 3] == 0x80 && u[i + 4] == 0x12 &&
            u[i + 5] == 0x15 && u[i + 6] == 0x00 && u[i + 7] == 0x01 && u[i + 8] == 0x00) {
            codecs |= (u[i + 1] == 0x60) ? AA_TR_CODEC_U : AA_TR_CODEC_A;
            if (!have_media) {
                memcpy(ip, u + i + 9, 4);
                port = ((unsigned) u[i + 13] << 8) | u[i + 14];
                have_media = 1;
            }
        }
    }
    if (!have_media || !codecs) return -1;

    cpstr(m->product, u + p + 4, lp);
    cpstr(m->version, u + p + 4 + lp + 1, lv);
    memcpy(m->guid, u + a - 17, 16);
    memcpy(m->sig_ip, u + a + 6, 4);
    m->sig_port = ((unsigned) u[a + 10] << 8) | u[a + 11];
    m->has_media = 1;
    memcpy(m->media_ip, ip, 4);
    m->rtp_port = port;
    m->codecs = codecs;
    return 0;
}

static int parse_uuie(const uint8_t *u, size_t n, aa_tr_msg_t *m)
{
    if (m->type == AA_TR_FACILITY) return parse_h245(u, n, m);
    const aa_tp_uuie_t *cands[2] = { tpl_for(m->type, AA_TR_CODEC_A | AA_TR_CODEC_U), tpl_for(m->type, AA_TR_CODEC_A) };
    for (int ci = 0; ci < 2; ci++) {
        if (!cands[ci] || (ci == 1 && cands[1] == cands[0])) continue;
        if (parse_uuie_t(cands[ci], ci == 0, u, n, m) == 0) return 0;
    }
    if (m->type == AA_TR_SETUP) return parse_setup_loose(u, n, m);
    return -1;
}

static int parse_uuie_t(const aa_tp_uuie_t *t, int dual, const uint8_t *u, size_t n, aa_tr_msg_t *m)
{
    size_t vend = 0;
    if (t->vendor) {
        size_t p = 0;
        for (; p + 4 < n; p++) if (u[p] == 0x00 && u[p + 1] == 0x4c && u[p + 2] == 0x54) break;
        if (p + 4 >= n || p + 3 != t->npre || memcmp(u, t->pre, t->npre) != 0) return -1;
        size_t lp = (size_t) u[p + 3] + 1;
        if (p + 4 + lp + 1 >= n) return -1;
        size_t lv = (size_t) u[p + 4 + lp] + 1;
        vend = p + 4 + lp + 1 + lv;
        if (vend > n) return -1;
        cpstr(m->product, u + p + 4, lp);
        cpstr(m->version, u + p + 4 + lp + 1, lv);
    }
    if (n - vend != t->nsuf) return -1;
    const uint8_t *s = u + vend;
    const aa_tp_patch_t *g = first_patch(t, AA_TP_GUID);
    if (g) memcpy(m->guid, s + g->off, 16);
    const aa_tp_patch_t *ip = first_patch(t, AA_TP_IP), *rtp = first_patch(t, AA_TP_RTP);
    if (ip && rtp) {
        m->has_media = 1;
        memcpy(m->media_ip, s + ip->off, 4);
        m->rtp_port = ((unsigned) s[rtp->off] << 8) | s[rtp->off + 1];
    }
    const aa_tp_patch_t *cd = first_patch(t, AA_TP_CODEC);
    if (cd) m->codecs = s[cd->off] == 0x60 ? AA_TR_CODEC_U : AA_TR_CODEC_A;
    else if (m->type == AA_TR_SETUP && dual) m->codecs = AA_TR_CODEC_A | AA_TR_CODEC_U;
    const aa_tp_patch_t *x2 = first_patch(t, AA_TP_X2);
    if (x2) { m->x2_set = 1; memcpy(m->x2, s + x2->off, 2); }
    const aa_tp_patch_t *si = first_patch(t, AA_TP_SIGIP), *sp = first_patch(t, AA_TP_SIGPORT);
    if (si && sp) {
        memcpy(m->sig_ip, s + si->off, 4);
        m->sig_port = ((unsigned) s[sp->off] << 8) | s[sp->off + 1];
    }
    return 0;
}

int aa_tr_parse(const uint8_t *f, size_t n, aa_tr_msg_t *m)
{
    aa_tr_msg_init(m, AA_TR_UNKNOWN);
    int tl = aa_tr_tpkt_len(f, n);
    if (tl <= 0 || (size_t) tl != n) return -1;
    const uint8_t *q = f + 4;
    size_t qn = n - 4;
    if (qn < 5 || q[0] != 0x08 || q[1] != 2) return -1;
    m->dest = (q[2] & 0x80) != 0;
    m->callref = (((unsigned) q[2] & 0x7f) << 8) | q[3];
    m->type = type_for_q931(q[4]);
    size_t p = 5;
    const uint8_t *uu = NULL;
    size_t uun = 0;
    while (p < qn) {
        unsigned ie = q[p];
        if (ie & 0x80) {
            if (ie == 0xa1) m->has_sc = 1;
            p++;
            continue;
        }
        size_t l, hl;
        if (ie == 0x7e) { if (p + 3 > qn) return -1; l = ((size_t) q[p + 1] << 8) | q[p + 2]; hl = 3; }
        else { if (p + 2 > qn) return -1; l = q[p + 1]; hl = 2; }
        if (p + hl + l > qn) return -1;
        const uint8_t *v = q + p + hl;
        switch (ie) {
        case 0x04: if (l == 3) { m->has_bearer = 1; memcpy(m->bearer, v, 3); } break;
        case 0x08: if (l >= 2) m->cause = v[1] & 0x7f; break;
        case 0x14: if (l >= 1) { m->has_callstate = 1; m->callstate = v[0]; } break;
        case 0x1e: if (l >= 2) { m->has_prog = 1; m->prog = v[1]; } break;
        case 0x6c: if (l >= 2) { m->has_calling = 1; m->calling_o3 = v[0]; m->calling_o3a = v[1]; cpstr(m->calling, v + 2, l - 2); } break;
        case 0x70: if (l >= 1) { m->has_called = 1; m->called_o3 = v[0]; cpstr(m->called, v + 1, l - 1); } break;
        case 0x28: m->has_display = 1; cpstr(m->display, v, l); break;
        case 0x7e: if (l >= 1 && v[0] == 5) { uu = v + 1; uun = l - 1; } break;
        default: break;
        }
        p += hl + l;
    }
    if (m->type == AA_TR_UNKNOWN) return -1;
    if (!uu) return -1;
    return parse_uuie(uu, uun, m);
}

/* ------------------------------------------------------------------------------------------------ */
#define NEED(k) do { if (o + (k) > cap) return -1; } while (0)

static int build_uuie(uint8_t *out, size_t cap, const aa_tr_msg_t *m)
{
    size_t o = 0;
    if (m->type == AA_TR_FACILITY) {
        size_t pre = sizeof(tp_fac_pre);
        if (m->pdu_n < 3 || m->pdu_n > AA_TR_PDU_MAX || cap < pre + 16 + 5 + m->pdu_n) return -1;
        memcpy(out, tp_fac_pre, pre); o = pre;
        memcpy(out + o, m->guid, 16); o += 16;
        out[o++] = 0x10; out[o++] = 0xc0; out[o++] = 0x01; out[o++] = 0x80;
        out[o++] = (uint8_t) m->pdu_n;
        memcpy(out + o, m->pdu, m->pdu_n);
        return (int) (o + m->pdu_n);
    }
    const aa_tp_uuie_t *t = tpl_for(m->type, m->codecs);
    if (!t) return -1;
    if (t->vendor) {
        size_t lp = strlen(m->product), lv = strlen(m->version);
        if (lp < 1 || lv < 1 || lp > 200 || lv > 200) return -1;
        NEED(t->npre + 2 + lp + lv);
        memcpy(out, t->pre, t->npre); o = t->npre;
        out[o++] = (uint8_t) (lp - 1); memcpy(out + o, m->product, lp); o += lp;
        out[o++] = (uint8_t) (lv - 1); memcpy(out + o, m->version, lv); o += lv;
    }
    NEED(t->nsuf);
    uint8_t *s = out + o;
    memcpy(s, t->suf, t->nsuf);
    for (size_t i = 0; i < t->np; i++) {
        unsigned off = t->p[i].off;
        switch (t->p[i].kind) {
        case AA_TP_GUID: memcpy(s + off, m->guid, 16); break;
        case AA_TP_IP: memcpy(s + off, m->media_ip, 4); break;
        case AA_TP_RTP: s[off] = (uint8_t) (m->rtp_port >> 8); s[off + 1] = (uint8_t) m->rtp_port; break;
        case AA_TP_RTCP: s[off] = (uint8_t) ((m->rtp_port + 1) >> 8); s[off + 1] = (uint8_t) (m->rtp_port + 1); break;
        case AA_TP_CODEC: s[off] = (m->codecs == AA_TR_CODEC_U) ? 0x60 : 0x20; break;
        case AA_TP_X2: if (m->x2_set) memcpy(s + off, m->x2, 2); break;
        case AA_TP_SIGIP: memcpy(s + off, m->sig_ip, 4); break;
        case AA_TP_SIGPORT: s[off] = (uint8_t) (m->sig_port >> 8); s[off + 1] = (uint8_t) m->sig_port; break;
        }
    }
    return (int) (o + t->nsuf);
}

int aa_tr_build(uint8_t *out, size_t cap, const aa_tr_msg_t *m)
{
    size_t o = 0;
    unsigned mt = q931_for_type(m->type);
    if (!mt || cap < 16) return -1;
    out[0] = 3; out[1] = 0;                       /* длину допишем в конце */
    o = 4;
    out[o++] = 0x08; out[o++] = 2;
    out[o++] = (uint8_t) (((m->callref >> 8) & 0x7f) | (m->dest ? 0x80 : 0));
    out[o++] = (uint8_t) m->callref;
    out[o++] = (uint8_t) mt;
    if (m->has_sc) { NEED(1); out[o++] = 0xa1; }
    if (m->has_bearer) { NEED(5); out[o++] = 0x04; out[o++] = 3; memcpy(out + o, m->bearer, 3); o += 3; }
    if (m->cause >= 0) { NEED(4); out[o++] = 0x08; out[o++] = 2; out[o++] = 0x81; out[o++] = (uint8_t) (0x80 | m->cause); }
    if (m->has_callstate) { NEED(3); out[o++] = 0x14; out[o++] = 1; out[o++] = m->callstate; }
    if (m->has_prog) { NEED(4); out[o++] = 0x1e; out[o++] = 2; out[o++] = 0x81; out[o++] = m->prog; }
    if (m->has_calling) {
        size_t l = strlen(m->calling);
        NEED(4 + l); out[o++] = 0x6c; out[o++] = (uint8_t) (2 + l); out[o++] = m->calling_o3; out[o++] = m->calling_o3a;
        memcpy(out + o, m->calling, l); o += l;
    }
    if (m->has_called) {
        size_t l = strlen(m->called);
        NEED(3 + l); out[o++] = 0x70; out[o++] = (uint8_t) (1 + l); out[o++] = m->called_o3;
        memcpy(out + o, m->called, l); o += l;
    }
    NEED(5);
    size_t lenpos = o + 1;
    out[o++] = 0x7e; o += 2; out[o++] = 5;
    int un = build_uuie(out + o, cap - o, m);
    if (un < 0) return -1;
    o += (size_t) un;
    size_t ul = (size_t) un + 1;
    out[lenpos] = (uint8_t) (ul >> 8); out[lenpos + 1] = (uint8_t) ul;
    if (m->has_display) {
        size_t l = strlen(m->display);
        NEED(4 + l); out[o++] = 0x96; out[o++] = 0x28; out[o++] = (uint8_t) l;
        memcpy(out + o, m->display, l); o += l;
    }
    out[2] = (uint8_t) (o >> 8); out[3] = (uint8_t) o;
    return (int) o;
}

/* ------------------------------------------------------------------------------------------------ */
size_t aa_tr_pdu_tcs(uint8_t *o, unsigned codecs)
{
    if (codecs == AA_TR_CODEC_A || codecs == AA_TR_CODEC_U) {       /* один кодек: шаблон G.711A, байт кодека 0x40/0xc0 */
        memcpy(o, tp_pdu_tcs1, sizeof(tp_pdu_tcs1));
        o[TP_TCS1_CODEC_OFF] = (codecs == AA_TR_CODEC_U) ? 0xc0 : 0x40;
        return sizeof(tp_pdu_tcs1);
    }
    memcpy(o, tp_pdu_tcs_dual, sizeof(tp_pdu_tcs_dual));            /* оба кодека (A, затем u) */
    return sizeof(tp_pdu_tcs_dual);
}

size_t aa_tr_pdu_msd(uint8_t *o, uint32_t num)
{
    memcpy(o, tp_pdu_msd, sizeof(tp_pdu_msd));
    size_t n = sizeof(tp_pdu_msd);
    o[n - 3] = (uint8_t) (num >> 16); o[n - 2] = (uint8_t) (num >> 8); o[n - 1] = (uint8_t) num;
    return n;
}

size_t aa_tr_pdu_msdack(uint8_t *o, int we_are_master)
{
    memcpy(o, tp_pdu_msdack, sizeof(tp_pdu_msdack));
    o[3] = we_are_master ? 0x80 : 0xa0;
    return sizeof(tp_pdu_msdack);
}

size_t aa_tr_pdu_tcsack(uint8_t *o) { memcpy(o, tp_pdu_tcsack, sizeof(tp_pdu_tcsack)); return sizeof(tp_pdu_tcsack); }
size_t aa_tr_pdu_endsession(uint8_t *o) { memcpy(o, tp_pdu_endsess, sizeof(tp_pdu_endsess)); return sizeof(tp_pdu_endsess); }
size_t aa_tr_pdu_caller_x1(uint8_t *o) { memcpy(o, tp_pdu_x1, sizeof(tp_pdu_x1)); return sizeof(tp_pdu_x1); }

size_t aa_tr_pdu_uii(uint8_t *o, char c)
{
    memcpy(o, tp_pdu_uii, sizeof(tp_pdu_uii));
    unsigned v = 0x2000u | (((unsigned) c & 0x7f) << 5);
    o[5] = (uint8_t) (v >> 8); o[6] = (uint8_t) v;
    return sizeof(tp_pdu_uii);
}

int aa_tr_msd_we_are_master(uint32_t ours, uint32_t theirs)
{
    uint32_t d = (ours - theirs) & 0xffffffu;
    return d > 0 && d < 0x800000u;
}
