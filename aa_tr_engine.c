/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * aa_tr_engine.c — движок H.323-транка: сокеты, потоки, автомат вызова (см. aa_tr_engine.h).
 * Последовательности сообщений сняты с дампов (captures/): см. docs/CAPTURES.md.
 *
 *  Входящий (мы — «.6» из дампов): SETUP <- ; CALL_PROC, ALERTING(fastStart), TCS, MSD ->; <- TCS, MSD; TCSack, MSDack ->;
 *  <- TCSack, MSDack; (slave шлёт x1); CONNECT -> при ответе FreeSWITCH; отбой: END + RELEASE_COMPLETE.
 *  Исходящий (мы — «193»): SETUP ->; <- CALL_PROC, ALERTING, TCS, MSD; TCS, MSD, TCSack, MSDack ->; <- TCSack, MSDack;
 *  <- CONNECT.
 *
 * Свободный пакет. Автор: Сенашенко Д.В., 2026.
 */
#define _GNU_SOURCE
#include "aa_tr_engine.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS 4096
#define RBUF 8192

enum { CS_NEW = 0, CS_PROC, CS_ALERTING, CS_CONNECTED, CS_DONE, CS_OUT_NEW };   /* CS_OUT_NEW: исходящий, SETUP ещё не отправлен */
enum { EV_RING = 1, EV_MEDIA, EV_ANSWER, EV_HANGUP, EV_DTMF };

typedef struct { int kind; int cause; char ch; char ip[48]; int port; unsigned codec; } ev_t;

typedef struct conn conn_t;
struct conn {
    conn_t *next;
    aa_tr_engine_t *e;
    int fd;
    int incoming;
    aa_tr_trunk_t trunk;
    int have_trunk;
    char call_id[AA_TR_ID_LEN];
    char peer_ip[48];
    int state;
    unsigned callref;
    uint8_t guid[16];
    unsigned codec;                    /* выбранный кодек (AA_TR_CODEC_A/U) */
    char rtp_ip[48]; int rtp_port;     /* наш приём RTP */
    char pm_ip[48]; int pm_port;       /* RTP дальней стороны */
    char o_calling[64], o_calling_name[64], o_called[64];   /* исходящий вызов: параметры SETUP */
    uint32_t msd_ours, msd_theirs;
    int we_master;
    int f_tcs_sent, f_tcs_rcv, f_msd_sent, f_msd_rcv, f_tcsack_sent, f_msdack_sent, f_tcsack_rcv, f_msdack_rcv, f_x1_sent;
    int kill, kill_cause;
    int answer_pending;
    int peer_end;
    int notified;                      /* on_hangup уже отправлен хозяину */
    int media_reported;
    uint8_t rb[RBUF];
    size_t rn;
};

typedef struct { char name[32]; int oos; } tstate_t;

struct aa_tr_engine {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    aa_tr_config_t cfg;
    aa_tr_host_t host;
    int lfd;
    int lport;
    char listen_err[160];       /* пусто — listener работает */
    pthread_t acc_th, hl_th;
    int acc_started, hl_started;
    atomic_int stop;
    conn_t *conns;
    int nconn;
    tstate_t ts[AA_TR_MAX_TRUNKS];
    int nts;
    unsigned long n_in, n_out, n_rej;
};

/* ------------------------------------------------------------------------------------------------ */
static void lg(aa_tr_engine_t *e, int lvl, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    if (!e->host.log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    e->host.log(e->host.ctx, lvl, b);
}

static void uuid_str(char *o)
{
    uint8_t g[16];
    aa_tr_guid_random(g);
    g[6] = (uint8_t) ((g[6] & 0x0f) | 0x40);
    g[8] = (uint8_t) ((g[8] & 0x3f) | 0x80);
    snprintf(o, AA_TR_ID_LEN, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[0], g[1], g[2], g[3], g[4], g[5],
             g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static unsigned rnd_callref(void)
{
    uint8_t g[16];
    aa_tr_guid_random(g);
    unsigned v = (((unsigned) g[0] << 8) | g[1]) & 0x7fff;
    return v ? v : 1;
}

static uint32_t rnd_msd(void)
{
    uint8_t g[16];
    aa_tr_guid_random(g);
    uint32_t v = (((uint32_t) g[0] << 16) | ((uint32_t) g[1] << 8) | g[2]) & 0xffffff;
    return v < 65536 ? v + 65536 : v;
}

static unsigned codec_bit(const char *name)
{
    return (name && (name[0] == 'P' || name[0] == 'p') && (name[3] == 'U' || name[3] == 'u')) ? AA_TR_CODEC_U : AA_TR_CODEC_A;
}

/* ------------------------------------------------------------------------------------------------ */
/* Таблица состояния транков (STATUS от дальней стороны). Вызывать под e->mu. */
static tstate_t *ts_get(aa_tr_engine_t *e, const char *name)
{
    for (int i = 0; i < e->nts; i++) if (!strcmp(e->ts[i].name, name)) return &e->ts[i];
    if (e->nts >= AA_TR_MAX_TRUNKS) return NULL;
    tstate_t *t = &e->ts[e->nts++];
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name);
    return t;
}

static int active_calls_locked(aa_tr_engine_t *e, const char *trunk)
{
    int n = 0;
    for (conn_t *c = e->conns; c; c = c->next)
        if (c->have_trunk && !strcmp(c->trunk.name, trunk) && (c->state == CS_PROC || c->state == CS_ALERTING || c->state == CS_CONNECTED || c->state == CS_OUT_NEW)) n++;
    return n;
}

/* ------------------------------------------------------------------------------------------------ */
/* Отправка */
static int send_all(int fd, const uint8_t *b, size_t n)
{
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t) w;
    }
    return 0;
}

static int send_msg(conn_t *c, const aa_tr_msg_t *m)
{
    uint8_t buf[1024];
    int n;
    if (c->fd < 0) return -1;
    n = aa_tr_build(buf, sizeof(buf), m);
    if (n <= 0) { lg(c->e, 40, "aa_tr: сборка сообщения типа %d не удалась", (int) m->type); return -1; }
    return send_all(c->fd, buf, (size_t) n);
}

static void mk(conn_t *c, aa_tr_msg_t *m, aa_tr_type_t t)
{
    aa_tr_msg_init(m, t);
    m->callref = c->callref;
    m->dest = c->incoming;               /* бит call reference: 1 у принявшей вызов стороны */
    memcpy(m->guid, c->guid, 16);
    snprintf(m->product, sizeof(m->product), "%s", c->e->cfg.product);
    snprintf(m->version, sizeof(m->version), "%s", c->e->cfg.version);
}

static int send_pdu(conn_t *c, size_t (*gen)(uint8_t *))
{
    aa_tr_msg_t m;
    mk(c, &m, AA_TR_FACILITY);
    m.pdu_n = gen(m.pdu);
    return send_msg(c, &m);
}

static size_t gen_tcsack(uint8_t *o) { return aa_tr_pdu_tcsack(o); }
static size_t gen_end(uint8_t *o) { return aa_tr_pdu_endsession(o); }
static size_t gen_x1(uint8_t *o) { return aa_tr_pdu_caller_x1(o); }

static int send_tcs(conn_t *c)
{
    aa_tr_msg_t m;
    mk(c, &m, AA_TR_FACILITY);
    m.pdu_n = aa_tr_pdu_tcs(m.pdu, c->codec);
    c->f_tcs_sent = 1;
    return send_msg(c, &m);
}

static int send_msd(conn_t *c)
{
    aa_tr_msg_t m;
    mk(c, &m, AA_TR_FACILITY);
    if (!c->msd_ours) c->msd_ours = rnd_msd();
    m.pdu_n = aa_tr_pdu_msd(m.pdu, c->msd_ours);
    c->f_msd_sent = 1;
    return send_msg(c, &m);
}

static int send_rc(conn_t *c, int cause)
{
    aa_tr_msg_t m;
    mk(c, &m, AA_TR_RELEASE_COMPLETE);
    m.cause = cause > 0 ? cause : 16;
    return send_msg(c, &m);
}

static int send_connect(conn_t *c)
{
    aa_tr_msg_t m;
    mk(c, &m, AA_TR_CONNECT);
    m.has_prog = 1; m.prog = 0x82;
    if (c->e->cfg.display[0]) { m.has_display = 1; snprintf(m.display, sizeof(m.display), "%s", c->e->cfg.display); }
    return send_msg(c, &m);
}

/* x1 шлёт slave, когда обмен TCS/MSD завершён с обеих сторон (по дампам). */
static void maybe_x1(conn_t *c)
{
    if (!c->f_x1_sent && !c->we_master && c->f_msd_rcv && c->f_tcsack_sent && c->f_msdack_sent && c->f_tcsack_rcv && c->f_msdack_rcv) {
        c->f_x1_sent = 1;
        send_pdu(c, gen_x1);
    }
}

/* ------------------------------------------------------------------------------------------------ */
static void ev_add(ev_t *ev, int *n, int kind)
{
    if (*n >= 8) return;
    memset(&ev[*n], 0, sizeof(ev[*n]));
    ev[*n].kind = kind;
    (*n)++;
}

static void dispatch(aa_tr_engine_t *e, const char *call_id, const ev_t *ev, int n)
{
    for (int i = 0; i < n; i++) {
        if (e->stop && ev[i].kind != EV_HANGUP) continue;
        switch (ev[i].kind) {
        case EV_RING: if (e->host.on_ringing) e->host.on_ringing(e->host.ctx, call_id); break;
        case EV_MEDIA: if (e->host.on_media) e->host.on_media(e->host.ctx, call_id, ev[i].ip, ev[i].port, ev[i].codec); break;
        case EV_ANSWER: if (e->host.on_answered) e->host.on_answered(e->host.ctx, call_id); break;
        case EV_HANGUP: if (e->host.on_hangup) e->host.on_hangup(e->host.ctx, call_id, ev[i].cause); break;
        case EV_DTMF: if (e->host.on_dtmf) e->host.on_dtmf(e->host.ctx, call_id, ev[i].ch); break;
        }
    }
}

static void ip4str(const uint8_t *ip, char *o, size_t cap)
{
    snprintf(o, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void ip4parse(const char *s, uint8_t *o)
{
    struct in_addr a;
    memset(o, 0, 4);
    if (inet_pton(AF_INET, s, &a) == 1) memcpy(o, &a, 4);
}

/* Отбой входящего SETUP (до принятия вызова хозяином). */
static void reject_setup(conn_t *c, const aa_tr_msg_t *m, int cause)
{
    aa_tr_msg_t r;
    c->callref = m->callref;
    memcpy(c->guid, m->guid, 16);
    mk(c, &r, AA_TR_RELEASE_COMPLETE);
    r.cause = cause;
    send_msg(c, &r);
    c->state = CS_DONE;
    c->e->n_rej++;
}

/* Общая часть: что делаем по приходу сообщения в уже идущем вызове. Под e->mu. */
static void on_h245(conn_t *c, const aa_tr_msg_t *m, ev_t *ev, int *nev)
{
    switch (m->h245) {
    case AA_H245_TCS:
        c->f_tcs_rcv = 1;
        if (!c->f_tcs_sent) send_tcs(c);
        if (!c->f_msd_sent) send_msd(c);
        send_pdu(c, gen_tcsack);
        c->f_tcsack_sent = 1;
        break;
    case AA_H245_MSD: {
        aa_tr_msg_t a;
        if (!c->f_tcs_sent) send_tcs(c);
        if (!c->f_msd_sent) send_msd(c);
        c->f_msd_rcv = 1;
        c->msd_theirs = m->msd_num;
        c->we_master = aa_tr_msd_we_are_master(c->msd_ours, c->msd_theirs);
        mk(c, &a, AA_TR_FACILITY);
        a.pdu_n = aa_tr_pdu_msdack(a.pdu, c->we_master);
        send_msg(c, &a);
        c->f_msdack_sent = 1;
        break;
    }
    case AA_H245_TCSACK: c->f_tcsack_rcv = 1; break;
    case AA_H245_MSDACK: c->f_msdack_rcv = 1; break;
    case AA_H245_UII:
        if (m->uii) { ev_add(ev, nev, EV_DTMF); ev[*nev - 1].ch = m->uii; }
        break;
    case AA_H245_ENDSESSION: c->peer_end = 1; break;
    default: break;      /* x1 и прочее — игнорируем */
    }
    maybe_x1(c);
}

/* Состояние транка по STATUS с глобальным call reference: cause 47 - выведен из обслуживания, 31/16 - в обслуживании. */
static void set_trunk_oos(aa_tr_engine_t *e, const char *name, int cause)
{
    tstate_t *t = ts_get(e, name);
    if (!t) return;
    int oos = (cause == 47);
    if (oos != t->oos) lg(e, 30, "aa_tr: транк %s: дальняя сторона объявила %s (STATUS cause=%d)", name, oos ? "ВЫВЕДЕН ИЗ ОБСЛУЖИВАНИЯ" : "в обслуживании", cause);
    t->oos = oos;
}

static void handle_status(conn_t *c, const aa_tr_msg_t *m)
{
    if (m->callref != 0 || !c->have_trunk) return;
    set_trunk_oos(c->e, c->trunk.name, m->cause);
}

/* Входящий SETUP. Вызывается с захваченным e->mu; на время обращения к хозяину мьютекс отпускается. */
static void handle_setup_in(conn_t *c, const aa_tr_msg_t *m, ev_t *ev, int *nev)
{
    aa_tr_engine_t *e = c->e;
    aa_tr_incoming_t in;
    aa_tr_accept_t acc;
    unsigned offered, want;
    int rc;

    (void) ev; (void) nev;
    c->callref = m->callref;
    memcpy(c->guid, m->guid, 16);
    if (!c->have_trunk) {
        lg(e, 30, "aa_tr: вызов с неизвестного адреса %s - отбой", c->peer_ip);
        reject_setup(c, m, 21);
        return;
    }
    if (c->trunk.lines > 0 && active_calls_locked(e, c->trunk.name) >= c->trunk.lines) {
        lg(e, 30, "aa_tr: транк %s: нет свободных линий (%d) - отбой", c->trunk.name, c->trunk.lines);
        reject_setup(c, m, 34);
        return;
    }
    offered = m->codecs ? m->codecs : (AA_TR_CODEC_A | AA_TR_CODEC_U);
    want = codec_bit(c->trunk.codec);
    if (!(offered & want)) {
        lg(e, 30, "aa_tr: транк %s: АТС не предлагает кодек %s - отбой", c->trunk.name, c->trunk.codec);
        reject_setup(c, m, 88);
        return;
    }
    c->codec = want;
    memset(&in, 0, sizeof(in));
    in.trunk = &c->trunk;
    snprintf(in.peer_ip, sizeof(in.peer_ip), "%s", c->peer_ip);
    snprintf(in.calling, sizeof(in.calling), "%s", m->has_calling ? m->calling : "");
    snprintf(in.calling_name, sizeof(in.calling_name), "%s", m->has_display ? m->display : "");
    snprintf(in.called_raw, sizeof(in.called_raw), "%s", m->has_called ? m->called : "");
    aa_tr_number_in(&c->trunk, in.called_raw, in.called, sizeof(in.called));
    if (m->has_media) {
        ip4str(m->media_ip, in.media_ip, sizeof(in.media_ip));
        in.media_port = (int) m->rtp_port;
        snprintf(c->pm_ip, sizeof(c->pm_ip), "%s", in.media_ip);
        c->pm_port = in.media_port;
    }
    in.codec = c->codec;
    snprintf(in.call_id, sizeof(in.call_id), "%s", c->call_id);
    memset(&acc, 0, sizeof(acc));

    c->state = CS_PROC;
    e->n_in++;
    pthread_mutex_unlock(&e->mu);
    rc = e->host.on_incoming ? e->host.on_incoming(e->host.ctx, &in, &acc) : 1;
    pthread_mutex_lock(&e->mu);

    if (c->state == CS_DONE) return;     /* вызов закрыт во время обращения */
    if (rc == 0 && c->kill) { rc = c->kill_cause ? c->kill_cause : 16; }
    if (rc != 0) {
        lg(e, 20, "aa_tr: входящий %s->%s отклонён хозяином, cause %d", in.calling, in.called, rc);
        reject_setup(c, m, rc);
        return;
    }
    snprintf(c->rtp_ip, sizeof(c->rtp_ip), "%s", acc.rtp_ip);
    c->rtp_port = acc.rtp_port;

    aa_tr_msg_t r;
    mk(c, &r, AA_TR_CALL_PROC);
    send_msg(c, &r);

    mk(c, &r, AA_TR_ALERTING);
    r.has_prog = 1; r.prog = 0x88;
    r.has_media = 1; ip4parse(c->rtp_ip, r.media_ip); r.rtp_port = (unsigned) c->rtp_port;
    r.codecs = c->codec;
    send_msg(c, &r);
    c->state = CS_ALERTING;

    send_tcs(c);
    send_msd(c);
    if (c->answer_pending) {
        c->answer_pending = 0;
        send_connect(c);
        c->state = CS_CONNECTED;
    }
}

static void handle_frame(conn_t *c, const uint8_t *f, size_t n)
{
    aa_tr_engine_t *e = c->e;
    aa_tr_msg_t m;
    ev_t ev[8];
    int nev = 0;
    int pr = aa_tr_parse(f, n, &m);
    char call_id[AA_TR_ID_LEN];

    pthread_mutex_lock(&e->mu);
    snprintf(call_id, sizeof(call_id), "%s", c->call_id);
    if (c->state == CS_DONE) { pthread_mutex_unlock(&e->mu); return; }

    if (pr != 0) {
        if (m.type == AA_TR_RELEASE_COMPLETE) {
            lg(e, 20, "aa_tr: транк %s: RELEASE COMPLETE от АТС, cause=%d (вызов %.36s)", c->have_trunk ? c->trunk.name : "?", m.cause, call_id);
            ev_add(ev, &nev, EV_HANGUP); ev[nev - 1].cause = m.cause >= 0 ? m.cause : 16;
            c->state = CS_DONE; c->notified = 1;
        } else if (m.type == AA_TR_SETUP && c->incoming && c->state == CS_NEW) {
            lg(e, 30, "aa_tr: SETUP неизвестного вида от %s (%zu байт) - отбой", c->peer_ip, n);
            {   /* дамп сообщения в журнал: по нему можно доработать разбор без pcap */
                char hx[2 * 400 + 1];
                size_t k, nk = n < 400 ? n : 400;
                for (k = 0; k < nk; k++) snprintf(hx + 2 * k, 3, "%02x", f[k]);
                hx[2 * nk] = 0;
                lg(e, 30, "aa_tr: SETUP от %s hex: %s%s", c->peer_ip, hx, n > nk ? "..." : "");
            }
            reject_setup(c, &m, 100);
        } else {
            lg(e, 20, "aa_tr: непонятное сообщение type=%d (%zu байт) от %s - игнорирую", (int) m.type, n, c->peer_ip);
        }
        goto out;
    }

    switch (m.type) {
    case AA_TR_STATUS:
        handle_status(c, &m);
        break;
    case AA_TR_SETUP:
        if (c->incoming && c->state == CS_NEW) handle_setup_in(c, &m, ev, &nev);
        break;
    case AA_TR_CALL_PROC:
        break;
    case AA_TR_ALERTING:
        if (!c->incoming) {
            if (m.has_media && !c->media_reported) {
                c->media_reported = 1;
                ip4str(m.media_ip, c->pm_ip, sizeof(c->pm_ip));
                c->pm_port = (int) m.rtp_port;
                ev_add(ev, &nev, EV_MEDIA);
                snprintf(ev[nev - 1].ip, sizeof(ev[nev - 1].ip), "%s", c->pm_ip);
                ev[nev - 1].port = c->pm_port;
                ev[nev - 1].codec = m.codecs ? m.codecs : c->codec;
            }
            if (c->state < CS_ALERTING) { c->state = CS_ALERTING; ev_add(ev, &nev, EV_RING); }
        }
        break;
    case AA_TR_CONNECT:
        if (!c->incoming && c->state < CS_CONNECTED) {
            c->state = CS_CONNECTED;
            ev_add(ev, &nev, EV_ANSWER);
        }
        break;
    case AA_TR_FACILITY:
        on_h245(c, &m, ev, &nev);
        break;
    case AA_TR_RELEASE_COMPLETE:
        if (!c->notified) {
            lg(e, 20, "aa_tr: транк %s: RELEASE COMPLETE от АТС, cause=%d (вызов %.36s)", c->have_trunk ? c->trunk.name : "?", m.cause, call_id);
            ev_add(ev, &nev, EV_HANGUP);
            ev[nev - 1].cause = m.cause >= 0 ? m.cause : 16;
            c->notified = 1;
        }
        c->state = CS_DONE;
        break;
    default:
        break;
    }
out:
    pthread_mutex_unlock(&e->mu);
    dispatch(e, call_id, ev, nev);
}

/* ------------------------------------------------------------------------------------------------ */
static void conn_finish(conn_t *c)
{
    aa_tr_engine_t *e = c->e;
    ev_t ev[1];
    int nev = 0;
    char call_id[AA_TR_ID_LEN];

    pthread_mutex_lock(&e->mu);
    snprintf(call_id, sizeof(call_id), "%s", c->call_id);
    if (!c->notified && c->state != CS_DONE && c->have_trunk && c->state != CS_NEW && !c->kill) {
        /* соединение оборвалось без RELEASE COMPLETE */
        lg(e, 30, "aa_tr: транк %s: соединение с АТС оборвано без RELEASE COMPLETE (%s), отбой cause=%d (вызов %.36s)",
           c->trunk.name, c->peer_end ? "закрыто АТС" : "ошибка сети", c->peer_end ? 16 : 38, call_id);
        ev_add(ev, &nev, EV_HANGUP);
        ev[0].cause = c->peer_end ? 16 : 38;
        c->notified = 1;
    }
    c->state = CS_DONE;
    for (conn_t **pp = &e->conns; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    int fd = c->fd;
    c->fd = -1;
    pthread_mutex_unlock(&e->mu);
    if (fd >= 0) close(fd);
    dispatch(e, call_id, ev, nev);
    free(c);
    pthread_mutex_lock(&e->mu);
    e->nconn--;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

static long long mono_ms(void);

/* Входящее соединение от АТС без вызова (постоянное соединение CM к Far-end Listen Port): как «.6» в дампах,
 * отправляем STATUS с глобальным call reference: при появлении cause 31, затем 16, дальше 16 каждые 7.6 с,
 * пока на соединении не пришёл SETUP. */
static void idle_heartbeat(conn_t *c, int *started, long long *next)
{
    aa_tr_engine_t *e = c->e;
    aa_tr_msg_t m;
    long long now = mono_ms();
    int ok = 1;

    if (!*next) *next = now + 1000;                 /* вызовная связь начинается с SETUP сразу - ждём 1 с тишины */
    if (now < *next) return;
    pthread_mutex_lock(&e->mu);
    if (c->state != CS_NEW || !c->have_trunk || c->fd < 0) { pthread_mutex_unlock(&e->mu); return; }
    if (!*started) aa_tr_guid_random(c->guid);
    aa_tr_msg_init(&m, AA_TR_STATUS);
    m.callref = 0; m.dest = 0; memcpy(m.guid, c->guid, 16);
    m.has_callstate = 1; m.callstate = 0;
    if (!*started) { m.cause = 31; ok = send_msg(c, &m) == 0; }
    m.cause = 16;
    if (ok) ok = send_msg(c, &m) == 0;
    if (ok && !*started) lg(e, 20, "aa_tr: транк %s: входящее соединение от %s без вызова - отправил STATUS 31/16", c->trunk.name, c->peer_ip);
    pthread_mutex_unlock(&e->mu);
    *started = 1;
    *next = now + 7600;
}

static void conn_loop(conn_t *c)
{
    aa_tr_engine_t *e = c->e;
    int hb_started = 0;
    long long hb_next = 0;
    for (;;) {
        struct pollfd p;
        int pr, done;

        pthread_mutex_lock(&e->mu);
        done = (c->state == CS_DONE || e->stop || (c->kill && c->state == CS_NEW));
        pthread_mutex_unlock(&e->mu);
        if (done) break;

        if (c->incoming) idle_heartbeat(c, &hb_started, &hb_next);
        p.fd = c->fd; p.events = POLLIN; p.revents = 0;
        pr = poll(&p, 1, 200);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = recv(c->fd, c->rb + c->rn, sizeof(c->rb) - c->rn, 0);
            if (n <= 0) break;
            c->rn += (size_t) n;
            for (;;) {
                int tl = aa_tr_tpkt_len(c->rb, c->rn);
                if (tl < 0) { lg(e, 30, "aa_tr: не TPKT от %s - закрываю", c->peer_ip); c->rn = 0; goto fin; }
                if (tl == 0) break;
                handle_frame(c, c->rb, (size_t) tl);
                memmove(c->rb, c->rb + tl, c->rn - (size_t) tl);
                c->rn -= (size_t) tl;
            }
            if (c->rn >= sizeof(c->rb)) goto fin;
        }
    }
fin:
    conn_finish(c);
}

static void *conn_thread_in(void *arg)
{
    conn_loop((conn_t *) arg);
    return NULL;
}

static int connect_to(aa_tr_engine_t *e, const aa_tr_trunk_t *t, int *fd_out)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (e->cfg.local_ip[0]) {
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        inet_pton(AF_INET, e->cfg.local_ip, &a.sin_addr);
        if (bind(fd, (struct sockaddr *) &a, sizeof(a)) != 0) { close(fd); return -1; }
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) t->port);
    if (inet_pton(AF_INET, t->ip, &a.sin_addr) != 1) { close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *) &a, sizeof(a));
    if (r != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r != 0) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0; socklen_t el = sizeof(err);
        if (poll(&p, 1, 5000) <= 0) { close(fd); return -1; }
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fl);
    *fd_out = fd;
    return 0;
}

static void sock_opts(int fd)
{
    int one = 1, idle = 60, intvl = 15, cnt = 4;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

static void *conn_thread_out(void *arg)
{
    conn_t *c = (conn_t *) arg;
    aa_tr_engine_t *e = c->e;
    int fd = -1;

    if (connect_to(e, &c->trunk, &fd) != 0) {
        lg(e, 30, "aa_tr: транк %s: нет соединения с %s:%d", c->trunk.name, c->trunk.ip, c->trunk.port);
        pthread_mutex_lock(&e->mu);
        c->fd = -1;
        pthread_mutex_unlock(&e->mu);
        conn_finish(c);       /* state == CS_OUT_NEW -> on_hangup(38) */
        return NULL;
    }
    sock_opts(fd);
    pthread_mutex_lock(&e->mu);
    c->fd = fd;
    if (c->kill || e->stop) {
        pthread_mutex_unlock(&e->mu);
        c->kill = 1;
        conn_finish(c);
        return NULL;
    }
    {
        aa_tr_msg_t m;
        mk(c, &m, AA_TR_SETUP);
        m.has_sc = 1;
        m.has_bearer = 1; m.bearer[0] = 0x90; m.bearer[1] = 0x90; m.bearer[2] = 0xa3;
        m.has_prog = 1; m.prog = 0x83;
        if (c->o_calling[0]) { m.has_calling = 1; m.calling_o3 = 0x00; m.calling_o3a = 0x80; snprintf(m.calling, sizeof(m.calling), "%s", c->o_calling); }
        m.has_called = 1; m.called_o3 = 0x81; snprintf(m.called, sizeof(m.called), "%s", c->o_called);
        {
            const char *dn = c->o_calling_name[0] ? c->o_calling_name : e->cfg.display;
            if (dn[0]) { m.has_display = 1; snprintf(m.display, sizeof(m.display), "%s", dn); }
        }
        m.has_media = 1; ip4parse(c->rtp_ip, m.media_ip); m.rtp_port = (unsigned) c->rtp_port;
        ip4parse(e->cfg.local_ip, m.sig_ip); m.sig_port = (unsigned) e->cfg.h225_port;
        m.codecs = c->codec;
        c->state = CS_PROC;
        if (send_msg(c, &m) != 0) {
            pthread_mutex_unlock(&e->mu);
            conn_finish(c);
            return NULL;
        }
        e->n_out++;
    }
    pthread_mutex_unlock(&e->mu);
    conn_loop(c);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ */
/* Служебное соединение к каждому транку (как у «.6» в дампах): АТС считает транк рабочим, пока на её H.225-порт
 * приходит STATUS с глобальным call reference. При подключении: STATUS cause 31, затем cause 16; дальше STATUS cause 16
 * примерно раз в 7.5 с (интервал из h323_out_b_o.pcap). Ответные STATUS от АТС (31/16/47) обновляют состояние транка.
 * Обрыв - переподключение раз в 5 с. */
#define HL_PERIOD_MS 7500
#define HL_RETRY_MS 5000

typedef struct { char name[sizeof(((aa_tr_trunk_t *) 0)->name)]; int fd; long long next_send, next_try; uint8_t guid[16]; uint8_t rb[512]; size_t rn; } hl_t;

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int hl_send(hl_t *h, int cause)
{
    aa_tr_msg_t m;
    uint8_t buf[512];
    int n;
    aa_tr_msg_init(&m, AA_TR_STATUS);
    m.callref = 0; m.dest = 0;
    memcpy(m.guid, h->guid, 16);
    m.cause = cause; m.has_callstate = 1; m.callstate = 0;
    n = aa_tr_build(buf, sizeof(buf), &m);
    if (n <= 0) return -1;
    return send_all(h->fd, buf, (size_t) n);
}

static void hl_close(aa_tr_engine_t *e, hl_t *h, const char *why)
{
    if (h->fd >= 0) {
        close(h->fd);
        h->fd = -1;
        lg(e, 30, "aa_tr: транк %s: служебное соединение закрыто (%s), повтор через %d с", h->name, why, HL_RETRY_MS / 1000);
    }
    h->rn = 0;
    h->next_try = mono_ms() + HL_RETRY_MS;
}

static void hl_read(aa_tr_engine_t *e, hl_t *h)
{
    ssize_t n = recv(h->fd, h->rb + h->rn, sizeof(h->rb) - h->rn, 0);
    if (n <= 0) { hl_close(e, h, n == 0 ? "закрыто АТС" : strerror(errno)); return; }
    h->rn += (size_t) n;
    for (;;) {
        int tl = aa_tr_tpkt_len(h->rb, h->rn);
        aa_tr_msg_t m;
        if (tl < 0) { hl_close(e, h, "не TPKT"); return; }
        if (tl == 0) break;
        if (aa_tr_parse(h->rb, (size_t) tl, &m) == 0 && m.type == AA_TR_STATUS && m.callref == 0) {
            pthread_mutex_lock(&e->mu);
            set_trunk_oos(e, h->name, m.cause);
            pthread_mutex_unlock(&e->mu);
        }
        memmove(h->rb, h->rb + tl, h->rn - (size_t) tl);
        h->rn -= (size_t) tl;
    }
    if (h->rn >= sizeof(h->rb)) h->rn = 0;
}

static void *health_thread(void *arg)
{
    aa_tr_engine_t *e = (aa_tr_engine_t *) arg;
    hl_t hl[AA_TR_MAX_TRUNKS];
    int nh = 0;

    memset(hl, 0, sizeof(hl));
    while (!atomic_load(&e->stop)) {
        aa_tr_trunk_t tr[AA_TR_MAX_TRUNKS];
        int nt, i, k;
        long long now;
        struct pollfd pf[AA_TR_MAX_TRUNKS];
        int pidx[AA_TR_MAX_TRUNKS], np = 0;

        pthread_mutex_lock(&e->mu);
        nt = e->cfg.ntrunk;
        for (i = 0; i < nt; i++) tr[i] = e->cfg.trunk[i];
        pthread_mutex_unlock(&e->mu);

        /* убрать соединения удалённых из конфига транков */
        for (k = 0; k < nh; k++) {
            int found = 0;
            for (i = 0; i < nt; i++) if (!strcmp(tr[i].name, hl[k].name)) found = 1;
            if (!found) { if (hl[k].fd >= 0) close(hl[k].fd); hl[k] = hl[--nh]; k--; }
        }
        now = mono_ms();
        for (i = 0; i < nt; i++) {
            hl_t *h = NULL;
            for (k = 0; k < nh; k++) if (!strcmp(hl[k].name, tr[i].name)) h = &hl[k];
            if (!h) {
                if (nh >= AA_TR_MAX_TRUNKS) continue;
                h = &hl[nh++];
                memset(h, 0, sizeof(*h));
                snprintf(h->name, sizeof(h->name), "%.31s", tr[i].name);
                h->fd = -1;
            }
            if (h->fd < 0 && now >= h->next_try) {
                int fd = -1;
                if (connect_to(e, &tr[i], &fd) == 0) {
                    sock_opts(fd);
                    h->fd = fd; h->rn = 0;
                    aa_tr_guid_random(h->guid);
                    if (hl_send(h, 31) != 0 || hl_send(h, 16) != 0) {
                        hl_close(e, h, "ошибка отправки");
                    } else {
                        h->next_send = now + HL_PERIOD_MS;
                        lg(e, 20, "aa_tr: транк %s: служебное соединение с %s:%d, STATUS 31/16 отправлены", h->name, tr[i].ip, tr[i].port);
                    }
                } else {
                    h->next_try = now + HL_RETRY_MS;
                }
            }
            if (h->fd >= 0 && now >= h->next_send) {
                if (hl_send(h, 16) != 0) hl_close(e, h, "ошибка отправки");
                else h->next_send = now + HL_PERIOD_MS;
            }
        }
        for (k = 0; k < nh; k++) if (hl[k].fd >= 0) { pf[np].fd = hl[k].fd; pf[np].events = POLLIN; pf[np].revents = 0; pidx[np++] = k; }
        if (np == 0) { usleep(200000); continue; }
        if (poll(pf, (nfds_t) np, 200) > 0)
            for (i = 0; i < np; i++)
                if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) hl_read(e, &hl[pidx[i]]);
    }
    for (int k = 0; k < nh; k++) if (hl[k].fd >= 0) close(hl[k].fd);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ */
static int try_listen(aa_tr_engine_t *e, char *err, size_t errcap);

static void *accept_thread(void *arg)
{
    aa_tr_engine_t *e = (aa_tr_engine_t *) arg;
    int waited = 0;
    while (!e->stop) {
        if (e->lfd < 0) {                      /* listener не поднят: пробуем раз в 5 с */
            char le[160] = "";
            usleep(200000);
            if (++waited < 25) continue;
            waited = 0;
            if (try_listen(e, le, sizeof(le)) == 0) {
                lg(e, 20, "aa_tr: listener поднят: слушаю H.225 на %s:%d", e->cfg.local_ip, e->lport);
            } else {
                pthread_mutex_lock(&e->mu);
                snprintf(e->listen_err, sizeof(e->listen_err), "%s", le);
                pthread_mutex_unlock(&e->mu);
            }
            continue;
        }
        struct pollfd p = { e->lfd, POLLIN, 0 };
        int pr = poll(&p, 1, 200);
        if (pr <= 0) continue;
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int fd = accept(e->lfd, (struct sockaddr *) &a, &al);
        if (fd < 0) continue;
        conn_t *c = calloc(1, sizeof(*c));
        if (!c) { close(fd); continue; }
        c->e = e; c->fd = fd; c->incoming = 1; c->state = CS_NEW;
        inet_ntop(AF_INET, &a.sin_addr, c->peer_ip, sizeof(c->peer_ip));
        uuid_str(c->call_id);
        sock_opts(fd);
        pthread_mutex_lock(&e->mu);
        const aa_tr_trunk_t *t = aa_tr_find_trunk_by_ip(&e->cfg, c->peer_ip);
        if (t) { c->trunk = *t; c->have_trunk = 1; }
        if (e->nconn >= MAX_CONNS) {
            pthread_mutex_unlock(&e->mu);
            close(fd); free(c);
            continue;
        }
        c->next = e->conns; e->conns = c; e->nconn++;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 512 * 1024);
        if (pthread_create(&th, &at, conn_thread_in, c) != 0) {
            e->conns = c->next; e->nconn--;
            pthread_mutex_unlock(&e->mu);
            pthread_attr_destroy(&at);
            close(fd); free(c);
            continue;
        }
        pthread_attr_destroy(&at);
        pthread_mutex_unlock(&e->mu);
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ */
aa_tr_engine_t *aa_tr_engine_new(const aa_tr_config_t *cfg, const aa_tr_host_t *host)
{
    aa_tr_engine_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->cfg = *cfg;
    e->host = *host;
    e->lfd = -1;
    signal(SIGPIPE, SIG_IGN);
    return e;
}

/* Открывает listener. 0 — ок. Вызывается из start и (при неудаче) повторно из потока accept. */
static int try_listen(aa_tr_engine_t *e, char *err, size_t errcap)
{
    struct sockaddr_in a;
    int one = 1, fd;
    socklen_t al = sizeof(a);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errcap, "socket: %s", strerror(errno)); return -1; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) e->cfg.h225_port);
    if (e->cfg.local_ip[0]) inet_pton(AF_INET, e->cfg.local_ip, &a.sin_addr);
    if (bind(fd, (struct sockaddr *) &a, sizeof(a)) != 0) {
        snprintf(err, errcap, "bind %s:%d: %s", e->cfg.local_ip, e->cfg.h225_port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        snprintf(err, errcap, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    getsockname(fd, (struct sockaddr *) &a, &al);
    pthread_mutex_lock(&e->mu);
    e->lport = ntohs(a.sin_port);
    e->listen_err[0] = 0;
    pthread_mutex_unlock(&e->mu);
    e->lfd = fd;
    return 0;
}

/* Модуль не должен отказываться загружаться, если порт занят: исходящие вызовы работают и без listener,
 * а listener поднимется сам, когда порт освободится (повтор раз в 5 с). Результат — в aa_tr_engine_listening(). */
int aa_tr_engine_start(aa_tr_engine_t *e, char *err, size_t errcap)
{
    char le[160] = "";

    if (try_listen(e, le, sizeof(le)) == 0) {
        lg(e, 20, "aa_tr: слушаю H.225 на %s:%d", e->cfg.local_ip, e->lport);
    } else {
        pthread_mutex_lock(&e->mu);
        snprintf(e->listen_err, sizeof(e->listen_err), "%s", le);
        pthread_mutex_unlock(&e->mu);
        lg(e, 40, "aa_tr: listener H.225 НЕ запущен (%s) - входящие вызовы не принимаются, повтор каждые 5 с; исходящие работают", le);
    }
    if (pthread_create(&e->acc_th, NULL, accept_thread, e) != 0) {
        snprintf(err, errcap, "pthread_create: %s", strerror(errno));
        if (e->lfd >= 0) { close(e->lfd); e->lfd = -1; }
        return -1;
    }
    e->acc_started = 1;
    if (pthread_create(&e->hl_th, NULL, health_thread, e) == 0) e->hl_started = 1;
    return 0;
}

int aa_tr_engine_listening(aa_tr_engine_t *e)
{
    int r;
    pthread_mutex_lock(&e->mu);
    r = e->listen_err[0] == 0;
    pthread_mutex_unlock(&e->mu);
    return r;
}

int aa_tr_engine_port(aa_tr_engine_t *e) { return e->lport; }

void aa_tr_engine_stop(aa_tr_engine_t *e)
{
    atomic_store(&e->stop, 1);
    if (e->acc_started) { pthread_join(e->acc_th, NULL); e->acc_started = 0; }
    if (e->hl_started) { pthread_join(e->hl_th, NULL); e->hl_started = 0; }
    if (e->lfd >= 0) { close(e->lfd); e->lfd = -1; }
    pthread_mutex_lock(&e->mu);
    for (conn_t *c = e->conns; c; c = c->next) {
        c->kill = 1;
        if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
    }
    while (e->nconn > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&e->cv, &e->mu, &ts);
    }
    pthread_mutex_unlock(&e->mu);
}

void aa_tr_engine_free(aa_tr_engine_t *e)
{
    if (!e) return;
    pthread_mutex_destroy(&e->mu);
    pthread_cond_destroy(&e->cv);
    free(e);
}

void aa_tr_engine_reload(aa_tr_engine_t *e, const aa_tr_config_t *cfg)
{
    pthread_mutex_lock(&e->mu);
    char lip[48]; int lport = e->cfg.h225_port;
    snprintf(lip, sizeof(lip), "%s", e->cfg.local_ip);
    e->cfg = *cfg;
    snprintf(e->cfg.local_ip, sizeof(e->cfg.local_ip), "%s", lip);   /* адрес listener меняется только перезапуском */
    e->cfg.h225_port = lport;
    pthread_mutex_unlock(&e->mu);
}

/* ------------------------------------------------------------------------------------------------ */
static conn_t *find_conn(aa_tr_engine_t *e, const char *call_id)
{
    for (conn_t *c = e->conns; c; c = c->next) if (!strcmp(c->call_id, call_id)) return c;
    return NULL;
}

int aa_tr_originate(aa_tr_engine_t *e, const char *trunk, const char *call_id, const char *calling, const char *calling_name,
                    const char *called, const char *rtp_ip, int rtp_port)
{
    int rc = 0;
    conn_t *c;
    pthread_t th;
    pthread_attr_t at;

    pthread_mutex_lock(&e->mu);
    const aa_tr_trunk_t *t = aa_tr_find_trunk_by_name(&e->cfg, trunk ? trunk : "");
    if (!t) { rc = 3; goto out; }
    {
        tstate_t *ts = ts_get(e, t->name);
        if (ts && ts->oos) { rc = 38; goto out; }
    }
    if (t->lines > 0 && active_calls_locked(e, t->name) >= t->lines) { rc = 34; goto out; }
    if (e->nconn >= MAX_CONNS || !called || !called[0]) { rc = 34; goto out; }
    char called_out[64];
    aa_tr_number_out(t, called, called_out, sizeof(called_out));   /* out_number_rules: номер, уходящий ИЗ FreeSWITCH */
    if (!called_out[0]) { rc = 28; goto out; }
    c = calloc(1, sizeof(*c));
    if (!c) { rc = 41; goto out; }
    c->e = e; c->fd = -1; c->incoming = 0; c->state = CS_OUT_NEW;
    c->trunk = *t; c->have_trunk = 1;
    snprintf(c->call_id, sizeof(c->call_id), "%s", call_id);
    snprintf(c->peer_ip, sizeof(c->peer_ip), "%s", t->ip);
    snprintf(c->o_calling, sizeof(c->o_calling), "%s", calling ? calling : "");
    snprintf(c->o_calling_name, sizeof(c->o_calling_name), "%s", calling_name ? calling_name : "");
    snprintf(c->o_called, sizeof(c->o_called), "%s", called_out);
    snprintf(c->rtp_ip, sizeof(c->rtp_ip), "%s", rtp_ip ? rtp_ip : "");
    c->rtp_port = rtp_port;
    c->codec = codec_bit(t->codec);
    c->callref = rnd_callref();
    aa_tr_guid_random(c->guid);
    c->next = e->conns; e->conns = c; e->nconn++;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    if (pthread_create(&th, &at, conn_thread_out, c) != 0) {
        e->conns = c->next; e->nconn--;
        free(c);
        rc = 41;
    }
    pthread_attr_destroy(&at);
out:
    pthread_mutex_unlock(&e->mu);
    return rc;
}

int aa_tr_answer(aa_tr_engine_t *e, const char *call_id)
{
    int rc = -1;
    pthread_mutex_lock(&e->mu);
    conn_t *c = find_conn(e, call_id);
    if (c && c->incoming && c->state != CS_DONE) {
        if (c->state == CS_ALERTING) {
            rc = send_connect(c);
            c->state = CS_CONNECTED;
        } else if (c->state == CS_PROC || c->state == CS_NEW) {
            c->answer_pending = 1;
            rc = 0;
        } else {
            rc = 0;        /* уже отвечен */
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

int aa_tr_hangup(aa_tr_engine_t *e, const char *call_id, int cause)
{
    int rc = -1;
    pthread_mutex_lock(&e->mu);
    conn_t *c = find_conn(e, call_id);
    if (c && c->state != CS_DONE) {
        if (cause <= 0) cause = 16;
        if (c->fd < 0 || c->state == CS_OUT_NEW || (c->incoming && c->state == CS_NEW) || (c->incoming && c->state == CS_PROC)) {
            /* SETUP ещё не обработан/не отправлен: поток сам закроет соединение */
            c->kill = 1; c->kill_cause = cause;
            c->notified = 1;
            rc = 0;
        } else {
            if (c->f_tcs_sent || c->f_tcs_rcv) send_pdu(c, gen_end);
            send_rc(c, cause);
            c->state = CS_DONE;
            c->notified = 1;
            shutdown(c->fd, SHUT_WR);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

int aa_tr_dtmf(aa_tr_engine_t *e, const char *call_id, char ch)
{
    int rc = -1;
    pthread_mutex_lock(&e->mu);
    conn_t *c = find_conn(e, call_id);
    if (c && c->state == CS_CONNECTED && c->fd >= 0) {
        aa_tr_msg_t m;
        mk(c, &m, AA_TR_FACILITY);
        m.pdu_n = aa_tr_pdu_uii(m.pdu, ch);
        rc = send_msg(c, &m);
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ */
int aa_tr_active_calls(aa_tr_engine_t *e, const char *trunk)
{
    int n;
    pthread_mutex_lock(&e->mu);
    n = aa_tr_find_trunk_by_name(&e->cfg, trunk) ? active_calls_locked(e, trunk) : -1;
    pthread_mutex_unlock(&e->mu);
    return n;
}

int aa_tr_trunk_in_service(aa_tr_engine_t *e, const char *trunk)
{
    int r = 1;
    pthread_mutex_lock(&e->mu);
    for (int i = 0; i < e->nts; i++) if (!strcmp(e->ts[i].name, trunk)) r = !e->ts[i].oos;
    pthread_mutex_unlock(&e->mu);
    return r;
}

char *aa_tr_status_text(aa_tr_engine_t *e)
{
    size_t cap = 4096 + (size_t) e->cfg.ntrunk * 256;
    char *b = malloc(cap);
    size_t o = 0;
    if (!b) return NULL;
    pthread_mutex_lock(&e->mu);
    if (e->listen_err[0])
        o += (size_t) snprintf(b + o, cap - o, "H.225 listener: NOT LISTENING (%s) - retrying every 5 s; outgoing calls work\n", e->listen_err);
    o += (size_t) snprintf(b + o, cap - o, "H.225 listener: %s:%d, calls in/out/rejected: %lu/%lu/%lu, active connections: %d\n", e->cfg.local_ip,
                           e->lport, e->n_in, e->n_out, e->n_rej, e->nconn);
    for (int i = 0; i < e->cfg.ntrunk && o + 256 < cap; i++) {
        const aa_tr_trunk_t *t = &e->cfg.trunk[i];
        int oos = 0;
        for (int k = 0; k < e->nts; k++) if (!strcmp(e->ts[k].name, t->name)) oos = e->ts[k].oos;
        o += (size_t) snprintf(b + o, cap - o, "trunk %s  %s:%d  codec=%s  lines=%d  active=%d  %s\n", t->name, t->ip, t->port, t->codec, t->lines,
                               active_calls_locked(e, t->name), oos ? "OUT OF SERVICE (far end STATUS 47)" : "in service");
    }
    pthread_mutex_unlock(&e->mu);
    return b;
}
