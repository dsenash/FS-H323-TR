/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* aa_smap.h — небольшая хеш-таблица «строка -> указатель» (цепочки). Без блокировок: защита — на вызывающем.
 * Копирайт: ИП Сенашенко Дмитрий Васильевич, 2026. Все права защищены. */
#ifndef AA_SMAP_H
#define AA_SMAP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct aa_smap aa_smap_t;
aa_smap_t *aa_smap_new(void);
void aa_smap_free(aa_smap_t *m);                         /* значения не освобождает */
void *aa_smap_get(const aa_smap_t *m, const char *key);   /* NULL, если нет */
int aa_smap_has(const aa_smap_t *m, const char *key);
int aa_smap_set(aa_smap_t *m, const char *key, void *val);/* 0 — ок (заменяет), -1 — нет памяти */
void *aa_smap_del(aa_smap_t *m, const char *key);         /* возвращает удалённое значение */
size_t aa_smap_count(const aa_smap_t *m);
/* Обход; cb возвращает != 0 для остановки. Изменять таблицу из cb нельзя. */
void aa_smap_foreach(const aa_smap_t *m, int (*cb)(const char *key, void *val, void *ctx), void *ctx);
#ifdef __cplusplus
}
#endif
#endif
