/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* aa_json.c — см. aa_json.h */
#include "aa_json.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *s; size_t n, i; char *err; size_t errcap; int bad; int depth; } P;

static void fail(P *p, const char *msg)
{
    if (!p->bad && p->err && p->errcap) snprintf(p->err, p->errcap, "%s (позиция %zu)", msg, p->i);
    p->bad = 1;
}

static void ws(P *p) { while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++; }

static aa_json_t *mk(aa_jtype_t t)
{
    aa_json_t *j = calloc(1, sizeof *j);
    if (j) j->type = t;
    return j;
}

void aa_json_free(aa_json_t *j)
{
    if (!j) return;
    for (size_t i = 0; i < j->n; i++) { aa_json_free(j->items ? j->items[i] : NULL); if (j->keys) free(j->keys[i]); }
    free(j->items); free(j->keys); free(j->str); free(j);
}

static int put_utf8(char *o, unsigned long cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex4(P *p, unsigned long *v)
{
    if (p->i + 4 > p->n) return 0;
    unsigned long r = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[p->i + (size_t)k];
        if (!isxdigit((unsigned char)c)) return 0;
        r = r * 16 + (unsigned long)(c <= '9' ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
    }
    p->i += 4; *v = r;
    return 1;
}

static char *parse_string(P *p)
{
    /* p->s[p->i] == '"' */
    p->i++;
    char *buf = malloc(p->n - p->i + 1);
    size_t w = 0;
    if (!buf) { fail(p, "нет памяти"); return NULL; }
    for (;;) {
        if (p->i >= p->n) { fail(p, "Unterminated string"); free(buf); return NULL; }
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') break;
        if (c < 0x20) { fail(p, "Invalid control character in string"); free(buf); return NULL; }
        if (c != '\\') { buf[w++] = (char)c; continue; }
        if (p->i >= p->n) { fail(p, "Unterminated string"); free(buf); return NULL; }
        char e = p->s[p->i++];
        switch (e) {
        case '"': buf[w++] = '"'; break;
        case '\\': buf[w++] = '\\'; break;
        case '/': buf[w++] = '/'; break;
        case 'b': buf[w++] = '\b'; break;
        case 'f': buf[w++] = '\f'; break;
        case 'n': buf[w++] = '\n'; break;
        case 'r': buf[w++] = '\r'; break;
        case 't': buf[w++] = '\t'; break;
        case 'u': {
            unsigned long cp;
            if (!hex4(p, &cp)) { fail(p, "Invalid \\uXXXX escape"); free(buf); return NULL; }
            if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 6 <= p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                size_t save = p->i; unsigned long lo;
                p->i += 2;
                if (hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else p->i = save;
            }
            char t[4]; int k = put_utf8(t, cp);   /* одиночный суррогат кодируется как есть (3 байта) */
            memcpy(buf + w, t, (size_t)k); w += (size_t)k;
            break;
        }
        default: fail(p, "Invalid \\escape"); free(buf); return NULL;
        }
    }
    buf[w] = 0;
    return buf;
}

static aa_json_t *parse_value(P *p);

static int add(aa_json_t *o, char *key, aa_json_t *v)
{
    if (o->type == AA_J_OBJ) {
        for (size_t i = 0; i < o->n; i++)
            if (strcmp(o->keys[i], key) == 0) { aa_json_free(o->items[i]); o->items[i] = v; free(key); return 1; }
    }
    aa_json_t **ni = realloc(o->items, (o->n + 1) * sizeof *ni);
    if (!ni) return 0;
    o->items = ni;
    if (o->type == AA_J_OBJ) {
        char **nk = realloc(o->keys, (o->n + 1) * sizeof *nk);
        if (!nk) return 0;
        o->keys = nk; o->keys[o->n] = key;
    }
    o->items[o->n++] = v;
    return 1;
}

static aa_json_t *parse_value(P *p)
{
    if (++p->depth > 100) { fail(p, "слишком глубокая вложенность"); return NULL; }
    ws(p);
    aa_json_t *r = NULL;
    if (p->i >= p->n) { fail(p, "Expecting value"); goto out; }
    char c = p->s[p->i];
    if (c == '{') {
        r = mk(AA_J_OBJ); p->i++; ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; goto out; }
        for (;;) {
            ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') { fail(p, "Expecting property name enclosed in double quotes"); break; }
            char *k = parse_string(p); if (!k) break;
            ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') { fail(p, "Expecting ':' delimiter"); free(k); break; }
            p->i++;
            aa_json_t *v = parse_value(p);
            if (!v) { free(k); break; }
            if (!add(r, k, v)) { fail(p, "нет памяти"); aa_json_free(v); free(k); break; }
            ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; goto out; }
            fail(p, "Expecting ',' delimiter"); break;
        }
        aa_json_free(r); r = NULL; goto out;
    }
    if (c == '[') {
        r = mk(AA_J_ARR); p->i++; ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; goto out; }
        for (;;) {
            aa_json_t *v = parse_value(p);
            if (!v) break;
            if (!add(r, NULL, v)) { fail(p, "нет памяти"); aa_json_free(v); break; }
            ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; goto out; }
            fail(p, "Expecting ',' delimiter"); break;
        }
        aa_json_free(r); r = NULL; goto out;
    }
    if (c == '"') {
        char *s = parse_string(p);
        if (!s) goto out;
        r = mk(AA_J_STR); if (r) r->str = s; else free(s);
        goto out;
    }
    if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "true", 4)) { p->i += 4; r = mk(AA_J_BOOL); if (r) r->b = 1; goto out; }
    if (p->n - p->i >= 5 && !strncmp(p->s + p->i, "false", 5)) { p->i += 5; r = mk(AA_J_BOOL); goto out; }
    if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "null", 4)) { p->i += 4; r = mk(AA_J_NULL); goto out; }
    {   /* Python json.load принимает NaN/Infinity/-Infinity; str(float) даёт nan/inf/-inf */
        static const struct { const char *lit; const char *py; double v; } K[] = {
            {"NaN", "nan", 0.0 / 0.0}, {"Infinity", "inf", 1.0 / 0.0}, {"-Infinity", "-inf", -1.0 / 0.0}};
        for (size_t k = 0; k < 3; k++) {
            size_t L = strlen(K[k].lit);
            if (p->n - p->i >= L && !strncmp(p->s + p->i, K[k].lit, L)) {
                p->i += L; r = mk(AA_J_NUM);
                if (r) { r->num = K[k].v; r->str = strdup(K[k].py); }
                goto out;
            }
        }
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t s = p->i, i = p->i; int isint = 1;
        if (p->s[i] == '-') i++;
        if (i >= p->n || !isdigit((unsigned char)p->s[i])) { fail(p, "Expecting value"); goto out; }
        if (p->s[i] == '0') i++; else while (i < p->n && isdigit((unsigned char)p->s[i])) i++;
        if (i + 1 < p->n && p->s[i] == '.' && isdigit((unsigned char)p->s[i + 1])) {
            isint = 0; i++; while (i < p->n && isdigit((unsigned char)p->s[i])) i++;
        }
        if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E')) {
            size_t k = i + 1;
            if (k < p->n && (p->s[k] == '+' || p->s[k] == '-')) k++;
            if (k < p->n && isdigit((unsigned char)p->s[k])) { isint = 0; i = k; while (i < p->n && isdigit((unsigned char)p->s[i])) i++; }
        }
        r = mk(AA_J_NUM); if (!r) goto out;
        r->str = malloc(i - s + 1);
        if (!r->str) { free(r); r = NULL; goto out; }
        memcpy(r->str, p->s + s, i - s); r->str[i - s] = 0;
        r->num = strtod(r->str, NULL);
        if (isint) { errno = 0; r->ival = strtoll(r->str, NULL, 10); r->is_int = (errno == 0); }
        p->i = i;
        goto out;
    }
    fail(p, "Expecting value");
out:
    p->depth--;
    return r;
}

aa_json_t *aa_json_parse(const char *text, size_t n, char *err, size_t errcap)
{
    P p = { text, n, 0, err, errcap, 0, 0 };
    if (err && errcap) err[0] = 0;
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        if (err && errcap) snprintf(err, errcap, "Unexpected UTF-8 BOM");
        return NULL;
    }
    aa_json_t *j = parse_value(&p);
    if (!j) { if (!p.bad && err && errcap) snprintf(err, errcap, "ошибка разбора"); return NULL; }
    ws(&p);
    if (p.i != p.n) { fail(&p, "Extra data"); aa_json_free(j); return NULL; }
    return j;
}

const aa_json_t *aa_json_member(const aa_json_t *j, const char *key)
{
    if (!j || j->type != AA_J_OBJ) return NULL;
    for (size_t i = 0; i < j->n; i++) if (strcmp(j->keys[i], key) == 0) return j->items[i];
    return NULL;
}

int aa_json_truthy(const aa_json_t *j)
{
    if (!j) return 0;
    switch (j->type) {
    case AA_J_NULL: return 0;
    case AA_J_BOOL: return j->b;
    case AA_J_NUM: return j->num != 0.0;
    case AA_J_STR: return j->str && j->str[0];
    default: return j->n > 0;
    }
}

size_t aa_json_to_str(const aa_json_t *j, char *buf, size_t cap)
{
    const char *s = "";
    if (!cap) return 0;
    if (j) switch (j->type) {
        case AA_J_NULL: s = "None"; break;
        case AA_J_BOOL: s = j->b ? "True" : "False"; break;
        case AA_J_NUM: case AA_J_STR: s = j->str ? j->str : ""; break;
        default: s = ""; break;
    }
    snprintf(buf, cap, "%s", s);
    return strlen(buf);
}
