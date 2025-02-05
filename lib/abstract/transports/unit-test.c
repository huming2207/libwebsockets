/*
 * libwebsockets lib/abstract/transports/unit-test.c
 *
 * Copyright (C) 2019 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 *
 *
 * An abstract transport that is useful for unit testing an abstract protocol.
 * It doesn't actually connect to anything, but checks the protocol's response
 * to provided canned packets from an array of test vectors.
 */

#include "core/private.h"
#include "abstract/private.h"

/* this is the transport priv instantiated at abs->ati */

typedef struct lws_abstxp_unit_test_priv {
	char					note[128];
	struct lws_abs				*abs;

	lws_sequencer_t				*seq;
	lws_unit_test_t				*current_test;
	lws_unit_test_packet_t			*expect;
	lws_unit_test_packet_test_cb		result_cb;
	const void				*result_cb_arg;

	lws_unit_test_packet_disposition	disposition;
	/* synthesized protocol timeout */
	time_t					timeout;

	uint8_t					established:1;
	uint8_t					connecting:1;
} abs_unit_test_priv_t;

typedef struct seq_priv {
	lws_abs_t *ai;
} seq_priv_t;

enum {
	UTSEQ_MSG_WRITEABLE = LWSSEQ_USER_BASE,
	UTSEQ_MSG_CLOSING,
	UTSEQ_MSG_TIMEOUT,
	UTSEQ_MSG_CONNECTING,
	UTSEQ_MSG_POST_TX_KICK,
	UTSEQ_MSG_DISPOSITION_KNOWN
};

/*
 * A definitive result has appeared for the current test
 */

static lws_unit_test_packet_disposition
lws_unit_test_packet_dispose(abs_unit_test_priv_t *priv,
			    lws_unit_test_packet_disposition disp,
			    const char *note)
{
	assert(priv->disposition == LPE_CONTINUE);

	lwsl_info("%s: %d\n", __func__, disp);

	if (note)
		lws_strncpy(priv->note, note, sizeof(priv->note));

	priv->disposition = disp;

	lws_sequencer_event(priv->seq, UTSEQ_MSG_DISPOSITION_KNOWN, NULL);

	return disp;
}

/*
 * start on the next step of the test
 */

lws_unit_test_packet_disposition
process_expect(abs_unit_test_priv_t *priv)
{
	assert(priv->disposition == LPE_CONTINUE);

	while (priv->expect->flags & LWS_AUT_EXPECT_RX &&
	       priv->disposition == LPE_CONTINUE) {
		int f = priv->expect->flags & LWS_AUT_EXPECT_LOCAL_CLOSE, s;

		if (priv->expect->pre)
			priv->expect->pre(priv->abs);

		lwsl_info("%s: rx()\n", __func__);
		lwsl_hexdump_debug(priv->expect->buffer, priv->expect->len);
		s = priv->abs->ap->rx(priv->abs->api, priv->expect->buffer,
							priv->expect->len);

		if (!!f != !!s) {
			lwsl_notice("%s: expected rx return %d, got %d\n",
					__func__, !!f, s);

			return lws_unit_test_packet_dispose(priv, LPE_FAILED,
						  "rx unexpected return");
		}

		if (priv->expect->flags & LWS_AUT_EXPECT_TEST_END) {
			lws_unit_test_packet_dispose(priv, LPE_SUCCEEDED, NULL);
			break;
		}

		priv->expect++;
	}

	return LPE_CONTINUE;
}

static lws_seq_cb_return_t
unit_test_sequencer_cb(struct lws_sequencer *seq, void *user, int event,
		       void *data)
{
	seq_priv_t *s = (seq_priv_t *)user;
	abs_unit_test_priv_t *priv = (abs_unit_test_priv_t *)s->ai->ati;
	time_t now;

	switch ((int)event) {
	case LWSSEQ_CREATED: /* our sequencer just got started */
		lwsl_notice("%s: %s: created\n", __func__,
			    lws_sequencer_name(seq));
		if (s->ai->at->client_conn(s->ai)) {
			lwsl_notice("%s: %s: abstract client conn failed\n",
					__func__, lws_sequencer_name(seq));

			return LWSSEQ_RET_DESTROY;
		}
		break;

	case LWSSEQ_DESTROYED:
		/*
		 * This sequencer is about to be destroyed.  If we have any
		 * other assets in play, detach them from us.
		 */

		if (priv->abs)
			lws_abs_destroy_instance(&priv->abs);

		break;

	case LWSSEQ_HEARTBEAT:

		/* synthesize a wsi-style timeout */

		if (!priv->timeout)
			goto ph;

		time(&now);

		if (now <= priv->timeout)
			goto ph;

		if (priv->expect->flags & LWS_AUT_EXPECT_SHOULD_TIMEOUT) {
			lwsl_user("%s: test got expected timeout\n",
				  __func__);
			lws_unit_test_packet_dispose(priv,
					LPE_FAILED_UNEXPECTED_TIMEOUT, NULL);

			return LWSSEQ_RET_DESTROY;
		}
		lwsl_user("%s: seq timed out\n", __func__);

ph:
		if (priv->abs->ap->heartbeat)
			priv->abs->ap->heartbeat(priv->abs->api);
		break;

	case UTSEQ_MSG_DISPOSITION_KNOWN:

		lwsl_info("%s: %s: DISPOSITION_KNOWN %s: %s\n", __func__,
			  priv->abs->ap->name,
			  priv->current_test->name,
			  priv->disposition == LPE_SUCCEEDED ? "OK" : "FAIL");

		/*
		 * if the test has a callback, call it back to let it
		 * know the result
		 */
		if (priv->result_cb)
			priv->result_cb(priv->result_cb_arg, priv->disposition);

		return LWSSEQ_RET_DESTROY;

        case UTSEQ_MSG_CONNECTING:
		lwsl_debug("UTSEQ_MSG_CONNECTING\n");

		if (priv->abs->ap->accept)
			priv->abs->ap->accept(priv->abs->api);

		priv->established = 1;

		/* fallthru */

        case UTSEQ_MSG_POST_TX_KICK:
        	if (priv->disposition)
        		break;

		if (process_expect(priv) != LPE_CONTINUE) {
			lwsl_notice("%s: UTSEQ_MSG_POST_TX_KICK failed\n",
				 __func__);
			return LWSSEQ_RET_DESTROY;
		}
		break;

	case UTSEQ_MSG_WRITEABLE:
		/*
		 * inform the protocol our transport is writeable now
		 */
		priv->abs->ap->writeable(priv->abs->api, 1024);
		break;

	case UTSEQ_MSG_CLOSING:

		if (!(priv->expect->flags & LWS_AUT_EXPECT_LOCAL_CLOSE)) {
			lwsl_user("%s: got unexpected close\n", __func__);

			lws_unit_test_packet_dispose(priv,
					LPE_FAILED_UNEXPECTED_CLOSE, NULL);
			goto done;
		}

		/* tell the abstract protocol we are closing on them */

		if (priv->abs && priv->abs->ap->closed)
			priv->abs->ap->closed(priv->abs->api);

		goto done;

	case UTSEQ_MSG_TIMEOUT: /* current step timed out */

		s->ai->at->close(s->ai->ati);

		if (!(priv->expect->flags & LWS_AUT_EXPECT_SHOULD_TIMEOUT)) {
			lwsl_user("%s: got unexpected timeout\n", __func__);

			lws_unit_test_packet_dispose(priv,
					LPE_FAILED_UNEXPECTED_TIMEOUT, NULL);
			return LWSSEQ_RET_DESTROY;
		}
		goto done;

done:
		lws_sequencer_timeout(lws_sequencer_from_user(s), 0);
		priv->expect++;
		if (!priv->expect->buffer) {
			/* the sequence has completed */
			lwsl_user("%s: sequence completed OK\n", __func__);

			return LWSSEQ_RET_DESTROY;
		}
		break;

	default:
		break;
	}

	return LWSSEQ_RET_CONTINUE;
}

static int
lws_atcut_close(lws_abs_transport_inst_t *ati)
{
	abs_unit_test_priv_t *priv = (abs_unit_test_priv_t *)ati;

	lwsl_notice("%s\n", __func__);

	lws_sequencer_event(priv->seq, UTSEQ_MSG_CLOSING, NULL);

	return 0;
}

static int
lws_atcut_tx(lws_abs_transport_inst_t *ati, uint8_t *buf, size_t len)
{
	abs_unit_test_priv_t *priv = (abs_unit_test_priv_t *)ati;

	assert(priv->disposition == LPE_CONTINUE);

	lwsl_info("%s: received tx\n", __func__);

	if (priv->expect->pre)
		priv->expect->pre(priv->abs);

	if (!(priv->expect->flags & LWS_AUT_EXPECT_TX)) {
		lwsl_notice("%s: unexpected tx\n", __func__);
		lwsl_hexdump_notice(buf, len);
		lws_unit_test_packet_dispose(priv, LPE_FAILED, "unexpected tx");

		return 1;
	}

	if (len != priv->expect->len) {
		lwsl_notice("%s: unexpected tx len %zu, expected %zu\n",
				__func__, len, priv->expect->len);
		lws_unit_test_packet_dispose(priv, LPE_FAILED,
					     "tx len mismatch");

		return 1;
	}

	if (memcmp(buf, priv->expect->buffer, len)) {
		lwsl_notice("%s: tx mismatch (exp / actual)\n", __func__);
		lwsl_hexdump_debug(priv->expect->buffer, len);
		lwsl_hexdump_debug(buf, len);
		lws_unit_test_packet_dispose(priv, LPE_FAILED,
					     "tx data mismatch");

		return 1;
	}

	if (priv->expect->flags & LWS_AUT_EXPECT_TEST_END) {
		lws_unit_test_packet_dispose(priv, LPE_SUCCEEDED, NULL);

		return 1;
	}

	priv->expect++;

	lws_sequencer_event(priv->seq, UTSEQ_MSG_POST_TX_KICK, NULL);

	return 0;
}

#if !defined(LWS_WITHOUT_CLIENT)
static int
lws_atcut_client_conn(const lws_abs_t *abs)
{
	abs_unit_test_priv_t *priv = (abs_unit_test_priv_t *)abs->ati;
	const lws_token_map_t *tm;

	if (priv->established) {
		lwsl_err("%s: already established\n", __func__);
		return 1;
	}

	/* set up the test start pieces... the array of test expects... */

	tm = lws_abs_get_token(abs->at_tokens, LTMI_PEER_V_EXPECT_TEST);
	if (!tm) {
		lwsl_notice("%s: unit_test needs LTMI_PEER_V_EXPECT_TEST\n",
			    __func__);

		return 1;
	}
	priv->current_test = (lws_unit_test_t *)tm->u.value;

	/* ... and the callback to deliver the result to */
	tm = lws_abs_get_token(abs->at_tokens, LTMI_PEER_V_EXPECT_RESULT_CB);
	if (tm)
		priv->result_cb = (lws_unit_test_packet_test_cb)tm->u.value;
	else
		priv->result_cb = NULL;

	/* ... and the arg to deliver it with */
	tm = lws_abs_get_token(abs->at_tokens,
			       LTMI_PEER_V_EXPECT_RESULT_CB_ARG);
	if (tm)
		priv->result_cb_arg = tm->u.value;

	priv->expect = priv->current_test->expect_array;
	priv->disposition = LPE_CONTINUE;
	priv->note[0] = '\0';

	lws_sequencer_timeout(priv->seq, priv->current_test->max_secs);

	lwsl_notice("%s: %s: test '%s': start\n", __func__, abs->ap->name,
		    priv->current_test->name);

	lws_sequencer_event(priv->seq, UTSEQ_MSG_CONNECTING, NULL);

	return 0;
}
#endif

static int
lws_atcut_ask_for_writeable(lws_abs_transport_inst_t *ati)
{
	abs_unit_test_priv_t *priv = (abs_unit_test_priv_t *)ati;

	if (!priv->established)
		return 1;

	/*
	 * Queue a writeable event... this won't be handled by teh sequencer
	 * until we have returned to the event loop, just like a real
	 * callback_on_writable()
	 */
	lws_sequencer_event(priv->seq, UTSEQ_MSG_WRITEABLE, NULL);

	return 0;
}

/*
 * An abstract protocol + transport has been instantiated
 */

static int
lws_atcut_create(lws_abs_t *ai)
{
	abs_unit_test_priv_t *priv;
	lws_sequencer_t *seq;
	seq_priv_t *s;

	/*
	 * Create the sequencer for the steps in a single unit test
	 */

	seq = lws_sequencer_create(ai->vh->context, 0, sizeof(*s),
				   (void **)&s, unit_test_sequencer_cb,
				   "unit-test-seq");
	if (!seq) {
		lwsl_err("%s: unable to create sequencer\n", __func__);

		return 1;
	}

	priv = ai->ati;
	memset(s, 0, sizeof(*s));
	memset(priv, 0, sizeof(*priv));

	/* the sequencer priv just points to the lws_abs_t */
	s->ai = ai;
	priv->abs = ai;
	priv->seq = seq;

	return 0;
}

static void
lws_atcut_destroy(lws_abs_transport_inst_t **pati)
{
	/*
	 * We don't free anything because the abstract layer combined our
	 * allocation with that of the instance, and it will free the whole
	 * thing after this.
	 */
	*pati = NULL;
}

static int
lws_atcut_set_timeout(lws_abs_transport_inst_t *ati, int reason, int secs)
{
	abs_unit_test_priv_t *priv = (abs_unit_test_priv_t *)ati;
	time_t now;

	time(&now);

	if (secs)
		priv->timeout = now + secs;
	else
		priv->timeout = 0;

	return 0;
}

static int
lws_atcut_state(lws_abs_transport_inst_t *ati)
{
	abs_unit_test_priv_t *priv = (abs_unit_test_priv_t *)ati;

	if (!priv || (!priv->established && !priv->connecting))
		return 0;

	return 1;
}


const lws_abs_transport_t lws_abs_transport_cli_unit_test = {
	.name			= "unit_test",
	.alloc			= sizeof(abs_unit_test_priv_t),

	.create			= lws_atcut_create,
	.destroy		= lws_atcut_destroy,

	.tx			= lws_atcut_tx,
#if defined(LWS_WITHOUT_CLIENT)
	.client_conn		= NULL,
#else
	.client_conn		= lws_atcut_client_conn,
#endif
	.close			= lws_atcut_close,
	.ask_for_writeable	= lws_atcut_ask_for_writeable,
	.set_timeout		= lws_atcut_set_timeout,
	.state			= lws_atcut_state,
};
