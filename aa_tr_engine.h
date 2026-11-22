/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * aa_tr_engine.h — движок H.323-транка (mod_aa_h323_tr): TCP-сигнализация H.225/H.245 с АТС Avaya, автомат вызова.
 * Чистый C (pthreads + сокеты), про FreeSWITCH не знает: хозяин (FS-модуль) получает события через aa_tr_host_t
 * и управляет вызовами через aa_tr_originate/answer/hangup/dtmf.
 *
 * Модель: один TCP-вызов = одно соединение = один поток. Входящие вызовы принимает listener на local.ip:h225_port
 * (роль «.6» из дампов), исходящие движок устанавливает сам на trunk[].ip:port (роль «193»).
 *
 * Свободный пакет. Автор: Сенашенко Д.В., 2026.
 */
#ifndef AA_TR_ENGINE_H
#define AA_TR_ENGINE_H

#include <stddef.h>
#include "aa_tr_config.h"
#include "aa_tr_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AA_TR_ID_LEN 40      /* строка идентификатора вызова (UUID) */

/* Входящий вызов от АТС (описание для хозяина). */
typedef struct {
    const aa_tr_trunk_t *trunk;
    char peer_ip[48];
    char calling[64], calling_name[64];
    char called_raw[64];     /* номер, как пришёл от АТС */
    char called[64];         /* после in_number_rules транка */
    char media_ip[48]; int media_port;   /* куда АТС просит слать RTP (из fastStart SETUP) */
    unsigned codec;          /* выбранный кодек: AA_TR_CODEC_A или AA_TR_CODEC_U */
    char call_id[AA_TR_ID_LEN];   /* присвоен движком; хозяин должен использовать его как UUID канала FreeSWITCH */
} aa_tr_incoming_t;

/* Ответ хозяина на входящий вызов: где принимать RTP. */
typedef struct {
    char rtp_ip[48]; int rtp_port;
} aa_tr_accept_t;

typedef struct {
    void *ctx;
    /* level: 10 debug, 20 info, 30 warning, 40 error */
    void (*log)(void *ctx, int level, const char *msg);
    /* Входящий вызов. Вернуть 0 — принят (заполнить out), иначе причина Q.850 для отбоя (например 1, 34, 38). */
    int (*on_incoming)(void *ctx, const aa_tr_incoming_t *in, aa_tr_accept_t *out);
    /* Исходящий: дальняя сторона звонит (ALERTING). */
    void (*on_ringing)(void *ctx, const char *call_id);
    /* Исходящий: известен адрес RTP дальней стороны (ALERTING/CONNECT). */
    void (*on_media)(void *ctx, const char *call_id, const char *ip, int port, unsigned codec);
    /* Исходящий: дальняя сторона ответила (CONNECT). */
    void (*on_answered)(void *ctx, const char *call_id);
    /* Вызов завершён дальней стороной или по ошибке (cause — Q.850). Для отбоя по инициативе хозяина не вызывается. */
    void (*on_hangup)(void *ctx, const char *call_id, int cause);
    /* Цифра DTMF от дальней стороны (H.245 UserInputIndication). */
    void (*on_dtmf)(void *ctx, const char *call_id, char c);
} aa_tr_host_t;

typedef struct aa_tr_engine aa_tr_engine_t;

/* cfg копируется. host копируется. */
aa_tr_engine_t *aa_tr_engine_new(const aa_tr_config_t *cfg, const aa_tr_host_t *host);
/* Открывает listener local.ip:h225_port и запускает поток accept. Занятый порт НЕ ошибка (см. aa_tr_engine_listening). -1 — только системная ошибка (err). */
int aa_tr_engine_start(aa_tr_engine_t *e, char *err, size_t errcap);
/* 1 — listener работает; 0 — порт занят/не открылся (повторы раз в 5 с). Модуль при этом загружается, исходящие вызовы работают. */
int aa_tr_engine_listening(aa_tr_engine_t *e);
/* Порт, на котором реально слушаем (если h225_port = 0 в тестах — выбранный ОС). */
int aa_tr_engine_port(aa_tr_engine_t *e);
/* Останавливает всё: закрывает соединения, дожидается потоков. Вызовов хозяину после возврата нет. */
void aa_tr_engine_stop(aa_tr_engine_t *e);
void aa_tr_engine_free(aa_tr_engine_t *e);

/* Замена конфигурации (транки, правила, лимиты). Идущие вызовы продолжаются со старой копией транка.
 * Адрес/порт listener меняются только перезапуском модуля. */
void aa_tr_engine_reload(aa_tr_engine_t *e, const aa_tr_config_t *cfg);

/* Исходящий вызов на транк. call_id — UUID канала хозяина. 0 — запущен (результат — через on_*), иначе причина Q.850:
 * 3 — нет такого транка, 34 — нет свободных линий, 38 — транк выведен из обслуживания дальней стороной. */
int aa_tr_originate(aa_tr_engine_t *e, const char *trunk, const char *call_id, const char *calling,
                    const char *calling_name, const char *called, const char *rtp_ip, int rtp_port);
/* Входящий вызов отвечен хозяином -> CONNECT. */
int aa_tr_answer(aa_tr_engine_t *e, const char *call_id);
/* Отбой по инициативе хозяина (любое направление). cause — Q.850, 0 = 16. */
int aa_tr_hangup(aa_tr_engine_t *e, const char *call_id, int cause);
/* DTMF в сторону АТС (UII). 0 — отправлено. */
int aa_tr_dtmf(aa_tr_engine_t *e, const char *call_id, char c);

/* Текстовая сводка (malloc, освободить free) и счётчик активных вызовов транка (-1 — нет такого). */
char *aa_tr_status_text(aa_tr_engine_t *e);
int aa_tr_active_calls(aa_tr_engine_t *e, const char *trunk);
/* 1 — дальняя сторона объявила транк работающим (по умолчанию), 0 — выведен (STATUS cause 47). */
int aa_tr_trunk_in_service(aa_tr_engine_t *e, const char *trunk);

#ifdef __cplusplus
}
#endif
#endif
