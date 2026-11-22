/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* САМОПИСНАЯ заглушка FreeSWITCH API - только для проверки синтаксиса/опечаток в fs/mod_aa_h323_tr.c без заголовков FS
 * (make fs-syntax). Это НЕ заголовки FreeSWITCH и не проверка ABI: настоящая проверка - сборка на стенде (install.sh). */
#ifndef STUB_SWITCH_H
#define STUB_SWITCH_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
typedef int switch_status_t; typedef uint16_t switch_port_t; typedef int switch_call_cause_t; typedef int switch_log_level_t;
typedef uint8_t switch_payload_t; typedef int switch_bool_t; typedef int switch_io_flag_t; typedef int switch_originate_flag_t;
typedef int switch_call_direction_t; typedef int switch_rtp_flag_t; typedef int switch_state_t;
enum { SWITCH_STATUS_SUCCESS, SWITCH_STATUS_FALSE, SWITCH_STATUS_TERM, SWITCH_STATUS_TIMEOUT, SWITCH_STATUS_BREAK };
enum { SWITCH_CAUSE_NONE, SWITCH_CAUSE_SUCCESS, SWITCH_CAUSE_NORMAL_CLEARING = 16, SWITCH_CAUSE_NO_ROUTE_DESTINATION = 3,
       SWITCH_CAUSE_INVALID_NUMBER_FORMAT = 28, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER = 27 };
enum { SWITCH_LOG_DEBUG, SWITCH_LOG_INFO, SWITCH_LOG_NOTICE, SWITCH_LOG_WARNING, SWITCH_LOG_ERROR, SWITCH_LOG_CRIT };
enum { SWITCH_CALL_DIRECTION_INBOUND, SWITCH_CALL_DIRECTION_OUTBOUND };
enum { SOF_NONE = 0 };
enum { SWITCH_RTP_FLAG_INVALID = 64 };
enum { CS_INIT, CS_ROUTING };
enum { CF_AUDIO, CF_ANSWERED };
enum { CCS_HELD, CCS_UNHELD };
enum { SWITCH_SIG_KILL, SWITCH_SIG_BREAK };
enum { SFF_NONE = 0 };
enum { SWITCH_CODEC_FLAG_ENCODE = 1, SWITCH_CODEC_FLAG_DECODE = 2 };
enum { SWITCH_MESSAGE_INDICATE_ANSWER, SWITCH_MESSAGE_INDICATE_HOLD, SWITCH_MESSAGE_INDICATE_UNHOLD };
enum { SWITCH_MUTEX_NESTED = 1 };
enum { SWITCH_ENDPOINT_INTERFACE };
enum { SWITCH_TRUE = 1, SWITCH_FALSE = 0 };
#define SWITCH_CHANNEL_LOG 0
#define SWITCH_CHANNEL_SESSION_LOG(s) 0
#define SWITCH_RECOMMENDED_BUFFER_SIZE 8192
#define SWITCH_READ_ACCEPTABLE(s) ((s) == SWITCH_STATUS_SUCCESS || (s) == SWITCH_STATUS_BREAK)
typedef struct switch_core_session switch_core_session_t; typedef struct switch_channel switch_channel_t;
typedef struct switch_memory_pool switch_memory_pool_t; typedef struct switch_mutex switch_mutex_t; typedef struct switch_rtp switch_rtp_t;
typedef struct { const char *interface_name; void *io_routines; void *state_handler; } switch_endpoint_interface_t;
typedef struct { int dummy; } switch_loadable_module_interface_t;
typedef struct { int dummy; } switch_api_interface_t;
typedef struct { char digit; uint32_t duration; } switch_dtmf_t;
typedef struct switch_stream_handle { int (*write_function)(struct switch_stream_handle *, const char *, ...); } switch_stream_handle_t;
typedef struct { switch_memory_pool_t *pool; const char *destination_number; const char *caller_id_name; const char *caller_id_number; } switch_caller_profile_t;
typedef struct { int dummy; } switch_codec_t;
typedef struct { void *data; uint32_t datalen; uint32_t buflen; switch_codec_t *codec; int flags; } switch_frame_t;
typedef struct { int message_id; } switch_core_session_message_t;
typedef struct { int dummy; } switch_event_t;
typedef struct switch_xml *switch_xml_t;
struct switch_xml { struct switch_xml *next; };
typedef struct {
    switch_status_t (*on_init)(switch_core_session_t *); switch_status_t (*on_routing)(switch_core_session_t *);
    switch_status_t (*on_execute)(switch_core_session_t *); switch_status_t (*on_hangup)(switch_core_session_t *);
    switch_status_t (*on_exchange_media)(switch_core_session_t *); switch_status_t (*on_soft_execute)(switch_core_session_t *);
    switch_status_t (*on_consume_media)(switch_core_session_t *); switch_status_t (*on_hibernate)(switch_core_session_t *);
    switch_status_t (*on_reset)(switch_core_session_t *); switch_status_t (*on_park)(switch_core_session_t *);
    switch_status_t (*on_reporting)(switch_core_session_t *); switch_status_t (*on_destroy)(switch_core_session_t *);
} switch_state_handler_table_t;
typedef struct {
    switch_call_cause_t (*outgoing_channel)(switch_core_session_t *, switch_event_t *, switch_caller_profile_t *, switch_core_session_t **,
                                            switch_memory_pool_t **, switch_originate_flag_t, switch_call_cause_t *);
    switch_status_t (*read_frame)(switch_core_session_t *, switch_frame_t **, switch_io_flag_t, int);
    switch_status_t (*write_frame)(switch_core_session_t *, switch_frame_t *, switch_io_flag_t, int);
    switch_status_t (*kill_channel)(switch_core_session_t *, int);
    switch_status_t (*send_dtmf)(switch_core_session_t *, const switch_dtmf_t *);
    switch_status_t (*receive_message)(switch_core_session_t *, switch_core_session_message_t *);
    switch_status_t (*receive_event)(switch_core_session_t *, switch_event_t *);
} switch_io_routines_t;

#define SWITCH_MODULE_LOAD_FUNCTION(name) switch_status_t name(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool)
#define SWITCH_MODULE_SHUTDOWN_FUNCTION(name) switch_status_t name(void)
#define SWITCH_MODULE_DEFINITION(name, load, shut, runtime) static const char modname[] = #name
#define SWITCH_STANDARD_API(name) switch_status_t name(const char *cmd, switch_core_session_t *session, switch_stream_handle_t *stream)
#define SWITCH_ADD_API(api_int, name, desc, func, syntax) do { (void) (api_int); (void) (name); (void) (desc); (void) (func); (void) (syntax); } while (0)
#define switch_set_string(dst, src) strncpy((dst), (src), sizeof(dst) - 1)
#define switch_snprintf snprintf
#define zstr(s) (!(s) || !*(s))
#define switch_safe_free(p) do { free(p); (p) = NULL; } while (0)
#define switch_test_flag(p, f) ((p)->flags & (f))
#define switch_set_flag_locked(p, f) ((p)->flags |= (f))
#define switch_clear_flag_locked(p, f) ((p)->flags &= ~(f))
#define switch_core_session_alloc(s, n) calloc(1, (n))
#define switch_core_strdup(pool, s) strdup(s)
#define switch_separate_string(buf, d, arr, n) stub_sep((buf), (d), (arr), (n))
int stub_sep(char *buf, char d, char **arr, int n);

void switch_log_printf(int channel, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
switch_core_session_t *switch_core_session_locate(const char *uuid);
void switch_core_session_rwunlock(switch_core_session_t *s);
switch_status_t switch_core_session_read_lock(switch_core_session_t *s);
switch_channel_t *switch_core_session_get_channel(switch_core_session_t *s);
switch_memory_pool_t *switch_core_session_get_pool(switch_core_session_t *s);
const char *switch_core_session_get_uuid(switch_core_session_t *s);
void *switch_core_session_get_private(switch_core_session_t *s);
void switch_core_session_set_private(switch_core_session_t *s, void *p);
switch_status_t switch_core_session_add_stream(switch_core_session_t *s, void *p);
switch_status_t switch_core_session_thread_launch(switch_core_session_t *s);
void switch_core_session_destroy(switch_core_session_t **s);
switch_core_session_t *switch_core_session_request(const switch_endpoint_interface_t *e, switch_call_direction_t d, switch_originate_flag_t f, switch_memory_pool_t **p);
switch_core_session_t *switch_core_session_request_uuid(const switch_endpoint_interface_t *e, switch_call_direction_t d, switch_originate_flag_t f, switch_memory_pool_t **p, const char *uuid);
switch_status_t switch_core_session_set_read_codec(switch_core_session_t *s, switch_codec_t *c);
switch_status_t switch_core_session_set_write_codec(switch_core_session_t *s, switch_codec_t *c);
switch_status_t switch_core_codec_init(switch_codec_t *c, const char *name, const char *fmtp, const char *mod, uint32_t rate, int ms, int ch, uint32_t flags, const void *settings, switch_memory_pool_t *pool);
int switch_core_codec_ready(switch_codec_t *c);
switch_status_t switch_core_codec_destroy(switch_codec_t *c);
uint32_t switch_core_default_dtmf_duration(uint32_t d);
switch_status_t switch_mutex_init(switch_mutex_t **m, unsigned f, switch_memory_pool_t *p);
switch_status_t switch_mutex_lock(switch_mutex_t *m);
switch_status_t switch_mutex_unlock(switch_mutex_t *m);

switch_status_t switch_channel_queue_dtmf(switch_channel_t *c, const switch_dtmf_t *d);
void switch_channel_set_name(switch_channel_t *c, const char *n);
const char *switch_channel_get_name(switch_channel_t *c);
void switch_channel_set_caller_profile(switch_channel_t *c, switch_caller_profile_t *p);
void switch_channel_set_state(switch_channel_t *c, switch_state_t s);
void switch_channel_set_flag(switch_channel_t *c, int f);
void switch_channel_mark_ring_ready(switch_channel_t *c);
void switch_channel_mark_answered(switch_channel_t *c);
void switch_channel_set_callstate(switch_channel_t *c, int s);
switch_status_t switch_channel_set_variable(switch_channel_t *c, const char *var, const char *val);
switch_call_cause_t switch_channel_hangup(switch_channel_t *c, switch_call_cause_t cause);
uint32_t switch_channel_get_cause_q850(switch_channel_t *c);
const char *switch_channel_cause2str(switch_call_cause_t c);
switch_caller_profile_t *switch_caller_profile_new(switch_memory_pool_t *p, const char *username, const char *dialplan, const char *cid_name, const char *cid_num,
                                                    const char *network_addr, const char *ani, const char *aniii, const char *rdnis, const char *source,
                                                    const char *context, const char *destination_number);
switch_caller_profile_t *switch_caller_profile_clone(switch_core_session_t *s, switch_caller_profile_t *p);

switch_port_t switch_rtp_request_port(const char *ip);
void switch_rtp_release_port(const char *ip, switch_port_t port);
switch_rtp_t *switch_rtp_new(const char *rx_host, switch_port_t rx_port, const char *tx_host, switch_port_t tx_port, switch_payload_t payload,
                             uint32_t samples, uint32_t ms, switch_rtp_flag_t *flags, char *timer_name, const char **err, switch_memory_pool_t *pool,
                             switch_port_t a, switch_port_t b);
int switch_rtp_ready(switch_rtp_t *r);
void switch_rtp_destroy(switch_rtp_t **r);
void switch_rtp_set_default_payload(switch_rtp_t *r, switch_payload_t p);
switch_status_t switch_rtp_set_remote_address(switch_rtp_t *r, const char *host, switch_port_t port, switch_port_t rtcp, switch_bool_t change, const char **err);
void switch_rtp_kill_socket(switch_rtp_t *r);
void switch_rtp_break(switch_rtp_t *r);
switch_status_t switch_rtp_zerocopy_read_frame(switch_rtp_t *r, switch_frame_t *f, switch_io_flag_t flags);
int switch_rtp_has_dtmf(switch_rtp_t *r);
switch_status_t switch_rtp_dequeue_dtmf(switch_rtp_t *r, switch_dtmf_t *d);
int switch_rtp_write_frame(switch_rtp_t *r, switch_frame_t *f);

switch_xml_t switch_xml_open_cfg(const char *file, switch_xml_t *node, void *params);
switch_xml_t switch_xml_child(switch_xml_t xml, const char *name);
const char *switch_xml_attr_soft(switch_xml_t xml, const char *attr);
void switch_xml_free(switch_xml_t xml);
switch_loadable_module_interface_t *switch_loadable_module_create_module_interface(switch_memory_pool_t *p, const char *name);
switch_endpoint_interface_t *switch_loadable_module_create_interface(switch_loadable_module_interface_t *m, int type);
#endif
