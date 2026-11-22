/*
 * Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
 * Copyright (c) 2026: Сенашенко Дмитрий.
 * Разработано в Российской Федерации.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * mod_aa_h323_tr -- H.323 trunk to Avaya PBXs (FreeSWITCH-facing part)
 *
 * This file is the FreeSWITCH half of the module: load/shutdown, XML + JSON config, the "aa_h323_tr" endpoint
 * (io_routines + state handlers), per-call RTP (switch_rtp, exactly as in the sibling mod_aa_h323) and the API command.
 * It never touches an H.225/H.245 byte: all of that lives in the pure-C engine (../aa_tr_engine.c + aa_tr_msg.c).
 *
 * Dial string (FreeSWITCH -> PBX):   aa_h323_tr/<trunk>/<number>        e.g.  bridge  aa_h323_tr/cm1/102
 * Calls from a PBX (PBX -> FreeSWITCH) are launched into the dialplan context "context" of aa_h323_tr.conf.xml
 * (default "aa_h323_tr"); destination_number is the called number AFTER the trunk's in_number_rules.
 *
 * Media: the PBX gateways exchange RTP directly with FreeSWITCH. We announce our RTP address/port in the H.323 fastStart
 * and send RTP to the address the far side announces in its own fastStart.
 *
 * Free software, no licence, no trial, no limits. Author: Senashenko D.V., 2026.
 */
#include <switch.h>
#include "../aa_tr_engine.h"

#ifndef AA_TR_BUILD
#define AA_TR_BUILD "dev"
#endif

SWITCH_MODULE_LOAD_FUNCTION(mod_aa_h323_tr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_aa_h323_tr_shutdown);
SWITCH_MODULE_DEFINITION(mod_aa_h323_tr, mod_aa_h323_tr_load, mod_aa_h323_tr_shutdown, NULL);

typedef enum {
	TFLAG_IO = (1 << 0),
	TFLAG_ANSWERED = (1 << 1)
} tr_tflag_t;

typedef enum { TR_DIR_TO_PBX, TR_DIR_FROM_PBX } tr_dir_t;

typedef struct {
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_caller_profile_t *caller_profile;
	switch_rtp_t *rtp_session;
	switch_mutex_t *flag_mutex;
	unsigned int flags;
	tr_dir_t direction;
	char call_id[AA_TR_ID_LEN];
	char trunk[32];
	char local_ip[64];
	switch_port_t local_port;
	char remote_ip[64];
	switch_port_t remote_port;
	int remote_known;
	char codec_name[8];
	switch_payload_t payload;
	switch_codec_t read_codec;
	switch_codec_t write_codec;
	switch_frame_t read_frame;
	unsigned char databuf[SWITCH_RECOMMENDED_BUFFER_SIZE];
} tr_private_t;

static struct {
	switch_memory_pool_t *pool;
	switch_mutex_t *mutex;
	switch_endpoint_interface_t *endpoint_interface;
	char config_path[512];
	char dialplan[32];
	char context[64];
	char rtp_ip[64];                 /* optional XML override: IP of our RTP sockets (default local.ip of the JSON) */
	aa_tr_config_t cfg;
	aa_tr_engine_t *engine;
} G;

extern switch_state_handler_table_t aa_tr_state_handlers;
extern switch_io_routines_t aa_tr_io_routines;

/* ---------------------------------------------------------------------- */
/* config                                                                  */
/* ---------------------------------------------------------------------- */

static switch_status_t tr_read_xml(void)
{
	switch_xml_t cfg, xml, settings, param;

	switch_set_string(G.dialplan, "XML");
	switch_set_string(G.context, "aa_h323_tr");
	G.config_path[0] = '\0';
	G.rtp_ip[0] = '\0';

	if (!(xml = switch_xml_open_cfg("aa_h323_tr.conf", &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: cannot open aa_h323_tr.conf\n");
		return SWITCH_STATUS_FALSE;
	}
	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");

			if (zstr(var) || zstr(val)) {
				continue;
			}
			if (!strcasecmp(var, "config-json")) {
				switch_set_string(G.config_path, val);
			} else if (!strcasecmp(var, "dialplan")) {
				switch_set_string(G.dialplan, val);
			} else if (!strcasecmp(var, "context")) {
				switch_set_string(G.context, val);
			} else if (!strcasecmp(var, "local-rtp-ip")) {
				switch_set_string(G.rtp_ip, val);
			}
		}
	}
	switch_xml_free(xml);

	if (!G.config_path[0]) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: param config-json is not set in aa_h323_tr.conf.xml\n");
		return SWITCH_STATUS_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* Reads the JSON config into *out. Returns 0 on success. */
static int tr_load_json(aa_tr_config_t *out, char *err, size_t cap)
{
	return aa_tr_config_load(G.config_path, out, err, cap);
}

static const char *tr_rtp_ip(void)
{
	return G.rtp_ip[0] ? G.rtp_ip : G.cfg.local_ip;
}

/* ---------------------------------------------------------------------- */
/* RTP (same approach as mod_aa_h323: switch_rtp inside the channel)       */
/* ---------------------------------------------------------------------- */

static void tr_rtp_safe_destroy(tr_private_t *tp)
{
	if (!tp->rtp_session) {
		return;
	}
	if (!tp->remote_known) {
		const char *e = NULL;
		switch_rtp_set_remote_address(tp->rtp_session, tp->local_ip, tp->local_port, 0, SWITCH_TRUE, &e);
	}
	switch_rtp_destroy(&tp->rtp_session);
}

/* codec: "PCMA" | "PCMU". remote_ip may be NULL (outbound: the far RTP address is learnt from its ALERTING). */
static switch_status_t tr_rtp_setup(tr_private_t *tp, switch_core_session_t *session, const char *codec, const char *remote_ip, switch_port_t remote_port)
{
	const char *err = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	const char *tx_host;
	switch_port_t tx_port;

	switch_set_string(tp->codec_name, codec);
	tp->payload = !strcmp(codec, "PCMU") ? 0 : 8;
	switch_set_string(tp->local_ip, tr_rtp_ip());

	tp->local_port = switch_rtp_request_port(tp->local_ip);
	if (!tp->local_port) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "aa_h323_tr: no free RTP port on %s\n", tp->local_ip);
		return SWITCH_STATUS_FALSE;
	}

	/* switch_rtp_new() with BOTH addresses at once is what makes the session "ready" (see mod_aa_h323's long comment). */
	tx_host = (!zstr(remote_ip) && remote_port) ? remote_ip : tp->local_ip;
	tx_port = (!zstr(remote_ip) && remote_port) ? remote_port : tp->local_port;
	tp->rtp_session = switch_rtp_new(tp->local_ip, tp->local_port, tx_host, tx_port, tp->payload, 160, 20000, flags, "soft", &err, pool, 0, 0);
	if (!tp->rtp_session) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "aa_h323_tr: switch_rtp_new failed: %s\n", err ? err : "unknown error");
		tp->local_port = 0;					/* switch_rtp_new() already released the port */
		return SWITCH_STATUS_FALSE;
	}
	if (!switch_rtp_ready(tp->rtp_session)) {
		switch_rtp_destroy(&tp->rtp_session);
		switch_rtp_release_port(tp->local_ip, tp->local_port);
		tp->local_port = 0;
		return SWITCH_STATUS_FALSE;
	}
	if (!zstr(remote_ip) && remote_port) {
		switch_set_string(tp->remote_ip, remote_ip);
		tp->remote_port = remote_port;
		tp->remote_known = 1;
	}
	switch_rtp_set_default_payload(tp->rtp_session, tp->payload);

	if (switch_core_codec_init(&tp->read_codec, tp->codec_name, NULL, NULL, 8000, 20, 1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "aa_h323_tr: read codec %s init failed\n", tp->codec_name);
		tr_rtp_safe_destroy(tp);
		switch_rtp_release_port(tp->local_ip, tp->local_port);
		tp->local_port = 0;
		return SWITCH_STATUS_FALSE;
	}
	if (switch_core_codec_init(&tp->write_codec, tp->codec_name, NULL, NULL, 8000, 20, 1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "aa_h323_tr: write codec %s init failed\n", tp->codec_name);
		switch_core_codec_destroy(&tp->read_codec);
		tr_rtp_safe_destroy(tp);
		switch_rtp_release_port(tp->local_ip, tp->local_port);
		tp->local_port = 0;
		return SWITCH_STATUS_FALSE;
	}
	tp->read_frame.data = tp->databuf;
	tp->read_frame.buflen = sizeof(tp->databuf);
	tp->read_frame.codec = &tp->read_codec;

	switch_core_session_set_read_codec(session, &tp->read_codec);
	switch_core_session_set_write_codec(session, &tp->write_codec);
	switch_set_flag_locked(tp, TFLAG_IO);
	switch_channel_set_flag(tp->channel, CF_AUDIO);
	return SWITCH_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* engine -> FreeSWITCH (called from engine threads)                       */
/* ---------------------------------------------------------------------- */

static void eh_log(void *ctx, int lvl, const char *msg)
{
	switch_log_level_t l = lvl >= 40 ? SWITCH_LOG_ERROR : lvl >= 30 ? SWITCH_LOG_WARNING : lvl >= 20 ? SWITCH_LOG_INFO : SWITCH_LOG_DEBUG;
	(void) ctx;
	switch_log_printf(SWITCH_CHANNEL_LOG, l, "%s\n", msg);
}

/* A call from the PBX: build an INBOUND FreeSWITCH channel and launch it into the dialplan. */
static int eh_incoming(void *ctx, const aa_tr_incoming_t *in, aa_tr_accept_t *out)
{
	switch_core_session_t *session;
	switch_memory_pool_t *pool = NULL;
	tr_private_t *tp;
	switch_channel_t *channel;
	char name[128];

	(void) ctx;
	if (!(session = switch_core_session_request_uuid(G.endpoint_interface, SWITCH_CALL_DIRECTION_INBOUND, SOF_NONE, &pool, in->call_id))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: cannot create an inbound session\n");
		return 41;					/* temporary failure */
	}
	if (!(tp = switch_core_session_alloc(session, sizeof(*tp)))) {
		switch_core_session_destroy(&session);
		return 41;
	}
	memset(tp, 0, sizeof(*tp));
	switch_core_session_add_stream(session, NULL);
	tp->session = session;
	tp->direction = TR_DIR_FROM_PBX;
	switch_set_string(tp->call_id, in->call_id);
	switch_set_string(tp->trunk, in->trunk->name);
	switch_mutex_init(&tp->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	switch_core_session_set_private(session, tp);

	channel = switch_core_session_get_channel(session);
	tp->channel = channel;
	switch_snprintf(name, sizeof(name), "aa_h323_tr/%s/%s", in->trunk->name, in->called);
	switch_channel_set_name(channel, name);

	if (tr_rtp_setup(tp, session, in->codec == AA_TR_CODEC_U ? "PCMU" : "PCMA", in->media_ip, (switch_port_t) in->media_port) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_destroy(&session);
		return 47;					/* resource unavailable */
	}
	if (switch_core_session_thread_launch(session) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: switch_core_session_thread_launch failed\n");
		switch_core_session_destroy(&session);
		return 41;
	}
	if (switch_core_session_read_lock(session) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_destroy(&session);
		return 41;
	}
	{
		switch_caller_profile_t *cp = switch_caller_profile_new(switch_core_session_get_pool(session), NULL, G.dialplan,
																 zstr(in->calling_name) ? in->calling : in->calling_name,
																 in->calling, in->peer_ip, NULL, NULL, NULL, "mod_aa_h323_tr", G.context, in->called);
		tp->caller_profile = cp;
		switch_channel_set_caller_profile(channel, cp);
	}
	switch_channel_set_variable(channel, "aa_h323_tr_trunk", in->trunk->name);
	switch_channel_set_variable(channel, "aa_h323_tr_called_raw", in->called_raw);
	switch_channel_set_variable(channel, "aa_h323_tr_codec", tp->codec_name);

	switch_snprintf(out->rtp_ip, sizeof(out->rtp_ip), "%s", tp->local_ip);
	out->rtp_port = tp->local_port;

	switch_channel_set_state(channel, CS_INIT);
	switch_core_session_rwunlock(session);
	return 0;
}

static switch_core_session_t *tr_locate(const char *call_id)
{
	return switch_core_session_locate(call_id);
}

static void eh_ringing(void *ctx, const char *call_id)
{
	switch_core_session_t *session = tr_locate(call_id);

	(void) ctx;
	if (session) {
		switch_channel_mark_ring_ready(switch_core_session_get_channel(session));
		switch_core_session_rwunlock(session);
	}
}

static void eh_media(void *ctx, const char *call_id, const char *ip, int port, unsigned codec)
{
	switch_core_session_t *session = tr_locate(call_id);
	tr_private_t *tp;
	const char *err = NULL;

	(void) ctx;
	(void) codec;
	if (!session) {
		return;
	}
	tp = switch_core_session_get_private(session);
	if (tp && tp->rtp_session) {
		switch_set_string(tp->remote_ip, ip);
		tp->remote_port = (switch_port_t) port;
		tp->remote_known = 1;
		if (switch_rtp_set_remote_address(tp->rtp_session, ip, (switch_port_t) port, 0, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "aa_h323_tr: set_remote_address %s:%d failed: %s\n", ip, port, err ? err : "?");
		}
	}
	switch_core_session_rwunlock(session);
}

static void eh_answered(void *ctx, const char *call_id)
{
	switch_core_session_t *session = tr_locate(call_id);

	(void) ctx;
	if (session) {
		switch_channel_mark_answered(switch_core_session_get_channel(session));
		switch_core_session_rwunlock(session);
	}
}

static void eh_hangup(void *ctx, const char *call_id, int cause)
{
	switch_core_session_t *session = tr_locate(call_id);

	(void) ctx;
	if (session) {
		switch_channel_hangup(switch_core_session_get_channel(session), cause > 0 ? (switch_call_cause_t) cause : SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
	}
}

static void eh_dtmf(void *ctx, const char *call_id, char digit)
{
	switch_core_session_t *session = tr_locate(call_id);
	switch_dtmf_t dtmf = { 0 };

	(void) ctx;
	if (session) {
		dtmf.digit = digit;
		dtmf.duration = switch_core_default_dtmf_duration(0);
		switch_channel_queue_dtmf(switch_core_session_get_channel(session), &dtmf);
		switch_core_session_rwunlock(session);
	}
}

/* ---------------------------------------------------------------------- */
/* state handlers                                                          */
/* ---------------------------------------------------------------------- */

static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s CHANNEL INIT\n", switch_channel_get_name(switch_core_session_get_channel(session)));
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_routing(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s CHANNEL ROUTING\n", switch_channel_get_name(switch_core_session_get_channel(session)));
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_execute(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s CHANNEL EXECUTE\n", switch_channel_get_name(switch_core_session_get_channel(session)));
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
	tr_private_t *tp = switch_core_session_get_private(session);

	if (tp) {
		if (tp->rtp_session) {
			tr_rtp_safe_destroy(tp);
		}
		if (tp->local_port) {
			switch_rtp_release_port(tp->local_ip, tp->local_port);
		}
		if (switch_core_codec_ready(&tp->read_codec)) {
			switch_core_codec_destroy(&tp->read_codec);
		}
		if (switch_core_codec_ready(&tp->write_codec)) {
			switch_core_codec_destroy(&tp->write_codec);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	tr_private_t *tp = switch_core_session_get_private(session);
	int cause = (int) switch_channel_get_cause_q850(channel);

	if (tp) {
		switch_clear_flag_locked(tp, TFLAG_IO);
		/* Releases the PBX leg for both call directions; a no-op if the PBX released first. */
		if (G.engine && tp->call_id[0]) {
			aa_tr_hangup(G.engine, tp->call_id, cause > 0 ? cause : 16);
		}
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s CHANNEL HANGUP\n", switch_channel_get_name(channel));
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig)
{
	tr_private_t *tp = switch_core_session_get_private(session);

	switch (sig) {
	case SWITCH_SIG_KILL:
		switch_clear_flag_locked(tp, TFLAG_IO);
		if (tp->rtp_session) {
			switch_rtp_kill_socket(tp->rtp_session);
		}
		break;
	case SWITCH_SIG_BREAK:
		if (tp->rtp_session) {
			switch_rtp_break(tp->rtp_session);
		}
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* DTMF from FreeSWITCH towards the PBX: H.245 UserInputIndication (what the PBX itself uses). */
static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	tr_private_t *tp = switch_core_session_get_private(session);

	if (tp && G.engine && tp->call_id[0] && dtmf) {
		aa_tr_dtmf(G.engine, tp->call_id, dtmf->digit);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	tr_private_t *tp = switch_core_session_get_private(session);
	switch_status_t status;

	*frame = NULL;
	if (!switch_test_flag(tp, TFLAG_IO) || !tp->rtp_session) {
		return SWITCH_STATUS_FALSE;
	}
	while (switch_test_flag(tp, TFLAG_IO)) {
		tp->read_frame.flags = SFF_NONE;
		status = switch_rtp_zerocopy_read_frame(tp->rtp_session, &tp->read_frame, flags);

		/* A plain "no packet yet" is SWITCH_STATUS_TIMEOUT (or BREAK) - keep polling, do not kill the channel (see mod_aa_h323). */
		if (!SWITCH_READ_ACCEPTABLE(status) && status != SWITCH_STATUS_TIMEOUT) {
			return SWITCH_STATUS_FALSE;
		}
		if (status != SWITCH_STATUS_SUCCESS) {
			continue;
		}
		if (switch_rtp_has_dtmf(tp->rtp_session)) {
			switch_dtmf_t dtmf = { 0 };
			switch_rtp_dequeue_dtmf(tp->rtp_session, &dtmf);
		}
		if (tp->read_frame.datalen == 0) {
			continue;
		}
		*frame = &tp->read_frame;
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	tr_private_t *tp = switch_core_session_get_private(session);

	if (!switch_test_flag(tp, TFLAG_IO) || !tp->rtp_session) {
		return SWITCH_STATUS_FALSE;
	}
	if (!tp->remote_known) {
		return SWITCH_STATUS_SUCCESS;		/* far RTP address not known yet: drop */
	}
	switch_rtp_write_frame(tp->rtp_session, frame);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	tr_private_t *tp = switch_core_session_get_private(session);

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		/* FreeSWITCH answered a call that came from the PBX -> CONNECT. (Outbound calls are answered by the PBX.) */
		if (tp && tp->direction == TR_DIR_FROM_PBX && G.engine) {
			aa_tr_answer(G.engine, tp->call_id);
		}
		break;
	case SWITCH_MESSAGE_INDICATE_HOLD:
		switch_channel_set_callstate(switch_core_session_get_channel(session), CCS_HELD);
		break;
	case SWITCH_MESSAGE_INDICATE_UNHOLD:
		switch_channel_set_callstate(switch_core_session_get_channel(session), CCS_UNHELD);
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_event(switch_core_session_t *session, switch_event_t *event)
{
	(void) session;
	(void) event;
	return SWITCH_STATUS_SUCCESS;
}

/* aa_h323_tr/<trunk>/<number>: FreeSWITCH -> PBX */
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													 switch_caller_profile_t *outbound_profile,
													 switch_core_session_t **new_session, switch_memory_pool_t **pool,
													 switch_originate_flag_t flags, switch_call_cause_t *cancel_cause)
{
	tr_private_t *tp;
	switch_channel_t *channel;
	switch_caller_profile_t *caller_profile;
	char *dest, *slash;
	const char *trunk, *number;
	char codec[8];
	char name[160];
	int rc, i;

	(void) session;
	(void) var_event;
	(void) cancel_cause;

	if (!outbound_profile || zstr(outbound_profile->destination_number)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: no destination (dial as aa_h323_tr/<trunk>/<number>)\n");
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}
	dest = strdup(outbound_profile->destination_number);
	slash = strchr(dest, '/');
	if (!slash || !slash[1]) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: bad destination '%s' (dial as aa_h323_tr/<trunk>/<number>)\n", dest);
		free(dest);
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}
	*slash = '\0';
	trunk = dest;
	number = slash + 1;

	/* the trunk's codec decides the RTP payload */
	switch_set_string(codec, "PCMA");
	switch_mutex_lock(G.mutex);
	for (i = 0; i < G.cfg.ntrunk; i++) {
		if (!strcmp(G.cfg.trunk[i].name, trunk)) {
			switch_set_string(codec, G.cfg.trunk[i].codec);
			break;
		}
	}
	i = (i < G.cfg.ntrunk);
	switch_mutex_unlock(G.mutex);
	if (!i) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: no such trunk '%s'\n", trunk);
		free(dest);
		return SWITCH_CAUSE_NO_ROUTE_DESTINATION;
	}

	if (!(*new_session = switch_core_session_request(G.endpoint_interface, SWITCH_CALL_DIRECTION_OUTBOUND, flags, pool))) {
		free(dest);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}
	if (!(tp = switch_core_session_alloc(*new_session, sizeof(*tp)))) {
		switch_core_session_destroy(new_session);
		free(dest);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}
	memset(tp, 0, sizeof(*tp));
	switch_core_session_add_stream(*new_session, NULL);
	tp->session = *new_session;
	tp->direction = TR_DIR_TO_PBX;
	switch_set_string(tp->trunk, trunk);
	switch_set_string(tp->call_id, switch_core_session_get_uuid(*new_session));
	switch_mutex_init(&tp->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(*new_session));
	switch_core_session_set_private(*new_session, tp);

	channel = switch_core_session_get_channel(*new_session);
	tp->channel = channel;
	switch_snprintf(name, sizeof(name), "aa_h323_tr/%s/%s", trunk, number);
	switch_channel_set_name(channel, name);

	caller_profile = switch_caller_profile_clone(*new_session, outbound_profile);
	/* destination_number of the channel is the plain number, not "trunk/number" */
	caller_profile->destination_number = switch_core_strdup(caller_profile->pool, number);
	switch_channel_set_caller_profile(channel, caller_profile);
	tp->caller_profile = caller_profile;

	if (tr_rtp_setup(tp, *new_session, codec, NULL, 0) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_destroy(new_session);
		free(dest);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	rc = aa_tr_originate(G.engine, trunk, tp->call_id,
						 zstr(outbound_profile->caller_id_number) ? "" : outbound_profile->caller_id_number,
						 zstr(outbound_profile->caller_id_name) ? "" : outbound_profile->caller_id_name, number, tp->local_ip, tp->local_port);
	free(dest);
	if (rc != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(*new_session), SWITCH_LOG_WARNING, "aa_h323_tr: cannot start the call: %s\n", switch_channel_cause2str((switch_call_cause_t) rc));
		switch_core_session_destroy(new_session);
		return (switch_call_cause_t) rc;
	}

	switch_channel_set_state(channel, CS_INIT);
	return SWITCH_CAUSE_SUCCESS;
}

switch_state_handler_table_t aa_tr_state_handlers = {
	/*.on_init */ channel_on_init,
	/*.on_routing */ channel_on_routing,
	/*.on_execute */ channel_on_execute,
	/*.on_hangup */ channel_on_hangup,
	/*.on_exchange_media */ NULL,
	/*.on_soft_execute */ NULL,
	/*.on_consume_media */ NULL,
	/*.on_hibernate */ NULL,
	/*.on_reset */ NULL,
	/*.on_park */ NULL,
	/*.on_reporting */ NULL,
	/*.on_destroy */ channel_on_destroy
};

switch_io_routines_t aa_tr_io_routines = {
	/*.outgoing_channel */ channel_outgoing_channel,
	/*.read_frame */ channel_read_frame,
	/*.write_frame */ channel_write_frame,
	/*.kill_channel */ channel_kill_channel,
	/*.send_dtmf */ channel_send_dtmf,
	/*.receive_message */ channel_receive_message,
	/*.receive_event */ channel_receive_event
};

/* ---------------------------------------------------------------------- */
/* API: aa_h323_tr help|status|trunks|reload|number                        */
/* ---------------------------------------------------------------------- */

#define TR_API_SYNTAX \
	"aa_h323_tr help\n" \
	"aa_h323_tr status\n" \
	"aa_h323_tr trunks\n" \
	"aa_h323_tr reload\n" \
	"aa_h323_tr number <trunk> <called>   (check the in_number_rules of a trunk)\n"

SWITCH_STANDARD_API(aa_h323_tr_api)
{
	char *argv[4] = { 0 };
	char *mycmd = NULL;
	int argc = 0, i;

	if (!zstr(cmd)) {
		mycmd = strdup(cmd);
		argc = switch_separate_string(mycmd, ' ', argv, 4);
	}
	if (argc == 0 || !strcasecmp(argv[0], "help")) {
		stream->write_function(stream, "%s", TR_API_SYNTAX);
	} else if (!strcasecmp(argv[0], "status")) {
		char *st = G.engine ? aa_tr_status_text(G.engine) : NULL;

		stream->write_function(stream, "mod_aa_h323_tr build %s\nconfig: %s\nrtp ip: %s, dialplan %s / context %s\n%s", AA_TR_BUILD, G.config_path,
							   tr_rtp_ip(), G.dialplan, G.context, st ? st : "engine is not running\n");
		free(st);
	} else if (!strcasecmp(argv[0], "trunks")) {
		switch_mutex_lock(G.mutex);
		for (i = 0; i < G.cfg.ntrunk; i++) {
			const aa_tr_trunk_t *t = &G.cfg.trunk[i];

			stream->write_function(stream, "%s  %s:%d  codec=%s  lines=%d  active=%d  rules=%d  %s\n", t->name, t->ip, t->port, t->codec, t->lines,
								   G.engine ? aa_tr_active_calls(G.engine, t->name) : 0, t->nrules,
								   (G.engine && !aa_tr_trunk_in_service(G.engine, t->name)) ? "OUT OF SERVICE" : "in service");
		}
		switch_mutex_unlock(G.mutex);
	} else if (!strcasecmp(argv[0], "reload")) {
		char err[256] = "";
		aa_tr_config_t *nc = malloc(sizeof(*nc));

		if (nc && tr_load_json(nc, err, sizeof(err)) == 0) {
			switch_mutex_lock(G.mutex);
			G.cfg = *nc;
			switch_mutex_unlock(G.mutex);
			aa_tr_engine_reload(G.engine, nc);
			stream->write_function(stream, "+OK reloaded, trunks: %d\n(local.ip and h225_port change only on module reload)\n", nc->ntrunk);
		} else {
			stream->write_function(stream, "-ERR %s\n", nc ? err : "no memory");
		}
		free(nc);
	} else if (!strcasecmp(argv[0], "number") && argc >= 3) {
		const aa_tr_trunk_t *t;
		char out[64];
		int hit;

		switch_mutex_lock(G.mutex);
		t = aa_tr_find_trunk_by_name(&G.cfg, argv[1]);
		if (!t) {
			stream->write_function(stream, "-ERR no such trunk\n");
		} else {
			hit = aa_tr_number_in(t, argv[2], out, sizeof(out));
			stream->write_function(stream, "%s -> %s%s\n", argv[2], out, hit ? "" : "  (no rule matched)");
		}
		switch_mutex_unlock(G.mutex);
	} else {
		stream->write_function(stream, "-ERR unknown command\n%s", TR_API_SYNTAX);
	}
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* load / shutdown                                                         */
/* ---------------------------------------------------------------------- */

SWITCH_MODULE_LOAD_FUNCTION(mod_aa_h323_tr_load)
{
	switch_api_interface_t *api_interface;
	aa_tr_host_t host;
	char err[256] = "";

	memset(&G, 0, sizeof(G));
	G.pool = pool;
	switch_mutex_init(&G.mutex, SWITCH_MUTEX_NESTED, pool);

	if (tr_read_xml() != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_TERM;
	}
	if (tr_load_json(&G.cfg, err, sizeof(err)) != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "aa_h323_tr: %s: %s\n", G.config_path, err);
		return SWITCH_STATUS_TERM;
	}

	memset(&host, 0, sizeof(host));
	host.log = eh_log;
	host.on_incoming = eh_incoming;
	host.on_ringing = eh_ringing;
	host.on_media = eh_media;
	host.on_answered = eh_answered;
	host.on_hangup = eh_hangup;
	host.on_dtmf = eh_dtmf;
	if (!(G.engine = aa_tr_engine_new(&G.cfg, &host))) {
		return SWITCH_STATUS_TERM;
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	G.endpoint_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	G.endpoint_interface->interface_name = "aa_h323_tr";
	G.endpoint_interface->io_routines = &aa_tr_io_routines;
	G.endpoint_interface->state_handler = &aa_tr_state_handlers;
	SWITCH_ADD_API(api_interface, "aa_h323_tr", "H.323 trunk to Avaya PBXs", aa_h323_tr_api, TR_API_SYNTAX);

	if (aa_tr_engine_start(G.engine, err, sizeof(err)) != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "aa_h323_tr: cannot start the H.225 listener: %s\n", err);
		aa_tr_engine_free(G.engine);
		G.engine = NULL;
		return SWITCH_STATUS_TERM;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_aa_h323_tr %s loaded: %d trunk(s), H.225 listen %s:%d (%s), dialplan %s/%s\n", AA_TR_BUILD,
					  G.cfg.ntrunk, G.cfg.local_ip, G.cfg.h225_port, aa_tr_engine_listening(G.engine) ? "listening" : "NOT LISTENING - port busy?", G.dialplan,
					  G.context);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_aa_h323_tr_shutdown)
{
	aa_tr_engine_t *e = G.engine;

	G.engine = NULL;
	if (e) {
		aa_tr_engine_stop(e);
		aa_tr_engine_free(e);
	}
	return SWITCH_STATUS_SUCCESS;
}
