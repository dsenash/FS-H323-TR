/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* Тест движка транка: два движка друг с другом по loopback + «живой» SETUP из дампа 2111-Al.pcap. */
#define _GNU_SOURCE
#include "aa_tr_engine.h"
#include "test_tr_vec.h"
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- хозяин-заглушка ---------- */
#define MAXEV 256
typedef struct {
    const char *name;
    pthread_mutex_t mu;
    char ev[MAXEV][160];
    int n, mark;
    int accept_rc;                  /* что вернуть на входящий */
    int rtp_port;
    char last_called[64], last_calling[64], last_name[64], last_id[AA_TR_ID_LEN];
    char media_ip[48]; int media_port; unsigned codec;
} host_t;

static void hpush(host_t *h, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&h->mu);
    if (h->n < MAXEV) {
        va_start(ap, fmt);
        vsnprintf(h->ev[h->n++], sizeof(h->ev[0]), fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&h->mu);
}
static void h_log(void *c, int l, const char *m) { (void) c; if (l >= 30) printf("   [log %d] %s\n", l, m); }
static int h_in(void *c, const aa_tr_incoming_t *in, aa_tr_accept_t *out)
{
    host_t *h = c;
    pthread_mutex_lock(&h->mu);
    snprintf(h->last_called, sizeof(h->last_called), "%s", in->called);
    snprintf(h->last_calling, sizeof(h->last_calling), "%s", in->calling);
    snprintf(h->last_name, sizeof(h->last_name), "%s", in->calling_name);
    snprintf(h->last_id, sizeof(h->last_id), "%s", in->call_id);
    snprintf(h->media_ip, sizeof(h->media_ip), "%s", in->media_ip);
    h->media_port = in->media_port; h->codec = in->codec;
    pthread_mutex_unlock(&h->mu);
    hpush(h, "IN %s>%s raw=%s codec=%u", in->calling, in->called, in->called_raw, in->codec);
    if (h->accept_rc) return h->accept_rc;
    snprintf(out->rtp_ip, sizeof(out->rtp_ip), "127.0.0.1");
    out->rtp_port = h->rtp_port;
    return 0;
}
static void h_ring(void *c, const char *id) { (void) id; hpush(c, "RING"); }
static void h_media(void *c, const char *id, const char *ip, int port, unsigned cd) { (void) id; hpush(c, "MEDIA %s:%d c=%u", ip, port, cd); }
static void h_ans(void *c, const char *id) { (void) id; hpush(c, "ANS"); }
static void h_hup(void *c, const char *id, int cause) { (void) id; hpush(c, "HUP %d", cause); }
static void h_dtmf(void *c, const char *id, char d) { (void) id; hpush(c, "DTMF %c", d); }

static aa_tr_host_t mkhost(host_t *h) { aa_tr_host_t t = { h, h_log, h_in, h_ring, h_media, h_ans, h_hup, h_dtmf }; return t; }

static int wait_ev(host_t *h, const char *prefix, int ms)
{
    for (int t = 0; t < ms; t += 5) {
        pthread_mutex_lock(&h->mu);
        for (int i = h->mark; i < h->n; i++)
            if (!strncmp(h->ev[i], prefix, strlen(prefix))) { h->mark = i + 1; pthread_mutex_unlock(&h->mu); return 1; }
        pthread_mutex_unlock(&h->mu);
        usleep(5000);
    }
    printf("   [%s] нет события '%s'; журнал:", h->name, prefix);
    for (int i = 0; i < h->n; i++) printf(" | %s", h->ev[i]);
    printf("\n");
    return 0;
}

/* ---------- конфиги ---------- */
static void mkcfg(aa_tr_config_t *c, const char *tname, int peer_port, const char *codec, int lines, const char *rule_match, int strip, const char *add)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->local_ip, sizeof(c->local_ip), "127.0.0.1");
    c->h225_port = 0;
    snprintf(c->product, sizeof(c->product), "Avaya Multivantage");
    snprintf(c->version, sizeof(c->version), "Adapter_3.5.8");
    snprintf(c->display, sizeof(c->display), "FreeSWITCH");
    c->ntrunk = 1;
    aa_tr_trunk_t *t = &c->trunk[0];
    snprintf(t->name, sizeof(t->name), "%s", tname);
    snprintf(t->ip, sizeof(t->ip), "127.0.0.1");
    t->port = peer_port;
    snprintf(t->codec, sizeof(t->codec), "%s", codec);
    t->lines = lines;
    if (rule_match) {
        t->nrules = 1;
        snprintf(t->rules[0].match, sizeof(t->rules[0].match), "%s", rule_match);
        t->rules[0].strip = strip;
        snprintf(t->rules[0].add, sizeof(t->rules[0].add), "%s", add);
    }
}

/* ---------- сырой клиент ---------- */
static int raw_connect(int port)
{
    struct sockaddr_in a = { 0 };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t) port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *) &a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}
/* читает следующее сообщение; один кадр на вызов (тест шлёт ответы по одному — кадры не склеиваются, т.к. читаем точно по TPKT) */
static uint8_t g_pend[8192]; static size_t g_pn;
static int raw_next(int fd, aa_tr_msg_t *m, int ms)
{
    uint8_t fr[2048];
    for (;;) {
        int tl = aa_tr_tpkt_len(g_pend, g_pn);
        if (tl > 0) {
            memcpy(fr, g_pend, (size_t) tl);
            memmove(g_pend, g_pend + tl, g_pn - (size_t) tl); g_pn -= (size_t) tl;
            return aa_tr_parse(fr, (size_t) tl, m) == 0 ? 1 : -2;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0) return 0;
        ssize_t r = recv(fd, g_pend + g_pn, sizeof(g_pend) - g_pn, 0);
        if (r <= 0) return -1;
        g_pn += (size_t) r;
    }
}

static const char *K(const aa_tr_msg_t *m)
{
    static char b[40];
    switch (m->type) {
    case AA_TR_SETUP: return "SETUP"; case AA_TR_CALL_PROC: return "PROC"; case AA_TR_ALERTING: return "ALERT";
    case AA_TR_CONNECT: return "CONN"; case AA_TR_RELEASE_COMPLETE: return "RC"; case AA_TR_STATUS: return "STATUS";
    case AA_TR_FACILITY:
        switch (m->h245) {
        case AA_H245_TCS: return "TCS"; case AA_H245_MSD: return "MSD"; case AA_H245_TCSACK: return "TCSack";
        case AA_H245_MSDACK: return "MSDack"; case AA_H245_ENDSESSION: return "END"; case AA_H245_UII: snprintf(b, sizeof(b), "UII"); return b;
        default: return "H245?";
        }
    default: return "?";
    }
}

static const uint8_t *vec(const char *cap, int src, int nth, size_t *n)
{
    int k = 0;
    for (int i = 0; i < TR_NVECS; i++) {
        if (strncmp(TR_VECS[i].cap, cap, 8) || TR_VECS[i].src != src) continue;
        aa_tr_msg_t m;
        if (aa_tr_parse(TR_VECS[i].d, TR_VECS[i].n, &m) == 0 && m.type == AA_TR_SETUP && nth >= 0) { if (k++ == nth) { *n = TR_VECS[i].n; return TR_VECS[i].d; } }
    }
    return NULL;
}

/* ---------- сценарии ---------- */
int main(void)
{
    host_t hx = { .name = "X", .rtp_port = 20000 }, hy = { .name = "Y", .rtp_port = 30000 };
    pthread_mutex_init(&hx.mu, NULL); pthread_mutex_init(&hy.mu, NULL);
    aa_tr_config_t cx, cy;
    char err[200];

    /* Y слушает, правило 111xxx -> 0xxx; X звонит на Y */
    mkcfg(&cy, "x", 1, "PCMA", 100, "111", 3, "0");
    aa_tr_host_t hty = mkhost(&hy);
    aa_tr_engine_t *ey = aa_tr_engine_new(&cy, &hty);
    CHECK(aa_tr_engine_start(ey, err, sizeof(err)) == 0, "Y start: %s", err);
    int yport = aa_tr_engine_port(ey);
    mkcfg(&cx, "y", yport, "PCMA", 100, NULL, 0, "");
    /* out_number_rules X: 9111xxx -> 111xxx (префикс выхода 9 убирается на исходящем ИЗ FreeSWITCH) */
    snprintf(cx.trunk[0].out_rules[0].match, sizeof(cx.trunk[0].out_rules[0].match), "9111");
    cx.trunk[0].out_rules[0].strip = 1; cx.trunk[0].nout_rules = 1;
    aa_tr_host_t htx = mkhost(&hx);
    aa_tr_engine_t *ex = aa_tr_engine_new(&cx, &htx);
    CHECK(aa_tr_engine_start(ex, err, sizeof(err)) == 0, "X start: %s", err);
    int xport = aa_tr_engine_port(ex);
    /* порт X нужен Y как trunk-порт для исходящих (в этом тесте Y не звонит) */

    printf("-- 1: исходящий X -> Y, ответ, DTMF, отбой X\n");
    CHECK(aa_tr_originate(ex, "y", "11111111-1111-4111-8111-111111111111", "101", "Alice", "9111555", "127.0.0.1", 20000) == 0, "originate");
    CHECK(wait_ev(&hy, "IN ", 3000), "Y: входящий");
    CHECK(!strcmp(hy.last_called, "0555"), "out-правило X (9111555->111555) + in-правило Y (111555->0555): %s", hy.last_called);
    CHECK(!strcmp(hy.last_calling, "101") && !strcmp(hy.last_name, "Alice"), "calling %s / name %s", hy.last_calling, hy.last_name);
    CHECK(hy.codec == AA_TR_CODEC_A && hy.media_port == 20000 && !strcmp(hy.media_ip, "127.0.0.1"), "media %s:%d codec %u", hy.media_ip, hy.media_port, hy.codec);
    CHECK(wait_ev(&hx, "MEDIA 127.0.0.1:30000", 3000), "X: media от Y");
    CHECK(wait_ev(&hx, "RING", 3000), "X: ringing");
    CHECK(aa_tr_answer(ey, hy.last_id) == 0, "Y answer");
    CHECK(wait_ev(&hx, "ANS", 3000), "X: answered");
    usleep(100000);
    const char *digs = "1234567890*#";
    for (const char *d = digs; *d; d++) CHECK(aa_tr_dtmf(ex, "11111111-1111-4111-8111-111111111111", *d) == 0, "dtmf send");
    for (const char *d = digs; *d; d++) { char w[16]; snprintf(w, sizeof(w), "DTMF %c", *d); CHECK(wait_ev(&hy, w, 2000), "Y: %s", w); }
    CHECK(aa_tr_dtmf(ey, hy.last_id, '7') == 0, "dtmf Y->X");
    CHECK(wait_ev(&hx, "DTMF 7", 2000), "X: DTMF 7");
    CHECK(aa_tr_active_calls(ex, "y") == 1 && aa_tr_active_calls(ey, "x") == 1, "активных 1/1: %d/%d", aa_tr_active_calls(ex, "y"), aa_tr_active_calls(ey, "x"));
    CHECK(aa_tr_hangup(ex, "11111111-1111-4111-8111-111111111111", 16) == 0, "X hangup");
    CHECK(wait_ev(&hy, "HUP 16", 3000), "Y: HUP 16");
    usleep(100000);
    CHECK(aa_tr_active_calls(ex, "y") == 0 && aa_tr_active_calls(ey, "x") == 0, "после отбоя 0/0: %d/%d", aa_tr_active_calls(ex, "y"), aa_tr_active_calls(ey, "x"));

    printf("-- 2: отбой стороной Y после ответа\n");
    CHECK(aa_tr_originate(ex, "y", "22222222-2222-4222-8222-222222222222", "", "", "999", "127.0.0.1", 20002) == 0, "originate 2");
    CHECK(wait_ev(&hy, "IN ", 3000), "Y: входящий 2");
    CHECK(!strcmp(hy.last_called, "999") && hy.last_calling[0] == 0, "без правила и без calling: '%s' '%s'", hy.last_called, hy.last_calling);
    aa_tr_answer(ey, hy.last_id);
    CHECK(wait_ev(&hx, "ANS", 3000), "X: ANS 2");
    CHECK(aa_tr_hangup(ey, hy.last_id, 17) == 0, "Y hangup");
    CHECK(wait_ev(&hx, "HUP 17", 3000), "X: HUP 17");

    printf("-- 3: отбой до ответа (Y отклоняет с cause 21), X отменяет до ответа\n");
    hy.accept_rc = 21;
    CHECK(aa_tr_originate(ex, "y", "33333333-3333-4333-8333-333333333333", "1", "", "5", "127.0.0.1", 20004) == 0, "originate 3");
    CHECK(wait_ev(&hx, "HUP 21", 3000), "X: HUP 21 после отказа");
    hy.accept_rc = 0;
    CHECK(aa_tr_originate(ex, "y", "44444444-4444-4444-8444-444444444444", "1", "", "5", "127.0.0.1", 20006) == 0, "originate 4");
    CHECK(wait_ev(&hx, "RING", 3000), "X: ring 4");
    CHECK(aa_tr_hangup(ex, "44444444-4444-4444-8444-444444444444", 19) == 0, "X cancel");
    CHECK(wait_ev(&hy, "HUP 19", 3000), "Y: HUP 19 отмена");

    printf("-- 4: ошибки originate\n");
    CHECK(aa_tr_originate(ex, "nope", "55555555-5555-4555-8555-555555555555", "1", "", "5", "127.0.0.1", 1) == 3, "нет транка = 3");
    {
        aa_tr_config_t c2; mkcfg(&c2, "y", 1, "PCMA", 100, NULL, 0, "");        /* порт 1 — никто не слушает */
        aa_tr_engine_reload(ex, &c2);
        CHECK(aa_tr_originate(ex, "y", "66666666-6666-4666-8666-666666666666", "1", "", "5", "127.0.0.1", 1) == 0, "originate на закрытый порт");
        CHECK(wait_ev(&hx, "HUP 38", 3000), "X: нет соединения = 38");
        mkcfg(&c2, "y", yport, "PCMA", 100, NULL, 0, ""); aa_tr_engine_reload(ex, &c2);
    }

    printf("-- 5: кодек: X=PCMU, Y=PCMA -> отбой 88; оба PCMU -> ok\n");
    {
        aa_tr_config_t c2; mkcfg(&c2, "y", yport, "PCMU", 100, NULL, 0, ""); aa_tr_engine_reload(ex, &c2);
        CHECK(aa_tr_originate(ex, "y", "77777777-7777-4777-8777-777777777777", "1", "", "5", "127.0.0.1", 20008) == 0, "originate 7");
        CHECK(wait_ev(&hx, "HUP 88", 3000), "X: HUP 88");
        mkcfg(&cy, "x", 1, "PCMU", 100, NULL, 0, ""); aa_tr_engine_reload(ey, &cy);
        CHECK(aa_tr_originate(ex, "y", "88888888-8888-4888-8888-888888888888", "1", "", "5", "127.0.0.1", 20010) == 0, "originate 8");
        CHECK(wait_ev(&hy, "IN ", 3000) && hy.codec == AA_TR_CODEC_U, "Y: PCMU codec=%u", hy.codec);
        CHECK(wait_ev(&hx, "MEDIA 127.0.0.1:30000 c=2", 3000), "X: MEDIA c=2");
        aa_tr_hangup(ex, "88888888-8888-4888-8888-888888888888", 16);
        CHECK(wait_ev(&hy, "HUP 16", 3000), "Y: HUP");
        mkcfg(&cy, "x", 1, "PCMA", 100, "111", 3, "0"); aa_tr_engine_reload(ey, &cy);
        mkcfg(&c2, "y", yport, "PCMA", 100, NULL, 0, ""); aa_tr_engine_reload(ex, &c2);
    }

    printf("-- 6: лимит линий\n");
    {
        mkcfg(&cy, "x", 1, "PCMA", 1, NULL, 0, ""); aa_tr_engine_reload(ey, &cy);
        CHECK(aa_tr_originate(ex, "y", "a1111111-1111-4111-8111-111111111111", "1", "", "5", "127.0.0.1", 20012) == 0, "первый");
        CHECK(wait_ev(&hy, "IN ", 3000), "Y: первый");
        CHECK(aa_tr_originate(ex, "y", "a2222222-2222-4222-8222-222222222222", "1", "", "6", "127.0.0.1", 20014) == 0, "второй стартует");
        CHECK(wait_ev(&hx, "HUP 34", 3000), "X: второй отбит cause 34 на Y");
        aa_tr_hangup(ex, "a1111111-1111-4111-8111-111111111111", 16);
        usleep(300000);
        mkcfg(&cy, "x", 1, "PCMA", 100, "111", 3, "0"); aa_tr_engine_reload(ey, &cy);
        aa_tr_config_t c2; mkcfg(&c2, "y", yport, "PCMA", 1, NULL, 0, ""); aa_tr_engine_reload(ex, &c2);
        CHECK(aa_tr_originate(ex, "y", "a3333333-3333-4333-8333-333333333333", "1", "", "5", "127.0.0.1", 20016) == 0, "лимит X: первый");
        CHECK(aa_tr_originate(ex, "y", "a4444444-4444-4444-8444-444444444444", "1", "", "6", "127.0.0.1", 20018) == 34, "лимит X: второй = 34");
        aa_tr_hangup(ex, "a3333333-3333-4333-8333-333333333333", 16);
        usleep(300000);
        mkcfg(&c2, "y", yport, "PCMA", 100, NULL, 0, ""); aa_tr_engine_reload(ex, &c2);
    }

    printf("-- 7: STATUS 47 / 16 от дальней стороны\n");
    usleep(1500000);   /* пусть Y отправит свой стартовый STATUS 31/16 служебному соединению X до проверки */
    {
        int fd = raw_connect(xport);
        CHECK(fd >= 0, "raw connect");
        aa_tr_msg_t m; uint8_t buf[512];
        aa_tr_msg_init(&m, AA_TR_STATUS); m.callref = 0; m.dest = 0; aa_tr_guid_random(m.guid);
        m.cause = 47; m.has_callstate = 1; m.callstate = 0;
        int n = aa_tr_build(buf, sizeof(buf), &m);
        CHECK(n > 0 && send(fd, buf, (size_t) n, 0) == n, "STATUS 47 послан");
        usleep(150000);
        CHECK(!aa_tr_trunk_in_service(ex, "y"), "транк y выведен");
        CHECK(aa_tr_originate(ex, "y", "b1111111-1111-4111-8111-111111111111", "1", "", "5", "127.0.0.1", 20020) == 38, "originate при выводе = 38");
        m.cause = 31; n = aa_tr_build(buf, sizeof(buf), &m); send(fd, buf, (size_t) n, 0);
        m.cause = 16; n = aa_tr_build(buf, sizeof(buf), &m); send(fd, buf, (size_t) n, 0);
        usleep(150000);
        CHECK(aa_tr_trunk_in_service(ex, "y"), "транк y введён обратно");
        close(fd);
    }

    printf("-- 8: «живой» SETUP из дампа 2111-Al (193 -> 6): Y отвечает как .6\n");
    {
        size_t sn = 0;
        const uint8_t *sf = vec("2111-Al.", 193, 0, &sn);
        CHECK(sf != NULL, "SETUP в векторах");
        if (sf) {
            hy.mark = hy.n;
            int fd = raw_connect(yport);
            CHECK(send(fd, sf, sn, 0) == (ssize_t) sn, "SETUP послан");
            aa_tr_msg_t m; const char *seq[8]; int ns = 0; uint32_t theirs_msd = 0;
            for (int i = 0; i < 4; i++) { int r = raw_next(fd, &m, 2000); CHECK(r == 1, "ответ #%d (r=%d)", i, r); if (r != 1) break; seq[ns++] = K(&m);
                if (m.type == AA_TR_ALERTING) CHECK(m.has_prog && m.prog == 0x88 && m.has_media && m.rtp_port == 30000 && m.codecs == AA_TR_CODEC_A && m.dest, "ALERTING: prog/media/codec/dest");
                if (m.type == AA_TR_CALL_PROC) CHECK(m.dest == 1 && m.callref == 0x3729, "CALL_PROC callref %x dest %d", m.callref, m.dest);
                if (m.h245 == AA_H245_TCS) CHECK(m.codecs == AA_TR_CODEC_A, "наш TCS = A"); }
            CHECK(ns == 4 && !strcmp(seq[0], "PROC") && !strcmp(seq[1], "ALERT") && !strcmp(seq[2], "TCS") && !strcmp(seq[3], "MSD"), "последовательность PROC ALERT TCS MSD");
            CHECK(wait_ev(&hy, "IN 101>102", 2000) && hy.codec == AA_TR_CODEC_A, "Y: IN 101>102");
            CHECK(!strcmp(hy.last_name, "Analog phone"), "name '%s'", hy.last_name);
            theirs_msd = m.msd_num;       /* наш MSD (последний прочитанный) */
            /* отвечаем как 193: TCS (из дампа), MSD 3f4cad, TCSack, MSDack */
            aa_tr_msg_t o; uint8_t buf[512]; int n;
            aa_tr_msg_init(&o, AA_TR_FACILITY); o.callref = 0x3729; o.dest = 0; memcpy(o.guid, m.guid, 16);
            o.pdu_n = aa_tr_pdu_tcs(o.pdu, AA_TR_CODEC_A); n = aa_tr_build(buf, sizeof(buf), &o); send(fd, buf, (size_t) n, 0);
            o.pdu_n = aa_tr_pdu_msd(o.pdu, 0x3f4cad); n = aa_tr_build(buf, sizeof(buf), &o); send(fd, buf, (size_t) n, 0);
            int got_tcsack = 0, got_msdack = 0, master_bit = -1;
            for (int i = 0; i < 2; i++) { if (raw_next(fd, &m, 2000) != 1) break; if (m.h245 == AA_H245_TCSACK) got_tcsack = 1; if (m.h245 == AA_H245_MSDACK) { got_msdack = 1; master_bit = m.msd_master; } }
            CHECK(got_tcsack && got_msdack, "TCSack и MSDack от нас");
            CHECK(master_bit == aa_tr_msd_we_are_master(theirs_msd, 0x3f4cad), "ack master=%d по правилу", master_bit);
            o.pdu_n = aa_tr_pdu_tcsack(o.pdu); n = aa_tr_build(buf, sizeof(buf), &o); send(fd, buf, (size_t) n, 0);
            o.pdu_n = aa_tr_pdu_msdack(o.pdu, !master_bit); n = aa_tr_build(buf, sizeof(buf), &o); send(fd, buf, (size_t) n, 0);
            if (!master_bit) {            /* мы master -> x1 нет; мы slave -> шлём x1 */
            }
            if (master_bit == 0) {        /* наш ack a0 = мы slave -> ждём x1 */
                int r = raw_next(fd, &m, 2000);
                CHECK(r == 1 && m.h245 == AA_H245_OTHER && m.pdu_n == 8, "x1 от slave (r=%d)", r);
            }
            CHECK(aa_tr_answer(ey, hy.last_id) == 0, "Y answer");
            int r = raw_next(fd, &m, 2000);
            CHECK(r == 1 && m.type == AA_TR_CONNECT && m.has_prog && m.prog == 0x82 && m.has_display && !strcmp(m.display, "FreeSWITCH"), "CONNECT (r=%d)", r);
            o.pdu_n = aa_tr_pdu_uii(o.pdu, '9'); n = aa_tr_build(buf, sizeof(buf), &o); send(fd, buf, (size_t) n, 0);
            CHECK(wait_ev(&hy, "DTMF 9", 2000), "Y: DTMF 9");
            o.pdu_n = aa_tr_pdu_endsession(o.pdu); n = aa_tr_build(buf, sizeof(buf), &o); send(fd, buf, (size_t) n, 0);
            aa_tr_msg_init(&o, AA_TR_RELEASE_COMPLETE); o.callref = 0x3729; memcpy(o.guid, m.guid, 16); o.cause = 16;
            n = aa_tr_build(buf, sizeof(buf), &o); send(fd, buf, (size_t) n, 0);
            CHECK(wait_ev(&hy, "HUP 16", 2000), "Y: HUP 16");
            close(fd);
        }
    }

    printf("-- 9: неизвестный адрес -> отбой 21\n");
    {
        aa_tr_config_t c2; mkcfg(&c2, "zz", 1, "PCMA", 100, NULL, 0, "");
        snprintf(c2.trunk[0].ip, sizeof(c2.trunk[0].ip), "10.9.9.9"); aa_tr_engine_reload(ey, &c2);
        size_t sn = 0; const uint8_t *sf = vec("2111-Al.", 193, 0, &sn);
        int fd = raw_connect(yport); send(fd, sf, sn, 0);
        aa_tr_msg_t m; g_pn = 0;
        int r = raw_next(fd, &m, 2000);
        CHECK(r == 1 && m.type == AA_TR_RELEASE_COMPLETE && m.cause == 21, "RC 21 (r=%d cause=%d)", r, m.cause);
        close(fd);
        mkcfg(&cy, "x", 1, "PCMA", 100, "111", 3, "0"); aa_tr_engine_reload(ey, &cy);
    }

    printf("-- 10: занятый порт: модуль не падает, listener поднимается позже\n");
    {
        aa_tr_config_t c3; mkcfg(&c3, "x", 1, "PCMA", 100, NULL, 0, "");
        int bfd = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in ba = { 0 }; socklen_t bl = sizeof(ba);
        ba.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &ba.sin_addr);
        bind(bfd, (struct sockaddr *) &ba, sizeof(ba)); listen(bfd, 1); getsockname(bfd, (struct sockaddr *) &ba, &bl);
        c3.h225_port = ntohs(ba.sin_port);
        aa_tr_host_t h3 = mkhost(&hx);
        aa_tr_engine_t *e3 = aa_tr_engine_new(&c3, &h3);
        CHECK(aa_tr_engine_start(e3, err, sizeof(err)) == 0, "start при занятом порте не должен падать");
        CHECK(!aa_tr_engine_listening(e3), "listener не работает");
        char *s3 = aa_tr_status_text(e3); CHECK(strstr(s3, "NOT LISTENING") != NULL, "status говорит NOT LISTENING"); free(s3);
        close(bfd);
        for (int i = 0; i < 80 && !aa_tr_engine_listening(e3); i++) usleep(100000);
        CHECK(aa_tr_engine_listening(e3), "listener поднялся после освобождения порта");
        aa_tr_engine_stop(e3); aa_tr_engine_free(e3);
    }

    printf("-- 11: служебное соединение к транку: STATUS 31, 16, затем реакция на 47\n");
    {
        int lfd = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in la = { 0 }; socklen_t ll = sizeof(la);
        la.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &la.sin_addr);
        bind(lfd, (struct sockaddr *) &la, sizeof(la)); listen(lfd, 4); getsockname(lfd, (struct sockaddr *) &la, &ll);
        aa_tr_config_t c4; mkcfg(&c4, "hb", ntohs(la.sin_port), "PCMA", 100, NULL, 0, "");
        c4.h225_port = 0;
        aa_tr_host_t h4 = mkhost(&hx);
        aa_tr_engine_t *e4 = aa_tr_engine_new(&c4, &h4);
        CHECK(aa_tr_engine_start(e4, err, sizeof(err)) == 0, "start e4");
        struct pollfd pp = { lfd, POLLIN, 0 };
        CHECK(poll(&pp, 1, 3000) > 0, "engine сам подключился к транку");
        int cfd = accept(lfd, NULL, NULL);
        uint8_t rb[512]; size_t rn = 0; int got31 = 0, got16 = 0;
        for (int i = 0; i < 30 && !(got31 && got16); i++) {
            struct pollfd p2 = { cfd, POLLIN, 0 };
            if (poll(&p2, 1, 100) > 0) { ssize_t r = recv(cfd, rb + rn, sizeof(rb) - rn, 0); if (r <= 0) break; rn += (size_t) r; }
            for (;;) {
                int tl = aa_tr_tpkt_len(rb, rn); aa_tr_msg_t m;
                if (tl <= 0) break;
                if (aa_tr_parse(rb, (size_t) tl, &m) == 0 && m.type == AA_TR_STATUS && m.callref == 0) { if (m.cause == 31 && !got16) got31 = 1; if (m.cause == 16 && got31) got16 = 1; }
                memmove(rb, rb + tl, rn - (size_t) tl); rn -= (size_t) tl;
            }
        }
        CHECK(got31 && got16, "получены STATUS 31, затем 16");
        aa_tr_msg_t m; uint8_t buf[512];
        aa_tr_msg_init(&m, AA_TR_STATUS); m.callref = 0; m.dest = 0; aa_tr_guid_random(m.guid); m.cause = 47; m.has_callstate = 1; m.callstate = 0;
        int n = aa_tr_build(buf, sizeof(buf), &m); send(cfd, buf, (size_t) n, 0);
        usleep(300000);
        CHECK(!aa_tr_trunk_in_service(e4, "hb"), "STATUS 47 по служебному соединению выводит транк");
        m.cause = 16; n = aa_tr_build(buf, sizeof(buf), &m); send(cfd, buf, (size_t) n, 0);
        usleep(300000);
        CHECK(aa_tr_trunk_in_service(e4, "hb"), "STATUS 16 возвращает транк");
        close(cfd); close(lfd);
        aa_tr_engine_stop(e4); aa_tr_engine_free(e4);
    }

    char *st = aa_tr_status_text(ex);
    printf("-- status X:\n%s", st); free(st);
    aa_tr_engine_stop(ex); aa_tr_engine_stop(ey);
    aa_tr_engine_free(ex); aa_tr_engine_free(ey);
    printf("test_tr_engine: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
