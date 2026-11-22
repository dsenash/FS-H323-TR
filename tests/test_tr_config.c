/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* test_tr_config.c — config.json транка: пример из config/, правила номеров, ошибки. */
#include <stdio.h>
#include <string.h>
#include "aa_tr_config.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int parse(const char *s, aa_tr_config_t *c, char *err) { return aa_tr_config_parse(s, strlen(s), c, err, 200); }

int main(void)
{
    aa_tr_config_t c; char err[200] = "", o[64];
    CHECK(aa_tr_config_load("config/aa_h323_tr.example.json", &c, err, sizeof(err)) == 0, "example: %s", err);
    CHECK(c.h225_port == 1722 && !strcmp(c.local_ip, "192.168.0.6") && c.ntrunk == 1, "example fields");
    const aa_tr_trunk_t *t = &c.trunk[0];
    CHECK(!strcmp(t->name, "cm1") && !strcmp(t->ip, "192.168.0.193") && t->port == 1720 && !strcmp(t->codec, "PCMA") && t->lines == 100 && t->nrules == 1, "trunk fields");
    CHECK(aa_tr_number_in(t, "111123", o, sizeof(o)) == 1 && !strcmp(o, "0123"), "111123 -> %s", o);
    CHECK(aa_tr_number_in(t, "222123", o, sizeof(o)) == 0 && !strcmp(o, "222123"), "no match unchanged");
    CHECK(aa_tr_number_in(t, "111", o, sizeof(o)) == 1 && !strcmp(o, "0"), "111 -> 0");
    CHECK(aa_tr_find_trunk_by_ip(&c, "192.168.0.193") == t && !aa_tr_find_trunk_by_ip(&c, "10.0.0.1") && aa_tr_find_trunk_by_name(&c, "cm1") == t, "find trunk");

    /* значения по умолчанию */
    CHECK(parse("{\"trunk\":[{\"ip\":\"10.1.1.1\"}]}", &c, err) == 0 && c.h225_port == 1722 && c.trunk[0].port == 1720 &&
          !strcmp(c.trunk[0].codec, "PCMA") && c.trunk[0].lines == 100 && !strcmp(c.trunk[0].name, "trunk1") && c.trunk[0].nrules == 0, "defaults: %s", err);
    /* порт слушателя необязателен, но если задан — берётся */
    CHECK(parse("{\"local\":{\"h225_port\":1730},\"trunk\":[{\"ip\":\"10.1.1.1\",\"port\":\"1721\",\"codec\":\"pcmu\",\"lines\":0}]}", &c, err) == 0 &&
          c.h225_port == 1730 && c.trunk[0].port == 1721 && !strcmp(c.trunk[0].codec, "PCMU") && c.trunk[0].lines == 0, "overrides: %s", err);
    /* правила: порядок, match пустой, strip длиннее номера */
    CHECK(parse("{\"trunk\":[{\"ip\":\"10.1.1.1\",\"in_number_rules\":[{\"match\":\"8\",\"strip\":1,\"add\":\"7\"},{\"match\":\"\",\"strip\":2,\"add\":\"9\"},{\"strip\":0,\"add\":\"*\"}]}]}", &c, err) == 0, "rules parse: %s", err);
    t = &c.trunk[0];
    aa_tr_number_in(t, "89001112233", o, sizeof(o)); CHECK(!strcmp(o, "79001112233"), "rule1 %s", o);
    aa_tr_number_in(t, "4567", o, sizeof(o)); CHECK(!strcmp(o, "967"), "rule2 any %s", o);
    aa_tr_number_in(t, "5", o, sizeof(o)); CHECK(!strcmp(o, "*5"), "rule3 when strip>len skipped %s", o);
    aa_tr_number_in(t, "12345", o, 4); CHECK(!strcmp(o, "934"), "truncation %s", o);
    /* ошибки */
    const char *bad[] = {
        "{}", "{\"trunk\":{}}", "{\"trunk\":[{}]}", "{\"trunk\":[{\"ip\":\"999.1.1.1\"}]}", "{\"trunk\":[{\"ip\":\"10.1.1.1\",\"port\":0}]}",
        "{\"trunk\":[{\"ip\":\"10.1.1.1\",\"codec\":\"G729\"}]}", "{\"trunk\":[{\"ip\":\"10.1.1.1\",\"lines\":-1}]}",
        "{\"trunk\":[{\"ip\":\"10.1.1.1\",\"in_number_rules\":[{\"strip\":-1}]}]}", "{\"trunk\":[{\"ip\":\"10.1.1.1\",\"in_number_rules\":[{\"add\":\"x1\"}]}]}",
        "{\"trunk\":[{\"name\":\"a\",\"ip\":\"10.1.1.1\"},{\"name\":\"a\",\"ip\":\"10.1.1.2\"}]}", "{\"trunk\":[{\"ip\":\"10.1.1.1\"},{\"ip\":\"10.1.1.1\"}]}",
        "{\"local\":{\"h225_port\":70000},\"trunk\":[]}", "{\"trunk\":[{\"name\":\"a b\",\"ip\":\"10.1.1.1\"}]}", "{not json", NULL };
    for (int i = 0; bad[i]; i++) { err[0] = 0; CHECK(parse(bad[i], &c, err) != 0 && err[0], "bad[%d] принято: %s", i, bad[i]); }
    CHECK(parse("{\"trunk\":[]}", &c, err) == 0 && c.ntrunk == 0, "empty trunk array ok");
    printf("test_tr_config: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
