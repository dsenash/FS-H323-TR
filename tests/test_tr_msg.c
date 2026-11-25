/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* test_tr_msg.c — каждое сообщение каждого дампа: parse -> build должен дать тот же кадр; H.245 PDU — теми же сборщиками. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "aa_tr_msg.h"
#include "test_tr_vec.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 30) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

typedef struct { char cap[9]; unsigned cr; uint32_t num[2]; int have[2]; } msdrec_t;
static msdrec_t recs[64]; static int nrec;
static msdrec_t *rec(const char *cap, unsigned cr)
{
    for (int i = 0; i < nrec; i++) if (!strcmp(recs[i].cap, cap) && recs[i].cr == cr) return &recs[i];
    msdrec_t *r = &recs[nrec++]; memset(r, 0, sizeof(*r)); strncpy(r->cap, cap, 8); r->cr = cr; return r;
}

int main(void)
{
    int nparsed = 0, nfac = 0, nmaster = 0;
    int kinds[AA_TR_FACILITY + 1] = {0};
    for (int i = 0; i < TR_NVECS; i++) {
        const tr_vec_t *v = &TR_VECS[i];
        aa_tr_msg_t m;
        int r = aa_tr_parse(v->d, v->n, &m);
        CHECK(r == 0, "vec %d (%s) parse failed type=%d len=%zu", i, v->cap, (int) m.type, v->n);
        if (r) continue;
        nparsed++; kinds[m.type]++;
        uint8_t out[1024];
        int bn = aa_tr_build(out, sizeof(out), &m);
        CHECK(bn == (int) v->n && !memcmp(out, v->d, v->n), "vec %d (%s) rebuild mismatch type=%d n=%d/%zu", i, v->cap, (int) m.type, bn, v->n);
        if (m.type == AA_TR_FACILITY) {
            nfac++;
            uint8_t p[AA_TR_PDU_MAX]; size_t pn = 0;
            switch (m.h245) {
            case AA_H245_MSD: pn = aa_tr_pdu_msd(p, m.msd_num); {
                msdrec_t *rc = rec(v->cap, m.callref); rc->num[m.dest] = m.msd_num; rc->have[m.dest] = 1; } break;
            case AA_H245_MSDACK: pn = aa_tr_pdu_msdack(p, m.msd_master); {
                msdrec_t *rc = rec(v->cap, m.callref);
                if (rc->have[0] && rc->have[1]) {
                    uint32_t ours = rc->num[m.dest], theirs = rc->num[!m.dest];
                    CHECK(aa_tr_msd_we_are_master(ours, theirs) == m.msd_master, "vec %d MSD rule ours=%u theirs=%u ack_master=%d", i, ours, theirs, m.msd_master);
                    nmaster++;
                } } break;
            case AA_H245_TCSACK: pn = aa_tr_pdu_tcsack(p); break;
            case AA_H245_ENDSESSION: pn = aa_tr_pdu_endsession(p); break;
            case AA_H245_UII: pn = aa_tr_pdu_uii(p, m.uii); break;
            case AA_H245_TCS: {
                pn = aa_tr_pdu_tcs(p, m.codecs); } break;
            case AA_H245_OTHER: pn = aa_tr_pdu_caller_x1(p); break;
            default: break;
            }
            CHECK(pn == m.pdu_n && !memcmp(p, m.pdu, pn), "vec %d H.245 kind %d pdu builder mismatch", i, (int) m.h245);
        }
    }
    /* UII: все символы DTMF кодируются обратимо */
    for (const char *c = "0123456789#*ABCD"; *c; c++) {
        uint8_t p[AA_TR_PDU_MAX]; aa_tr_msg_t m; aa_tr_msg_init(&m, AA_TR_FACILITY);
        aa_tr_guid_random(m.guid); m.callref = 5; m.pdu_n = aa_tr_pdu_uii(m.pdu, *c); (void) p;
        uint8_t out[256]; int n = aa_tr_build(out, sizeof(out), &m); aa_tr_msg_t m2;
        CHECK(n > 0 && aa_tr_parse(out, (size_t) n, &m2) == 0 && m2.h245 == AA_H245_UII && m2.uii == *c, "uii '%c'", *c);
    }
    /* product/version произвольной длины и номера: сборка -> разбор */
    {
        aa_tr_msg_t m, m2; uint8_t out[1024];
        aa_tr_msg_init(&m, AA_TR_SETUP); m.callref = 0x1234; aa_tr_guid_random(m.guid);
        m.has_sc = 1; m.has_bearer = 1; m.bearer[0] = 0x90; m.bearer[1] = 0x90; m.bearer[2] = 0xa3;
        m.has_prog = 1; m.prog = 0x83; m.has_calling = 1; m.calling_o3 = 0; m.calling_o3a = 0x80; strcpy(m.calling, "1234567");
        m.has_called = 1; m.called_o3 = 0x81; strcpy(m.called, "89001112233"); m.has_display = 1; strcpy(m.display, "FreeSWITCH");
        strcpy(m.product, "FS trunk"); strcpy(m.version, "1.0");
        m.has_media = 1; m.media_ip[0] = 10; m.media_ip[3] = 7; m.rtp_port = 16384; m.sig_ip[0] = 10; m.sig_ip[3] = 7; m.sig_port = 1722;
        int n = aa_tr_build(out, sizeof(out), &m);
        CHECK(n > 0 && aa_tr_parse(out, (size_t) n, &m2) == 0, "custom setup parse");
        CHECK(!strcmp(m2.calling, "1234567") && !strcmp(m2.called, "89001112233") && !strcmp(m2.product, "FS trunk") &&
              !strcmp(m2.version, "1.0") && m2.rtp_port == 16384 && m2.media_ip[3] == 7 && m2.sig_port == 1722 &&
              !memcmp(m2.guid, m.guid, 16) && m2.callref == 0x1234 && !strcmp(m2.display, "FreeSWITCH"), "custom setup fields");
    }
    /* кодеки: SETUP/ALERTING/TCS для A, U и A|U собираются и читаются обратно */
    for (unsigned cd = 1; cd <= 3; cd++) {
        aa_tr_msg_t m, m2; uint8_t out[1024];
        aa_tr_msg_init(&m, AA_TR_SETUP); m.callref = 7; aa_tr_guid_random(m.guid); m.codecs = cd;
        strcpy(m.product, "P"); strcpy(m.version, "V"); m.has_media = 1; m.media_ip[3] = 9; m.rtp_port = 20000;
        int n = aa_tr_build(out, sizeof(out), &m);
        CHECK(n > 0 && aa_tr_parse(out, (size_t) n, &m2) == 0 && m2.codecs == cd && m2.rtp_port == 20000, "setup codecs %u", cd);
        if (cd < 3) {
            aa_tr_msg_init(&m, AA_TR_ALERTING); m.callref = 7; aa_tr_guid_random(m.guid); m.codecs = cd;
            strcpy(m.product, "P"); strcpy(m.version, "V"); m.has_media = 1; m.media_ip[3] = 9; m.rtp_port = 20000;
            n = aa_tr_build(out, sizeof(out), &m);
            CHECK(n > 0 && aa_tr_parse(out, (size_t) n, &m2) == 0 && m2.codecs == cd, "alerting codec %u", cd);
        }
        aa_tr_msg_init(&m, AA_TR_FACILITY); m.callref = 7; aa_tr_guid_random(m.guid); m.pdu_n = aa_tr_pdu_tcs(m.pdu, cd);
        n = aa_tr_build(out, sizeof(out), &m);
        CHECK(n > 0 && aa_tr_parse(out, (size_t) n, &m2) == 0 && m2.h245 == AA_H245_TCS && m2.codecs == cd, "tcs codecs %u", cd);
    }
    /* Реальный SETUP от CM R015x.02.1.016.4 (10.111.1.30 -> FS, дамп 25.11): UUIE на 1 байт длиннее шаблона (лишний 0x00
       перед conferenceID). Строгий разбор по шаблону его отвергал (отбой cause 100), запасной разбор по якорям - принимает. */
    {
        static const uint8_t cm15[] = {
            0x03, 0x00, 0x01, 0x29, 0x08, 0x02, 0x75, 0xe8, 0x05, 0xa1, 0x04, 0x03, 0x90, 0x90, 0xa3, 0x1e,
            0x02, 0x81, 0x83, 0x6c, 0x06, 0x21, 0x80, 0x31, 0x30, 0x32, 0x37, 0x70, 0x05, 0x81, 0x31, 0x30,
            0x30, 0x31, 0x7e, 0x00, 0xf5, 0x05, 0x20, 0x90, 0x06, 0x00, 0x08, 0x91, 0x4a, 0x00, 0x05, 0x3c,
            0xc0, 0xb5, 0x00, 0x4c, 0x54, 0x11, 0x41, 0x76, 0x61, 0x79, 0x61, 0x20, 0x4d, 0x75, 0x6c, 0x74,
            0x69, 0x76, 0x61, 0x6e, 0x74, 0x61, 0x67, 0x65, 0x0f, 0x52, 0x30, 0x31, 0x35, 0x78, 0x2e, 0x30,
            0x32, 0x2e, 0x31, 0x2e, 0x30, 0x31, 0x36, 0x2e, 0x34, 0x10, 0x03, 0x14, 0x05, 0x04, 0x01, 0x00,
            0x00, 0xc0, 0x2c, 0x05, 0x04, 0x01, 0x00, 0x00, 0xc0, 0x3c, 0x05, 0x04, 0x01, 0x00, 0x00, 0xc0,
            0x20, 0x01, 0x01, 0x80, 0x43, 0x34, 0x00, 0x80, 0xe6, 0x4b, 0x76, 0x7f, 0xe9, 0xf1, 0x01, 0x73,
            0x05, 0x69, 0xed, 0x60, 0x1c, 0x00, 0x00, 0x00, 0xd5, 0x1d, 0x80, 0x00, 0x07, 0x00, 0x0a, 0x6f,
            0x01, 0x1e, 0x06, 0xb8, 0x11, 0x00, 0x80, 0xe6, 0x4b, 0x76, 0x7f, 0xe9, 0xf1, 0x01, 0x73, 0x05,
            0x69, 0xed, 0x60, 0x1c, 0x00, 0x00, 0x67, 0x04, 0x13, 0x00, 0x00, 0x64, 0x0c, 0x20, 0x13, 0x80,
            0x0b, 0x0f, 0x00, 0x01, 0x00, 0x0a, 0x6f, 0x01, 0x0a, 0xe9, 0x8d, 0x00, 0x1e, 0x40, 0x00, 0x64,
            0x06, 0x04, 0x01, 0x00, 0x4c, 0x20, 0x13, 0x80, 0x12, 0x15, 0x00, 0x01, 0x00, 0x0a, 0x6f, 0x01,
            0x0a, 0xe9, 0x8c, 0x00, 0x0a, 0x6f, 0x01, 0x0a, 0xe9, 0x8d, 0x00, 0x13, 0x00, 0x00, 0x64, 0x0c,
            0x60, 0x13, 0x80, 0x0b, 0x0f, 0x00, 0x01, 0x00, 0x0a, 0x6f, 0x01, 0x0a, 0xe9, 0x8d, 0x00, 0x1e,
            0x40, 0x00, 0x64, 0x06, 0x04, 0x01, 0x00, 0x4c, 0x60, 0x13, 0x80, 0x12, 0x15, 0x00, 0x01, 0x00,
            0x0a, 0x6f, 0x01, 0x0a, 0xe9, 0x8c, 0x00, 0x0a, 0x6f, 0x01, 0x0a, 0xe9, 0x8d, 0x00, 0x01, 0x00,
            0x01, 0x80, 0x01, 0x00, 0x01, 0x00, 0x10, 0x80, 0x01, 0x80, 0x96, 0x28, 0x0c, 0x42, 0x72, 0x79,
            0x6b, 0x73, 0x69, 0x6e, 0x2c, 0x20, 0x41, 0x20, 0x53,
        };
        aa_tr_msg_t m15;
        memset(&m15, 0, sizeof(m15));
        CHECK(aa_tr_parse(cm15, sizeof(cm15), &m15) == 0 && m15.type == AA_TR_SETUP, "CM R015x SETUP parse");
        CHECK(!strcmp(m15.called, "1001") && !strcmp(m15.calling, "1027") && !strcmp(m15.display, "Bryksin, A S"), "CM R015x numbers %s/%s/%s", m15.called, m15.calling, m15.display);
        CHECK(!strcmp(m15.product, "Avaya Multivantage") && !strcmp(m15.version, "R015x.02.1.016.4"), "CM R015x vendor %s / %s", m15.product, m15.version);
        CHECK(m15.has_media && m15.media_ip[0] == 10 && m15.media_ip[1] == 111 && m15.media_ip[2] == 1 && m15.media_ip[3] == 10 && m15.rtp_port == 59788,
              "CM R015x media %u.%u.%u.%u:%u", m15.media_ip[0], m15.media_ip[1], m15.media_ip[2], m15.media_ip[3], m15.rtp_port);
        CHECK(m15.codecs == (AA_TR_CODEC_A | AA_TR_CODEC_U), "CM R015x codecs %u", m15.codecs);
        CHECK(m15.sig_ip[0] == 10 && m15.sig_ip[3] == 30 && m15.sig_port == 1720, "CM R015x sig %u.%u.%u.%u:%u", m15.sig_ip[0], m15.sig_ip[1], m15.sig_ip[2], m15.sig_ip[3], m15.sig_port);
        CHECK(m15.guid[0] == 0x80 && m15.guid[1] == 0xe6 && m15.guid[14] == 0 && m15.guid[15] == 0, "CM R015x guid %02x%02x", m15.guid[0], m15.guid[1]);
    }
    printf("vectors %d parsed %d (setup %d proc %d alert %d conn %d rc %d status %d fac %d) MSD-rule checks %d\n", TR_NVECS, nparsed,
           kinds[AA_TR_SETUP], kinds[AA_TR_CALL_PROC], kinds[AA_TR_ALERTING], kinds[AA_TR_CONNECT], kinds[AA_TR_RELEASE_COMPLETE],
           kinds[AA_TR_STATUS], kinds[AA_TR_FACILITY], nmaster);
    printf("test_tr_msg: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
