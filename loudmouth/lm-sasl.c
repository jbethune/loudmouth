/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "lm-sock.h"
#include "lm-debug.h"
#include "lm-error.h"
#include "lm-internals.h"
#include "lm-message-queue.h"
#include "lm-misc.h"
#include "lm-ssl-internals.h"
#include "lm-parser.h"
#include "lm-sha.h"
#include "lm-connection.h"
#include "lm-utils.h"
#include "lm-socket.h"
#include "lm-sasl.h"

#include "md5.h"
#include "base64.h"

typedef enum {
	AUTH_TYPE_PLAIN  = 1,
	AUTH_TYPE_DIGEST = 2
} AuthType;

typedef enum {
  SASL_AUTH_STATE_NO_MECH = 0,
  SASL_AUTH_STATE_PLAIN_STARTED,
  SASL_AUTH_STATE_DIGEST_MD5_STARTED,
  SASL_AUTH_STATE_DIGEST_MD5_SENT_AUTH_RESPONSE,
  SASL_AUTH_STATE_DIGEST_MD5_SENT_FINAL_RESPONSE,
} SaslAuthState;

struct _LmSASL {
	LmConnection *connection;
	AuthType auth_type;
	SaslAuthState state;
	gchar *username;
	gchar *password;
	gchar *server;
	gchar *digest_md5_rspauth;
	LmMessageHandler *features_cb;
	LmMessageHandler *challenge_cb;
	LmMessageHandler *success_cb;
	LmMessageHandler *failure_cb;

	LmSASLResultHandler handler;
};

#define XMPP_NS_SASL_AUTH "urn:ietf:params:xml:ns:xmpp-sasl"

static LmHandlerResult
features_cb (LmMessageHandler *handler,
	     LmConnection *connection,
	     LmMessage *message,
	     gpointer user_data);

static LmHandlerResult
challenge_cb (LmMessageHandler *handler,
	      LmConnection *connection,
	      LmMessage *message,
	      gpointer user_data);

static LmHandlerResult
success_cb (LmMessageHandler *handler,
	    LmConnection *connection,
	    LmMessage *message,
	    gpointer user_data);

static LmHandlerResult
failure_cb (LmMessageHandler *handler,
	    LmConnection *connection,
	    LmMessage *message,
	    gpointer user_data);

LmSASL *
lm_sasl_new (LmConnection *connection,
	     const gchar *username,
	     const gchar *password,
	     const gchar *server,
	     LmSASLResultHandler handler)
{
	LmSASL *sasl = g_new0 (LmSASL, 1);

	sasl->connection = connection;
	sasl->username = g_strdup (username);
	sasl->password = g_strdup (password);
	sasl->server = g_strdup (server);
	sasl->handler = handler;

	sasl->features_cb  =
		lm_message_handler_new (features_cb,
					sasl,
					NULL);
	lm_connection_register_message_handler (connection,
						sasl->features_cb,
						LM_MESSAGE_TYPE_STREAM_FEATURES,
						LM_HANDLER_PRIORITY_FIRST);

	sasl->challenge_cb  =
		lm_message_handler_new (challenge_cb,
					sasl,
					NULL);
	lm_connection_register_message_handler (connection,
						sasl->challenge_cb,
						LM_MESSAGE_TYPE_CHALLENGE,
						LM_HANDLER_PRIORITY_FIRST);
	sasl->success_cb  =
		lm_message_handler_new (success_cb,
					sasl,
					NULL);
	lm_connection_register_message_handler (connection,
						sasl->success_cb,
						LM_MESSAGE_TYPE_SUCCESS,
						LM_HANDLER_PRIORITY_FIRST);

	sasl->failure_cb  =
		lm_message_handler_new (failure_cb,
					sasl,
					NULL);
	lm_connection_register_message_handler (connection,
						sasl->failure_cb,
						LM_MESSAGE_TYPE_FAILURE,
						LM_HANDLER_PRIORITY_FIRST);
	return sasl;
}

void
lm_sasl_free (LmSASL *sasl)
{
	g_return_if_fail (sasl != NULL);

	if (sasl->username)
		g_free (sasl->username);
	if (sasl->password)
		g_free (sasl->password);
	if (sasl->server)
		g_free (sasl->server);

	if (sasl->features_cb)
		lm_connection_unregister_message_handler (sasl->connection,
			sasl->features_cb, LM_MESSAGE_TYPE_STREAM_FEATURES);
	if (sasl->challenge_cb)
		lm_connection_unregister_message_handler (sasl->connection,
			sasl->challenge_cb, LM_MESSAGE_TYPE_CHALLENGE);

	g_free (sasl);
}

/* DIGEST-MD5 mechanism code from libgibber */

static gchar *
strndup_unescaped(const gchar *str, gsize len) {
	const gchar *s;
	gchar *d, *ret;

	ret = g_malloc0(len + 1);
	for (s = str, d = ret ; s < (str + len) ; s++, d++) {
		if (*s == '\\') s++;
		*d = *s;
	}

	return ret;
}

static GHashTable *
digest_md5_challenge_to_hash(const gchar * challenge) {
	const gchar *keystart, *keyend, *valstart;
	const gchar *c = challenge;
	gchar *key, *val;
	GHashTable *result = g_hash_table_new_full(g_str_hash, 
		g_str_equal, g_free, g_free);

	do { 
		keystart = c;
		for (; *c != '\0' && *c != '='; c++);

		if (*c == '\0' || c == keystart) goto error;

		keyend = c; 
		c++;

		if (*c == '"') {
			c++;
			valstart = c;
			for (; *c != '\0' && *c != '"'; c++);
			if (*c == '\0' || c == valstart) goto error;
			val = strndup_unescaped(valstart, c - valstart);
			c++;
		} else {
			valstart = c;
			for (; *c !=  '\0' && *c != ','; c++);
			if (c == valstart) goto error;
			val = g_strndup(valstart, c - valstart);
		}

		key = g_strndup(keystart, keyend - keystart);

		g_hash_table_insert(result, key, val);

		if (*c == ',') c++;
	} while (*c != '\0');

	return result;
error:
	g_debug ("Failed to parse challenge: %s", challenge);
	g_hash_table_destroy(result);
	return NULL;
}

static gchar *
md5_hex_hash(gchar *value, gsize len) {
	md5_byte_t digest_md5[16];
	md5_state_t md5_calc;
	GString *str = g_string_sized_new(32);
	int i;

	md5_init(&md5_calc);
	md5_append(&md5_calc, (const md5_byte_t *)value, len);
	md5_finish(&md5_calc, digest_md5);
	for (i = 0 ; i < 16 ; i++) {
		g_string_append_printf(str, "%02x", digest_md5[i]);
	}
	return g_string_free(str, FALSE);
}

static gchar *
digest_md5_generate_cnonce(void) {
	/* RFC 2831 recommends the the nonce to be either hexadecimal or base64 with
	 * at least 64 bits of entropy */
#define NR 8
	guint32 n[NR]; 
	int i;
	for (i = 0; i < NR; i++)
		n[i] = g_random_int();
	return base64_encode((gchar *)n, sizeof(n));
}

static gchar *
md5_prepare_response(LmSASL *sasl, GHashTable *challenge) {
	GString *response = g_string_new("");
	const gchar *realm, *nonce;
	gchar *a1, *a1h, *a2, *a2h, *kd, *kdh;
	gchar *cnonce = NULL;
	gchar *tmp;
	md5_byte_t digest_md5[16];
	md5_state_t md5_calc;
	gsize len;

	if (sasl->username == NULL || sasl->password == NULL) {
		g_debug ("%s: no username or password provided", G_STRFUNC);
		if (sasl->handler) {
			sasl->handler (sasl, sasl->connection, FALSE, "no username/password provided");
		}
		goto error;
	}

	nonce = g_hash_table_lookup (challenge, "nonce");
	if (nonce == NULL || nonce == '\0') {
		g_debug ("%s: server didn't provide a nonce in the challenge", G_STRFUNC);
		if (sasl->handler) {
			sasl->handler (sasl, sasl->connection, FALSE, "server error");
		}
		goto error;
	}

	cnonce = digest_md5_generate_cnonce();

	/* FIXME challenge can contain multiple realms */
	realm = g_hash_table_lookup (challenge, "realm");
	if (realm == NULL) {
		realm = sasl->server;
	}

	/* FIXME properly escape values */
	g_string_append_printf(response, "username=\"%s\"", sasl->username);
	g_string_append_printf(response, ",realm=\"%s\"", realm);
	g_string_append_printf(response, ",digest-uri=\"xmpp/%s\"", realm);
	g_string_append_printf(response, ",nonce=\"%s\",nc=00000001", nonce);
	g_string_append_printf(response, ",cnonce=\"%s\"", cnonce);
	/* FIXME should check if auth is in the cop challenge val */
	g_string_append_printf(response, ",qop=auth,charset=utf-8");

	tmp = g_strdup_printf("%s:%s:%s", sasl->username, realm, sasl->password);
	md5_init(&md5_calc);
	md5_append(&md5_calc, (const md5_byte_t *)tmp, strlen(tmp));
	md5_finish(&md5_calc, digest_md5);
	g_free(tmp);

	a1 = g_strdup_printf("0123456789012345:%s:%s", nonce, cnonce);
	len = strlen(a1);
	memcpy(a1, digest_md5, 16);
	a1h = md5_hex_hash(a1, len);

	a2 = g_strdup_printf("AUTHENTICATE:xmpp/%s", realm);
	a2h = md5_hex_hash(a2, strlen(a2));

	kd = g_strdup_printf("%s:%s:00000001:%s:auth:%s", a1h, nonce, cnonce, a2h);
	kdh = md5_hex_hash(kd, strlen(kd));
	g_string_append_printf(response, ",response=%s", kdh);

	g_free(kd);
	g_free(kdh);
	g_free(a2);
	g_free(a2h);

	/* Calculate the response we expect from the server */
	a2 = g_strdup_printf(":xmpp/%s", realm);
	a2h = md5_hex_hash(a2, strlen(a2));

	kd = g_strdup_printf("%s:%s:00000001:%s:auth:%s", a1h, nonce, cnonce, a2h);
	g_free (sasl->digest_md5_rspauth);
	sasl->digest_md5_rspauth = md5_hex_hash(kd, strlen(kd));

	g_free(a1);
	g_free(a1h);
	g_free(a2);
	g_free(a2h);
	g_free(kd);

out:
	g_free(cnonce);

	return response != NULL ? g_string_free(response, FALSE) : NULL;

error:
	g_string_free(response, TRUE);
	response = NULL;
	goto out;
}

static gboolean
digest_md5_send_initial_response(LmSASL *sasl, GHashTable *challenge) {
	LmMessage *msg;
	gchar *response, *response64;
	int result;

	response = md5_prepare_response(sasl, challenge);
	if (response == NULL) {
		return FALSE;
	}

	response64 = base64_encode((gchar *)response, strlen(response));

	msg = lm_message_new (NULL, LM_MESSAGE_TYPE_RESPONSE);
	lm_message_node_set_attributes (msg->node,
					"xmlns", XMPP_NS_SASL_AUTH,
					NULL);
	lm_message_node_set_value (msg->node, response64);

	result = lm_connection_send (sasl->connection, msg, NULL);

	g_free(response);
	g_free(response64);
	lm_message_unref (msg);

	if (!result)
		return FALSE;

	sasl->state = SASL_AUTH_STATE_DIGEST_MD5_SENT_AUTH_RESPONSE;

	return TRUE;
}

static gboolean
digest_md5_check_server_response(LmSASL *sasl, GHashTable *challenge) {
	LmMessage *msg;
	const gchar *rspauth;
	int result;

	rspauth = g_hash_table_lookup (challenge, "rspauth");
	if (rspauth == NULL) {
		g_debug ("%s: server sent an invalid reply (no rspauth)", G_STRFUNC);
		if (sasl->handler) {
			sasl->handler (sasl, sasl->connection, TRUE, "server error");
		}
		return FALSE;
	}

	if (strcmp(sasl->digest_md5_rspauth, rspauth)) {
		g_debug ("%s: server sent an invalid reply (rspauth not matching)", G_STRFUNC);
		if (sasl->handler) {
			sasl->handler (sasl, sasl->connection, TRUE, "server error");
		}
		return FALSE;
	}

	msg = lm_message_new (NULL, LM_MESSAGE_TYPE_RESPONSE);
	lm_message_node_set_attributes (msg->node,
					"xmlns", XMPP_NS_SASL_AUTH,
					NULL);

	result = lm_connection_send (sasl->connection, msg, NULL);
	lm_message_unref (msg);

	if (!result)
		return FALSE;

	sasl->state = SASL_AUTH_STATE_DIGEST_MD5_SENT_FINAL_RESPONSE;

	return TRUE;
}

static gboolean
digest_md5_handle_challenge(LmSASL *sasl, LmMessageNode *node)
{
	const gchar *encoded;
	gchar *challenge;
	gsize len;
	GHashTable *h;

	encoded = lm_message_node_get_value (node);
	if (!encoded) {
		g_debug ("%s: got empty challenge!", G_STRFUNC);
		return FALSE;
	}

	challenge = (gchar *) base64_decode (encoded, &len);
	h = digest_md5_challenge_to_hash (challenge);
	g_free(challenge);

	if (!h) {
		g_debug ("%s: server sent an invalid challenge", G_STRFUNC);
		if (sasl->handler) {
			sasl->handler (sasl, sasl->connection, FALSE, "server error");
		}
		return FALSE;
	}

	switch (sasl->state) {
	case SASL_AUTH_STATE_DIGEST_MD5_STARTED:
		digest_md5_send_initial_response(sasl, h); 
		break;
	case SASL_AUTH_STATE_DIGEST_MD5_SENT_AUTH_RESPONSE:
		digest_md5_check_server_response(sasl, h); 
		break;
	default:
		g_debug ("%s: server sent a challenge at the wrong time", G_STRFUNC);
		if (sasl->handler) {
			sasl->handler (sasl, sasl->connection, FALSE, "server error");
		}
		return FALSE;
	} 
	g_hash_table_destroy(h);
	return TRUE;
}

static LmHandlerResult
challenge_cb (LmMessageHandler *handler,
	      LmConnection *connection,
	      LmMessage *message,
	      gpointer user_data)
{
	LmSASL *sasl;
	const gchar *ns;
	
	ns = lm_message_node_get_attribute (message->node, "xmlns");
	if (!ns || strcmp (ns, XMPP_NS_SASL_AUTH))
		return LM_HANDLER_RESULT_ALLOW_MORE_HANDLERS;

	sasl = (LmSASL *) user_data;

	switch (sasl->auth_type) {
	case AUTH_TYPE_PLAIN:
		g_debug ("%s: server sent challenge for PLAIN mechanism", G_STRFUNC);
		if (sasl->handler) {
			sasl->handler (sasl, sasl->connection, FALSE, "server error");
		}
		break;
	case AUTH_TYPE_DIGEST:
		digest_md5_handle_challenge (sasl, message->node);
		break;
	default:
		g_assert_not_reached ();
	}

	return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static LmHandlerResult
success_cb (LmMessageHandler *handler,
	    LmConnection *connection,
	    LmMessage *message,
	    gpointer user_data)
{
	LmSASL *sasl;
	const gchar *ns;
	
	ns = lm_message_node_get_attribute (message->node, "xmlns");
	if (!ns || strcmp (ns, XMPP_NS_SASL_AUTH))
		return LM_HANDLER_RESULT_ALLOW_MORE_HANDLERS;

	sasl = (LmSASL *) user_data;

	switch (sasl->auth_type) {
	case AUTH_TYPE_PLAIN:
		if (sasl->state != SASL_AUTH_STATE_PLAIN_STARTED) {
			g_debug ("%s: server sent success before finishing auth", G_STRFUNC);
			if (sasl->handler) {
				sasl->handler (sasl, sasl->connection, FALSE, "server error");
			}
		}
		break;
	case AUTH_TYPE_DIGEST:
		if (sasl->state != SASL_AUTH_STATE_DIGEST_MD5_SENT_FINAL_RESPONSE) {
			g_debug ("%s: server sent success before finishing auth", G_STRFUNC);
			if (sasl->handler) {
				sasl->handler (sasl, sasl->connection, FALSE, "server error");
			}
		}
		break;
	default:
		g_assert_not_reached ();
	}

	g_debug ("%s: SASL authentication successful", G_STRFUNC);
	if (sasl->handler) {
		sasl->handler (sasl, sasl->connection, TRUE, NULL);
	}

	return LM_HANDLER_RESULT_REMOVE_MESSAGE;
	
}

static LmHandlerResult
failure_cb (LmMessageHandler *handler,
	    LmConnection *connection,
	    LmMessage *message,
	    gpointer user_data)
{
	LmSASL *sasl;
	const gchar *ns;
	const gchar *reason = "unknown reason";
	
	ns = lm_message_node_get_attribute (message->node, "xmlns");
	if (!ns || strcmp (ns, XMPP_NS_SASL_AUTH))
		return LM_HANDLER_RESULT_ALLOW_MORE_HANDLERS;

	sasl = (LmSASL *) user_data;

	if (message->node->children) {
		const gchar *r = lm_message_node_get_value (message->node->children);
		if (r) reason = r;
	}
	g_debug ("%s: SASL authentication failed: %s", G_STRFUNC, reason);
	if (sasl->handler) {
		sasl->handler (sasl, sasl->connection, FALSE, reason);
	}

	return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}


static gboolean
lm_sasl_start (LmSASL *sasl)
{
	LmMessage *auth_msg;
	gboolean result;
	const char *mech = NULL;

	auth_msg = lm_message_new (NULL, LM_MESSAGE_TYPE_AUTH);

	if (sasl->auth_type == AUTH_TYPE_PLAIN) {
      		GString *str = g_string_new("");
		gchar *cstr;

		mech = "PLAIN";
		sasl->state = SASL_AUTH_STATE_PLAIN_STARTED;

		if (sasl->username == NULL || sasl->password == NULL) {
			g_debug ("%s: no username or password provided", G_STRFUNC);
			if (sasl->handler) {
				sasl->handler (sasl, sasl->connection, FALSE, "no username/password provided");
			}
			return FALSE;
		}

		g_string_append_c(str, '\0');
		g_string_append(str, sasl->username);
		g_string_append_c(str, '\0');
		g_string_append(str, sasl->password);
		cstr = base64_encode((gchar *)str->str, str->len);

		lm_message_node_set_value (auth_msg->node, cstr);

		g_string_free(str, TRUE);
		g_free(cstr);

		/* Here we say the Google magic word. Bad Google. */
		lm_message_node_set_attributes (auth_msg->node,
						"xmlns:ga", "http://www.google.com/talk/protocol/auth",
						"ga:client-uses-full-bind-result", "true",
						NULL);

	} else if (sasl->auth_type == AUTH_TYPE_DIGEST) {
		mech = "DIGEST-MD5";
		sasl->state = SASL_AUTH_STATE_DIGEST_MD5_STARTED;
	}

	lm_message_node_set_attributes (auth_msg->node,
					"xmlns", XMPP_NS_SASL_AUTH,
					"mechanism", mech,
					NULL);

	result = lm_connection_send (sasl->connection, auth_msg, NULL);
	lm_message_unref (auth_msg);
	if (!result)
		return FALSE;

	return TRUE;
}

static gboolean
lm_sasl_authenticate (LmSASL *sasl, LmMessageNode *mechanisms)
{
	LmMessageNode *m;
	AuthType auth_type = 0;
	const gchar *ns;

	ns = lm_message_node_get_attribute (mechanisms, "xmlns");
	if (!ns || strcmp (ns, XMPP_NS_SASL_AUTH))
		return FALSE;

	for (m = mechanisms->children; m; m = m->next) {
		const gchar *name = lm_message_node_get_value (m);
		if (!name)
			continue;
		if (!strcmp (name, "PLAIN")) {
			auth_type |= AUTH_TYPE_PLAIN;
			continue;
		}
		if (!strcmp (name, "DIGEST-MD5")) {
			auth_type |= AUTH_TYPE_DIGEST;
			continue;
		}
		g_debug ("%s: unknown SASL auth mechanism: %s", G_STRFUNC, name);
	}
	if (auth_type == 0) {
		g_debug ("%s: no supported SASL auth mechanisms found", G_STRFUNC);
		return FALSE;
	}
	/* Prefer DIGEST */
	if (auth_type & AUTH_TYPE_DIGEST) {
		sasl->auth_type = AUTH_TYPE_DIGEST;
		return lm_sasl_start (sasl);
	} else if (auth_type & AUTH_TYPE_PLAIN) {
		sasl->auth_type = AUTH_TYPE_PLAIN;
		return lm_sasl_start (sasl);
	} 

	g_assert_not_reached ();
	return FALSE;
}

static LmHandlerResult
features_cb (LmMessageHandler *handler,
	     LmConnection *connection,
	     LmMessage *message,
	     gpointer user_data)
{
    	LmMessageNode *mechanisms;
	LmSASL *sasl;

	mechanisms = lm_message_node_find_child (message->node, "mechanisms");
	if (!mechanisms)
		return LM_HANDLER_RESULT_ALLOW_MORE_HANDLERS;

	sasl = (LmSASL *) user_data;

	lm_sasl_authenticate (sasl, mechanisms);

	return LM_HANDLER_RESULT_ALLOW_MORE_HANDLERS;
}


