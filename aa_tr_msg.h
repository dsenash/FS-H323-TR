/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * aa_tr_msg.h — сообщения H.225 (Q.931/TPKT) и туннелированный H.245 для H.323-транка Avaya (mod_aa_h323_tr), чистый C.
 *
 * Сообщения разбираются и собираются по шаблонам, снятым с реальных АТС (tools/gen_tr.py -> aa_tr_tpl.h):
 * подставляются call reference, GUID, адрес/порт RTP, сигнальный адрес, номера, display, product/version, cause.
 * Ядро не знает ни о сокетах, ни о FreeSWITCH.
 *
 * Свободный пакет (лицензия — см. LICENSE). Автор: Сенашенко Д.В., 2026.
 */
#ifndef AA_TR_MSG_H
#define AA_TR_MSG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AA_TR_STR 64
#define AA_TR_PDU_MAX 160

#define AA_TR_CODEC_A 1u   /* G.711A */
#define AA_TR_CODEC_U 2u   /* G.711u */

typedef enum {
    AA_TR_UNKNOWN = 0, AA_TR_SETUP, AA_TR_CALL_PROC, AA_TR_ALERTING, AA_TR_CONNECT,
    AA_TR_RELEASE_COMPLETE, AA_TR_STATUS, AA_TR_FACILITY
} aa_tr_type_t;

typedef enum {
    AA_H245_NONE = 0, AA_H245_TCS, AA_H245_MSD, AA_H245_TCSACK, AA_H245_MSDACK, AA_H245_ENDSESSION,
    AA_H245_UII, AA_H245_OTHER
} aa_h245_kind_t;

typedef struct {
    aa_tr_type_t type;
    unsigned callref;            /* 15 бит */
    int dest;                    /* флаг call reference: 1 — сообщение от принявшей стороны */
    uint8_t guid[16];            /* callIdentifier (и conferenceID в SETUP/CONNECT) */

    /* Q.931 */
    int has_sc;                  /* Sending complete */
    int has_bearer; uint8_t bearer[3];
    int cause;                   /* Q.850, -1 — нет */
    int has_callstate; uint8_t callstate;
    int has_prog; uint8_t prog;  /* Progress description: 0x83, 0x88, 0x82 */
    int has_calling; uint8_t calling_o3, calling_o3a; char calling[AA_TR_STR];
    int has_called; uint8_t called_o3; char called[AA_TR_STR];
    int has_display; char display[AA_TR_STR];

    /* UUIE */
    char product[AA_TR_STR], version[AA_TR_STR];     /* vendor: только SETUP/CALL_PROC/ALERTING/CONNECT */
    int has_media; uint8_t media_ip[4]; unsigned rtp_port;   /* fastStart: куда слать RTP (RTCP = +1) */
    uint8_t sig_ip[4]; unsigned sig_port;                    /* SETUP: sourceCallSignalAddress */
    unsigned codecs;             /* SETUP/TCS: предложенные кодеки (A|U); ALERTING: выбранный (A или U); иначе 0 */
    int x2_set; uint8_t x2[2];   /* SETUP: 2 байта перед conferenceID (в дампах 43 50, в одном вызове cc c0); не задано — как в шаблоне */

    /* туннелированный H.245 (FACILITY) */
    aa_h245_kind_t h245;
    uint8_t pdu[AA_TR_PDU_MAX]; size_t pdu_n;
    uint32_t msd_num;            /* MSD: statusDeterminationNumber */
    int msd_master;              /* MSDACK: 1 — отправитель ack является master */
    char uii;                    /* UII: символ DTMF */
} aa_tr_msg_t;

/* TPKT: длина первого кадра в буфере; 0 — кадр неполный, -1 — не TPKT. */
int aa_tr_tpkt_len(const uint8_t *b, size_t n);

/* Разбор одного кадра (с TPKT). 0 — ок; -1 — неизвестный вариант (m->type может быть заполнен частично). */
int aa_tr_parse(const uint8_t *frame, size_t n, aa_tr_msg_t *m);

/* Сборка кадра из полей m. Возвращает длину или -1 (мало места / неподдерживаемое сочетание). */
int aa_tr_build(uint8_t *out, size_t cap, const aa_tr_msg_t *m);

/* ---- H.245: готовые PDU. Возвращают длину. ---- */
size_t aa_tr_pdu_tcs(uint8_t *o, unsigned codecs);   /* набор возможностей: codecs = A, U или A|U, RFC 2833 (как у АТС) */
size_t aa_tr_pdu_msd(uint8_t *o, uint32_t num);            /* num: 65536..16777215 (3 октета) */
size_t aa_tr_pdu_msdack(uint8_t *o, int we_are_master);
size_t aa_tr_pdu_tcsack(uint8_t *o);
size_t aa_tr_pdu_endsession(uint8_t *o);
size_t aa_tr_pdu_uii(uint8_t *o, char c);
size_t aa_tr_pdu_caller_x1(uint8_t *o);                    /* PDU, который АТС-звонящая шлёт после MSD */
int aa_tr_msd_we_are_master(uint32_t ours, uint32_t theirs);   /* H.245 8.2: разность по модулю 2^24 */

/* Вспомогательное: заполнить поля нового сообщения. */
void aa_tr_msg_init(aa_tr_msg_t *m, aa_tr_type_t t);
void aa_tr_guid_random(uint8_t g[16]);

#ifdef __cplusplus
}
#endif
#endif
