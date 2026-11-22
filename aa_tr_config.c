/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * aa_tr_config.c — разбор config.json модуля mod_aa_h323_tr. Свободный пакет. Автор: Сенашенко Д.В., 2026.
 */
#include "aa_tr_config.h"
#include "aa_json.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void seterr(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void seterr(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || !cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, cap, fmt, ap); va_end(ap);
}

static void cpy(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s ? s : ""); }

/* строка-член; NULL, если нет или не строка */
static const char *jstr(const aa_json_t *o, const char *k)
{
    const aa_json_t *m = aa_json_member(o, k);
    return (m && m->type == AA_J_STR) ? m->str : NULL;
}

/* целое-член: 0 — нет ключа, 1 — есть (в *v), -1 — есть, но не целое */
static int jint(const aa_json_t *o, const char *k, long long *v)
{
    const aa_json_t *m = aa_json_member(o, k);
    if (!m) return 0;
    if (m->type == AA_J_NUM && m->is_int) { *v = m->ival; return 1; }
    if (m->type == AA_J_STR && m->str[0]) {
        char *e; long long x = strtoll(m->str, &e, 10);
        if (*e == 0) { *v = x; return 1; }
    }
    return -1;
}

static int digits_ok(const char *s)    /* цифры номера: 0-9 * # A-D + */
{
    for (; *s; s++) if (!(isdigit((unsigned char) *s) || strchr("*#ABCD+", *s))) return 0;
    return 1;
}

static int ipv4_ok(const char *s)
{
    struct in_addr a;
    return s && s[0] && inet_pton(AF_INET, s, &a) == 1;
}

static int parse_trunk(const aa_json_t *j, int idx, aa_tr_trunk_t *t, char *err, size_t ec)
{
    memset(t, 0, sizeof(*t));
    if (j->type != AA_J_OBJ) { seterr(err, ec, "trunk[%d]: ожидался объект", idx); return -1; }
    const char *s = jstr(j, "name");
    if (s && s[0]) {
        if (strlen(s) >= sizeof(t->name)) { seterr(err, ec, "trunk[%d]: name слишком длинное", idx); return -1; }
        for (const char *p = s; *p; p++)
            if (!(isalnum((unsigned char) *p) || *p == '_' || *p == '-')) { seterr(err, ec, "trunk[%d]: name допускает только буквы, цифры, _ и -", idx); return -1; }
        cpy(t->name, sizeof(t->name), s);
    } else snprintf(t->name, sizeof(t->name), "trunk%d", idx + 1);
    s = jstr(j, "ip");
    if (!ipv4_ok(s)) { seterr(err, ec, "trunk '%s': нужен ключ ip (IPv4 дальней стороны)", t->name); return -1; }
    cpy(t->ip, sizeof(t->ip), s);
    long long v;
    t->port = AA_TR_DEF_PEER_PORT;
    int r = jint(j, "port", &v);
    if (r < 0 || (r == 1 && (v < 1 || v > 65535))) { seterr(err, ec, "trunk '%s': port должен быть 1..65535", t->name); return -1; }
    if (r == 1) t->port = (int) v;
    s = jstr(j, "codec");
    cpy(t->codec, sizeof(t->codec), "PCMA");
    if (s && s[0]) {
        char u[8]; size_t i = 0;
        for (; s[i] && i < sizeof(u) - 1; i++) u[i] = (char) toupper((unsigned char) s[i]);
        u[i] = 0;
        if (strcmp(u, "PCMA") && strcmp(u, "PCMU")) { seterr(err, ec, "trunk '%s': codec должен быть PCMA или PCMU", t->name); return -1; }
        cpy(t->codec, sizeof(t->codec), u);
    }
    t->lines = AA_TR_DEF_LINES;
    r = jint(j, "lines", &v);
    if (r < 0 || (r == 1 && (v < 0 || v > 100000))) { seterr(err, ec, "trunk '%s': lines должно быть 0..100000 (0 = без ограничения)", t->name); return -1; }
    if (r == 1) t->lines = (int) v;
    const aa_json_t *rl = aa_json_member(j, "in_number_rules");
    if (rl) {
        if (rl->type != AA_J_ARR) { seterr(err, ec, "trunk '%s': in_number_rules должен быть массивом", t->name); return -1; }
        if (rl->n > AA_TR_MAX_RULES) { seterr(err, ec, "trunk '%s': правил больше %d", t->name, AA_TR_MAX_RULES); return -1; }
        for (size_t i = 0; i < rl->n; i++) {
            const aa_json_t *o = rl->items[i];
            aa_tr_rule_t *ru = &t->rules[t->nrules];
            if (o->type != AA_J_OBJ) { seterr(err, ec, "trunk '%s': правило %zu не объект", t->name, i + 1); return -1; }
            const char *m = jstr(o, "match"), *a = jstr(o, "add");
            if ((m && (strlen(m) >= sizeof(ru->match) || !digits_ok(m))) || (a && (strlen(a) >= sizeof(ru->add) || !digits_ok(a)))) {
                seterr(err, ec, "trunk '%s': правило %zu: match/add - только цифры и * # A-D +", t->name, i + 1); return -1;
            }
            cpy(ru->match, sizeof(ru->match), m); cpy(ru->add, sizeof(ru->add), a);
            r = jint(o, "strip", &v);
            if (r < 0 || (r == 1 && (v < 0 || v > 32))) { seterr(err, ec, "trunk '%s': правило %zu: strip 0..32", t->name, i + 1); return -1; }
            ru->strip = (r == 1) ? (int) v : 0;
            t->nrules++;
        }
    }
    return 0;
}

int aa_tr_config_parse(const char *text, size_t n, aa_tr_config_t *c, char *err, size_t ec)
{
    memset(c, 0, sizeof(*c));
    char jerr[160] = "";
    aa_json_t *root = aa_json_parse(text, n, jerr, sizeof(jerr));
    if (!root) { seterr(err, ec, "JSON: %s", jerr); return -1; }
    int rc = -1;
    if (root->type != AA_J_OBJ) { seterr(err, ec, "корень config.json должен быть объектом"); goto out; }
    c->h225_port = AA_TR_DEF_LOCAL_PORT;
    const aa_json_t *loc = aa_json_member(root, "local");
    if (loc) {
        const char *ip = jstr(loc, "ip");
        if (ip && ip[0]) {
            if (!ipv4_ok(ip)) { seterr(err, ec, "local.ip: не IPv4 адрес"); goto out; }
            cpy(c->local_ip, sizeof(c->local_ip), ip);
        }
        long long v; int r = jint(loc, "h225_port", &v);
        if (r < 0 || (r == 1 && (v < 1 || v > 65535))) { seterr(err, ec, "local.h225_port должен быть 1..65535"); goto out; }
        if (r == 1) c->h225_port = (int) v;
    }
    cpy(c->product, sizeof(c->product), "Avaya Multivantage");
    cpy(c->version, sizeof(c->version), "Adapter_3.5.8");
    cpy(c->display, sizeof(c->display), "FreeSWITCH");
    const aa_json_t *id = aa_json_member(root, "identity");
    if (id) {
        const char *s;
        if ((s = jstr(id, "product")) && s[0]) { if (strlen(s) >= sizeof(c->product)) { seterr(err, ec, "identity.product слишком длинный"); goto out; } cpy(c->product, sizeof(c->product), s); }
        if ((s = jstr(id, "version")) && s[0]) { if (strlen(s) >= sizeof(c->version)) { seterr(err, ec, "identity.version слишком длинный"); goto out; } cpy(c->version, sizeof(c->version), s); }
        if ((s = jstr(id, "display"))) { if (strlen(s) >= sizeof(c->display)) { seterr(err, ec, "identity.display слишком длинный"); goto out; } cpy(c->display, sizeof(c->display), s); }
    }
    cpy(c->log_level, sizeof(c->log_level), "INFO");
    const aa_json_t *lg = aa_json_member(root, "logging");
    if (lg) {
        const char *s;
        if ((s = jstr(lg, "level")) && s[0]) cpy(c->log_level, sizeof(c->log_level), s);
        if ((s = jstr(lg, "file"))) cpy(c->log_file, sizeof(c->log_file), s);
    }
    const aa_json_t *tr = aa_json_member(root, "trunk");
    if (!tr) { seterr(err, ec, "нет блока \"trunk\" (массив транков)"); goto out; }
    if (tr->type != AA_J_ARR) { seterr(err, ec, "\"trunk\" должен быть массивом"); goto out; }
    if (tr->n > AA_TR_MAX_TRUNKS) { seterr(err, ec, "транков больше %d", AA_TR_MAX_TRUNKS); goto out; }
    for (size_t i = 0; i < tr->n; i++) {
        if (parse_trunk(tr->items[i], (int) i, &c->trunk[c->ntrunk], err, ec) != 0) goto out;
        for (int k = 0; k < c->ntrunk; k++) {
            if (!strcmp(c->trunk[k].name, c->trunk[c->ntrunk].name)) { seterr(err, ec, "имя транка '%s' повторяется", c->trunk[k].name); goto out; }
            if (!strcmp(c->trunk[k].ip, c->trunk[c->ntrunk].ip)) { seterr(err, ec, "транки '%s' и '%s' с одним ip %s: входящий вызов нельзя отнести к транку", c->trunk[k].name, c->trunk[c->ntrunk].name, c->trunk[k].ip); goto out; }
        }
        c->ntrunk++;
    }
    rc = 0;
out:
    aa_json_free(root);
    return rc;
}

int aa_tr_config_load(const char *path, aa_tr_config_t *c, char *err, size_t ec)
{
    FILE *f = fopen(path, "rb");
    if (!f) { seterr(err, ec, "не открыть %s", path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (4 << 20)) { fclose(f); seterr(err, ec, "%s: странный размер", path); return -1; }
    char *buf = malloc((size_t) n + 1);
    if (!buf) { fclose(f); seterr(err, ec, "нет памяти"); return -1; }
    size_t got = fread(buf, 1, (size_t) n, f); fclose(f);
    int r = aa_tr_config_parse(buf, got, c, err, ec);
    free(buf);
    return r;
}

int aa_tr_number_in(const aa_tr_trunk_t *t, const char *num, char *out, size_t cap)
{
    size_t nl = strlen(num);
    for (int i = 0; i < t->nrules; i++) {
        const aa_tr_rule_t *r = &t->rules[i];
        size_t ml = strlen(r->match);
        if (ml && strncmp(num, r->match, ml) != 0) continue;
        if ((size_t) r->strip > nl) continue;          /* номер короче, чем надо убрать — правило не подходит */
        snprintf(out, cap, "%s%s", r->add, num + r->strip);
        return 1;
    }
    snprintf(out, cap, "%s", num);
    return 0;
}

const aa_tr_trunk_t *aa_tr_find_trunk_by_ip(const aa_tr_config_t *c, const char *ip)
{
    for (int i = 0; i < c->ntrunk; i++) if (!strcmp(c->trunk[i].ip, ip)) return &c->trunk[i];
    return NULL;
}

const aa_tr_trunk_t *aa_tr_find_trunk_by_name(const aa_tr_config_t *c, const char *name)
{
    for (int i = 0; i < c->ntrunk; i++) if (!strcmp(c->trunk[i].name, name)) return &c->trunk[i];
    return NULL;
}
