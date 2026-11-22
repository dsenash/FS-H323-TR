/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "aa_smap.h"
#include <stdlib.h>
#include <string.h>

typedef struct node { char *key; void *val; struct node *next; } node_t;
struct aa_smap { node_t **b; size_t nb, n; };

static size_t hash(const char *s)
{
    size_t h = 1469598103934665603ULL % ((size_t)-1);
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

aa_smap_t *aa_smap_new(void)
{
    aa_smap_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->nb = 64; m->b = calloc(m->nb, sizeof *m->b);
    if (!m->b) { free(m); return NULL; }
    return m;
}

void aa_smap_free(aa_smap_t *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->nb; i++) for (node_t *n = m->b[i], *nx; n; n = nx) { nx = n->next; free(n->key); free(n); }
    free(m->b); free(m);
}

static node_t *find(const aa_smap_t *m, const char *k)
{
    for (node_t *n = m->b[hash(k) % m->nb]; n; n = n->next) if (!strcmp(n->key, k)) return n;
    return NULL;
}

void *aa_smap_get(const aa_smap_t *m, const char *k) { node_t *n = find(m, k); return n ? n->val : NULL; }
int aa_smap_has(const aa_smap_t *m, const char *k) { return find(m, k) != NULL; }

static int grow(aa_smap_t *m)
{
    size_t nb = m->nb * 2;
    node_t **b = calloc(nb, sizeof *b);
    if (!b) return -1;
    for (size_t i = 0; i < m->nb; i++) for (node_t *n = m->b[i], *nx; n; n = nx) { nx = n->next; size_t h = hash(n->key) % nb; n->next = b[h]; b[h] = n; }
    free(m->b); m->b = b; m->nb = nb;
    return 0;
}

int aa_smap_set(aa_smap_t *m, const char *k, void *v)
{
    node_t *n = find(m, k);
    if (n) { n->val = v; return 0; }
    if (m->n >= m->nb * 2 && grow(m) < 0) return -1;
    n = malloc(sizeof *n);
    if (!n) return -1;
    n->key = strdup(k);
    if (!n->key) { free(n); return -1; }
    n->val = v; size_t h = hash(k) % m->nb; n->next = m->b[h]; m->b[h] = n; m->n++;
    return 0;
}

void *aa_smap_del(aa_smap_t *m, const char *k)
{
    node_t **pp = &m->b[hash(k) % m->nb];
    for (; *pp; pp = &(*pp)->next) if (!strcmp((*pp)->key, k)) {
        node_t *n = *pp; void *v = n->val; *pp = n->next; free(n->key); free(n); m->n--; return v;
    }
    return NULL;
}

size_t aa_smap_count(const aa_smap_t *m) { return m->n; }

void aa_smap_foreach(const aa_smap_t *m, int (*cb)(const char *, void *, void *), void *ctx)
{
    for (size_t i = 0; i < m->nb; i++) for (node_t *n = m->b[i]; n; n = n->next) if (cb(n->key, n->val, ctx)) return;
}
