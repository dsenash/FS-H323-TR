/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * aa_tr_config.h — разбор config.json модуля mod_aa_h323_tr (чистый C, без FreeSWITCH).
 * Свободный пакет. Автор: Сенашенко Д.В., 2026.
 */
#ifndef AA_TR_CONFIG_H
#define AA_TR_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AA_TR_MAX_TRUNKS 32
#define AA_TR_MAX_RULES 16
#define AA_TR_DEF_LOCAL_PORT 1722     /* порт приёма вызовов от АТС, если не задан local.h225_port */
#define AA_TR_DEF_PEER_PORT 1720      /* порт дальней стороны, если не задан trunk[].port */
#define AA_TR_DEF_LINES 100

typedef struct {
    char match[32];     /* "" — любой номер */
    int strip;          /* сколько цифр убрать в начале */
    char add[32];       /* что дописать вместо убранных */
} aa_tr_rule_t;

typedef struct {
    char name[32];
    char ip[48];        /* IPv4 дальней стороны */
    int port;
    char codec[8];      /* "PCMA" | "PCMU" */
    int lines;          /* 0 — без ограничения */
    int nrules;
    aa_tr_rule_t rules[AA_TR_MAX_RULES];          /* in_number_rules: номер, ПРИШЕДШИЙ от АТС в FreeSWITCH */
    int nout_rules;
    aa_tr_rule_t out_rules[AA_TR_MAX_RULES];      /* out_number_rules: номер, УХОДЯЩИЙ из FreeSWITCH в АТС */
} aa_tr_trunk_t;

typedef struct {
    char local_ip[48];
    int h225_port;
    char product[64], version[64], display[48];
    char log_level[16], log_file[200];
    int ntrunk;
    aa_tr_trunk_t trunk[AA_TR_MAX_TRUNKS];
} aa_tr_config_t;

/* 0 — ок; -1 — ошибка (текст в err). */
int aa_tr_config_parse(const char *text, size_t n, aa_tr_config_t *c, char *err, size_t errcap);
int aa_tr_config_load(const char *path, aa_tr_config_t *c, char *err, size_t errcap);

/* Входящий номер АТС -> внутренний номер по правилам транка. Возвращает 1, если сработало правило; 0 — номер не менялся.
 * out всегда заполняется (при нехватке места усекается). */
int aa_tr_number_in(const aa_tr_trunk_t *t, const char *num, char *out, size_t cap);
int aa_tr_number_out(const aa_tr_trunk_t *t, const char *num, char *out, size_t cap);

/* Транк по IP источника входящего соединения (IPv4, текстом); NULL — нет. */
const aa_tr_trunk_t *aa_tr_find_trunk_by_ip(const aa_tr_config_t *c, const char *ip);
const aa_tr_trunk_t *aa_tr_find_trunk_by_name(const aa_tr_config_t *c, const char *name);

#ifdef __cplusplus
}
#endif
#endif
