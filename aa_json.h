/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * aa_json.h — минимальный JSON-разбор (DOM) для config.json модуля. Без внешних зависимостей.
 * Поведение приближено к Python json.load (strict): порядок ключей сохраняется, повтор ключа заменяет
 * значение на месте, управляющие символы в строках и BOM — ошибка; NaN/Infinity принимаются (как json.load).
 * Копирайт: ИП Сенашенко Дмитрий Васильевич, 2026. Все права защищены.
 */
#ifndef AA_JSON_H
#define AA_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AA_J_NULL, AA_J_BOOL, AA_J_NUM, AA_J_STR, AA_J_ARR, AA_J_OBJ } aa_jtype_t;

typedef struct aa_json aa_json_t;
struct aa_json {
    aa_jtype_t type;
    int b;                  /* BOOL */
    double num;             /* NUM */
    int is_int;             /* NUM: целое без точки/экспоненты */
    long long ival;         /* NUM, если is_int и помещается */
    char *str;              /* STR: UTF-8; NUM: исходный текст */
    size_t n;               /* ARR/OBJ: число элементов */
    aa_json_t **items;      /* ARR/OBJ: значения */
    char **keys;            /* OBJ: ключи (порядок вставки) */
};

/* NULL при ошибке; err (если задан) — сообщение. */
aa_json_t *aa_json_parse(const char *text, size_t n, char *err, size_t errcap);
void aa_json_free(aa_json_t *j);
/* Член объекта по ключу (NULL, если нет или j не объект). */
const aa_json_t *aa_json_member(const aa_json_t *j, const char *key);
/* Python-подобная «истинность» (для `x or default`): null/false/0/""/[]/{} — ложь. */
int aa_json_truthy(const aa_json_t *j);
/* str(value) для скаляров как в Python (True/False/None, число как есть). Возвращает длину, пишет в buf. */
size_t aa_json_to_str(const aa_json_t *j, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
