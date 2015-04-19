/********************************************************************\
  * BitlBee -- An IRC to other IM-networks gateway                     *
  *                                                                    *
  * Copyright 2002-2012 Wilmer van der Gaast and others                *
  \********************************************************************/

/* MSN module - Notification server callbacks                           */

/*
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License with
  the Debian GNU/Linux distribution in /usr/share/common-licenses/GPL;
  if not, write to the Free Software Foundation, Inc., 51 Franklin St.,
  Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <ctype.h>
#include <sys/utsname.h>
#include "nogaim.h"
#include "msn.h"
#include "md5.h"
#include "sha1.h"
#include "soap.h"
#include "xmltree.h"
#include "ssl_client.h"

static gboolean msn_ns_connected(gpointer data, int source, void *scd, b_input_condition cond);
static gboolean msn_ns_callback(gpointer data, gint source, b_input_condition cond);

static void msn_ns_send_adl_start(struct im_connection *ic);
static void msn_ns_send_adl(struct im_connection *ic);
static void msn_ns_structured_message(struct msn_data *md, char *msg, int msglen, char **cmd);
static void msn_ns_sdg(struct msn_data *md, char *who, char **parts, char *action);
static void msn_ns_nfy(struct msn_data *md, char *who, char **parts, char *action, gboolean is_put);

int msn_ns_write(struct im_connection *ic, const char *fmt, ...)
{
	struct msn_data *md = ic->proto_data;
	va_list params;
	char *out;
	size_t len;
	int st;

	va_start(params, fmt);
	out = g_strdup_vprintf(fmt, params);
	va_end(params);

	if (getenv("BITLBEE_DEBUG")) {
		fprintf(stderr, "\n\x1b[91m>>>[NS] %s\n\x1b[97m", out);
	}

	len = strlen(out);

	if (md->is_http) {
		st = len;
		msn_gw_write(md->gw, out, len);
	} else {
		st = ssl_write(md->ssl, out, len);
	}

	g_free(out);
	if (st != len) {
		imcb_error(ic, "Short write() to main server");
		imc_logout(ic, TRUE);
		return 0;
	}

	return 1;
}

int msn_ns_write_cmd(struct im_connection *ic, const char *cmd, const char *params, const char *payload)
{
	struct msn_data *md = ic->proto_data;
	int trid = ++md->trId;
	const char *headers = "\r\n"; /* not needed yet */
	size_t len = strlen(headers) + strlen(payload);

	if (params && *params) {
		return msn_ns_write(ic, "%s %d %s %zd\r\n%s%s", cmd, trid, params, len, headers, payload);
	} else {
		return msn_ns_write(ic, "%s %d %zd\r\n%s%s", cmd, trid, len, headers, payload);
	}
}

gboolean msn_ns_connect(struct im_connection *ic, const char *host, int port)
{
	struct msn_data *md = ic->proto_data;

	if (md->fd >= 0) {
		closesocket(md->fd);
	}

	if (md->is_http) {
		md->gw = msn_gw_new(ic);
		md->gw->callback = msn_ns_callback;
		msn_ns_connected(md, 0, NULL, B_EV_IO_READ);
	} else {
		md->ssl = ssl_connect((char *) host, port, TRUE, msn_ns_connected, md);
		md->fd = md->ssl ? ssl_getfd(md->ssl) : -1;
		if (md->fd < 0) {
			imcb_error(ic, "Could not connect to server");
			imc_logout(ic, TRUE);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean msn_ns_connected(gpointer data, int source, void *scd, b_input_condition cond)
{
	struct msn_data *md = data;
	struct im_connection *ic = md->ic;

	if (!scd && !md->is_http) {
		md->ssl = NULL;
		imcb_error(ic, "Could not connect to server");
		imc_logout(ic, TRUE);
		return FALSE;
	}

	g_free(md->rxq);
	md->rxlen = 0;
	md->rxq = g_new0(char, 1);

	if (md->uuid == NULL) {
		struct utsname name;
		sha1_state_t sha[1];

		/* UUID == SHA1("BitlBee" + my hostname + MSN username) */
		sha1_init(sha);
		sha1_append(sha, (void *) "BitlBee", 7);
		if (uname(&name) == 0) {
			sha1_append(sha, (void *) name.nodename, strlen(name.nodename));
		}
		sha1_append(sha, (void *) ic->acc->user, strlen(ic->acc->user));
		md->uuid = sha1_random_uuid(sha);
		memcpy(md->uuid, "b171be3e", 8);   /* :-P */
	}

	if (msn_ns_write_cmd(ic, "CNT", "CON", "<connect><ver>2</ver><agent><os>winnt</os><osVer>5.2</osVer><proc>x86</proc><lcid>en-us</lcid></agent></connect>")) {
		if (!md->is_http) {
			md->inpa = b_input_add(md->fd, B_EV_IO_READ, msn_ns_callback, md);
		}
		imcb_log(ic, "Connected to server, waiting for reply");
	}

	return FALSE;
}

void msn_ns_close(struct msn_data *md)
{
	if (md->gw) {
		msn_gw_free(md->gw);
	}

	if (md->ssl) {
		ssl_disconnect(md->ssl);
		b_event_remove(md->inpa);
	}

	md->ssl = NULL;
	md->fd = md->inpa = -1;

	g_free(md->rxq);
	g_free(md->cmd_text);

	md->rxlen = 0;
	md->rxq = NULL;
	md->cmd_text = NULL;
}

static gboolean msn_ns_callback(gpointer data, gint source, b_input_condition cond)
{
	struct msn_data *md = data;
	struct im_connection *ic = md->ic;
	char *bytes;
	int st;

	if (md->is_http) {
		st = msn_gw_read(md->gw, &bytes);
	} else {
		bytes = g_malloc(1024);
		st = ssl_read(md->ssl, bytes, 1024);
	}

	if (st == 0 || (st < 0 && (md->is_http || !ssl_sockerr_again(md->ssl)))) {
		imcb_error(ic, "Error while reading from server");
		imc_logout(ic, TRUE);
		g_free(bytes);
		return FALSE;
	}

	msn_queue_feed(md, bytes, st);

	g_free(bytes);

	if (!md->is_http && ssl_pending(md->ssl)) {
		return msn_ns_callback(data, source, cond);
	}

	msn_handler(md);

	return TRUE;
	
}

int msn_ns_command(struct msn_data *md, char **cmd, int num_parts, char *msg, int msglen)
{
	struct im_connection *ic = md->ic;
	struct xt_node *xml;

	if (strcmp(cmd[0], "XFR") == 0) {
		struct xt_node *target;
		char *server, *p;
		int port, st;

		if (!(xml = xt_from_string(msg + 2, msglen - 2)) ||
		    !(target = xt_find_node(xml->children, "target")) ||
		    !(server = target->text) ||
		    !(p = strchr(server, ':'))) {
			return 1;
		}
		
		server = target->text;
		*p = 0;
		port = atoi(p + 1);

		b_event_remove(md->inpa);
		md->inpa = -1;

		imcb_log(ic, "Transferring to other server");

		st = msn_ns_connect(ic, server, port);

		xt_free_node(xml);

		return st;

	} else if (strcmp(cmd[0], "CNT") == 0) {
		msn_soap_passport_sso_request(ic);

		/* continues in msn_auth_got_passport_token */

	} else if (strcmp(cmd[0], "ATH") == 0) {
		char *payload;

		if (md->flags & MSN_DONE_BND) {
			return 1;
		}

		md->flags |= MSN_DONE_BND;

		// BND
		payload = g_markup_printf_escaped(
			"<msgr><ver>1</ver><client><name>Skype</name><ver>2/4.3.0.37/174</ver></client>"
			"<epid>%s</epid></msgr>\r\n",
			md->uuid);

		msn_ns_write_cmd(ic, "BND", "CON\\MSGR", payload);

		g_free(payload);

	} else if (strcmp(cmd[0], "BND") == 0) {
		struct xt_node *node;
		char *nonce, *resp, *payload;

		if (!(xml = xt_from_string(msg + 2, msglen - 2)) ||
		    !(node = xt_find_node(xml->children, "nonce")) ||
		    !(nonce = node->text)) {
			return 1;
		}

		resp = msn_p11_challenge(nonce);

		// PUT MSGR\CHALLENGE
		payload = g_markup_printf_escaped(
			"<challenge><appId>%s</appId><response>%s</response></challenge>",
			MSNP11_PROD_ID, resp);

		msn_ns_write_cmd(ic, "PUT", "MSGR\\CHALLENGE", payload);

		imcb_log(ic, "Authenticated, getting buddy list");
		msn_soap_memlist_request(ic);

		xt_free_node(xml);
		g_free(payload);
		g_free(resp);

	} else if (strcmp(cmd[0], "PUT") == 0) {
		/* We could keep track TrIDs... or we could guess what this PUT means */
		if ((md->flags & MSN_DONE_BND) && !(md->flags & MSN_DONE_ADL)) {
			msn_ns_send_adl(ic);
			return msn_ns_finish_login(ic);
		}
	} else if (strcmp(cmd[0], "OUT") == 0) {
		imcb_error(ic, "Session terminated by remote server (%s)", cmd[1] ? cmd[1] : "reason unknown");
		imc_logout(ic, TRUE);
		return(0);
	} else if (strcmp(cmd[0], "QNG") == 0) {
		ic->flags |= OPT_PONGED;
	} else if (g_ascii_isdigit(cmd[0][0])) {
		int num = atoi(cmd[0]);
		const struct msn_status_code *err = msn_status_by_number(num);

		imcb_error(ic, "Error reported by MSN server: %s", err->text);

		if (err->flags & STATUS_FATAL) {
			imc_logout(ic, TRUE);
			return(0);
		}
	} else if ((strcmp(cmd[0], "SDG") == 0) || (strcmp(cmd[0], "NFY") == 0)) {
		msn_ns_structured_message(md, msg, msglen, cmd);
	} else {
		imcb_error(ic, "Received unknown command from main server: %s", cmd[0]);
	}

	return(1);
}

int msn_ns_message(struct msn_data *md, char *msg, int msglen, char **cmd, int num_parts)
{
	struct im_connection *ic = md->ic;
	char *body;
	int blen = 0;

	if ((body = strstr(msg, "\r\n\r\n"))) {
		body += 4;
		blen = msglen - (body - msg);
	}

	if (strcmp(cmd[0], "MSG") == 0) {
		if (g_strcasecmp(cmd[1], "Hotmail") == 0) {
			char *ct = get_rfc822_header(msg, "Content-Type:", msglen);

			if (!ct) {
				return(1);
			}

			if (g_strncasecmp(ct, "application/x-msmsgssystemmessage", 33) == 0) {
				char *mtype;
				char *arg1;

				if (!body) {
					return(1);
				}

				mtype = get_rfc822_header(body, "Type:", blen);
				arg1 = get_rfc822_header(body, "Arg1:", blen);

				if (mtype && strcmp(mtype, "1") == 0) {
					if (arg1) {
						imcb_log(ic, "The server is going down for maintenance in %s minutes.",
						         arg1);
					}
				}

				g_free(arg1);
				g_free(mtype);
			} else if (g_strncasecmp(ct, "text/x-msmsgsprofile", 20) == 0) {
				/* We don't care about this profile for now... */
			} else if (g_strncasecmp(ct, "text/x-msmsgsinitialemailnotification", 37) == 0) {
				if (set_getbool(&ic->acc->set, "mail_notifications")) {
					char *inbox = get_rfc822_header(body, "Inbox-Unread:", blen);
					char *folders = get_rfc822_header(body, "Folders-Unread:", blen);

					if (inbox && folders) {
						imcb_log(ic,
						         "INBOX contains %s new messages, plus %s messages in other folders.", inbox,
						         folders);
					}

					g_free(inbox);
					g_free(folders);
				}
			} else if (g_strncasecmp(ct, "text/x-msmsgsemailnotification", 30) == 0) {
				if (set_getbool(&ic->acc->set, "mail_notifications")) {
					char *from = get_rfc822_header(body, "From-Addr:", blen);
					char *fromname = get_rfc822_header(body, "From:", blen);

					if (from && fromname) {
						imcb_log(ic, "Received an e-mail message from %s <%s>.", fromname,
						         from);
					}

					g_free(from);
					g_free(fromname);
				}
			} else if (g_strncasecmp(ct, "text/x-msmsgsactivemailnotification", 35) == 0) {
				/* Notification that a message has been read... Ignore it */
			} else {
				debug("Can't handle %s packet from notification server", ct);
			}

			g_free(ct);
		}
	} else if (strcmp(cmd[0], "ADL") == 0) {
		struct xt_node *adl, *d, *c;

		if (!(adl = xt_from_string(msg, msglen))) {
			return 1;
		}

		for (d = adl->children; d; d = d->next) {
			char *dn;
			if (strcmp(d->name, "d") != 0 ||
			    (dn = xt_find_attr(d, "n")) == NULL) {
				continue;
			}
			for (c = d->children; c; c = c->next) {
				bee_user_t *bu;
				struct msn_buddy_data *bd;
				char *cn, *handle, *f, *l;
				int flags;

				if (strcmp(c->name, "c") != 0 ||
				    (l = xt_find_attr(c, "l")) == NULL ||
				    (cn = xt_find_attr(c, "n")) == NULL) {
					continue;
				}

				/* FIXME: Use "t" here, guess I should just add it
				   as a prefix like elsewhere in the protocol. */
				handle = g_strdup_printf("%s@%s", cn, dn);
				if (!((bu = bee_user_by_handle(ic->bee, ic, handle)) ||
				      (bu = bee_user_new(ic->bee, ic, handle, 0)))) {
					g_free(handle);
					continue;
				}
				g_free(handle);
				bd = bu->data;

				if ((f = xt_find_attr(c, "f"))) {
					http_decode(f);
					imcb_rename_buddy(ic, bu->handle, f);
				}

				flags = atoi(l) & 15;
				if (bd->flags != flags) {
					bd->flags = flags;
					msn_buddy_ask(bu);
				}
			}
		}
	}

	return 1;
}

static void msn_ns_structured_message(struct msn_data *md, char *msg, int msglen, char **cmd)
{
	char **parts = NULL;
	char *semicolon = NULL;
	char *action = NULL;
	char *from = NULL;
	char *who = NULL;

	parts = g_strsplit(msg, "\r\n\r\n", 4);

	if (!(from = get_rfc822_header(parts[0], "From", 0))) {
		goto cleanup;
	}

	/* either the semicolon or the end of the string */
	semicolon = strchr(from, ';') ? : (from + strlen(from));

	who = g_strndup(from + 2, semicolon - from - 2);

	if ((strcmp(cmd[0], "SDG") == 0) && (action = get_rfc822_header(parts[2], "Message-Type", 0))) {
		msn_ns_sdg(md, who, parts, action);

	} else if ((strcmp(cmd[0], "NFY") == 0) && (action = get_rfc822_header(parts[2], "Uri", 0))) {
		gboolean is_put = (strcmp(cmd[2], "MSGR\\PUT") == 0);
		msn_ns_nfy(md, who, parts, action, is_put);
	}

cleanup:
	g_strfreev(parts);
	g_free(action);
	g_free(from);
	g_free(who);
}

static void msn_ns_sdg(struct msn_data *md, char *who, char **parts, char *action)
{
	struct im_connection *ic = md->ic;

	if (strcmp(action, "Control/Typing") == 0) {
		imcb_buddy_typing(ic, who, OPT_TYPING);
	} else if (strcmp(action, "Text") == 0) {
		imcb_buddy_msg(ic, who, parts[3], 0, 0);
	}
}

static void msn_ns_nfy(struct msn_data *md, char *who, char **parts, char *action, gboolean is_put)
{
	struct im_connection *ic = md->ic;
	struct xt_node *body = NULL;
	struct xt_node *s = NULL;
	const char *state = NULL;
	char *nick = NULL;
	char *psm = NULL;
	int flags = OPT_LOGGED_IN;

	if (strcmp(action, "/user") != 0) {
		return;
	}

	if (!(body = xt_from_string(parts[3], 0))) {
		goto cleanup;
	}

	s = body->children;
	while ((s = xt_find_node(s, "s"))) {
		struct xt_node *s2;
		char *n = xt_find_attr(s, "n");  /* service name: IM, PE, etc */

		if (strcmp(n, "IM") == 0) {
			/* IM has basic presence information */
			if (!is_put) {
				/* NFY DEL with a <s> usually means log out from the last endpoint */
				flags &= ~OPT_LOGGED_IN;
				break;
			}

			s2 = xt_find_node(s->children, "Status");
			if (s2 && s2->text_len) {
				const struct msn_away_state *msn_state = msn_away_state_by_code(s2->text);
				state = msn_state->name;
				if (msn_state != msn_away_state_list) {
					flags |= OPT_AWAY;
				}
			}
		} else if (strcmp(n, "PE") == 0) {
			if ((s2 = xt_find_node(s->children, "PSM")) && s2->text_len) {
				psm = s2->text;
			}
			if ((s2 = xt_find_node(s->children, "FriendlyName")) && s2->text_len) {
				nick = s2->text;
			}
		}
		s = s->next;
	}

	imcb_buddy_status(ic, who, flags, state, psm);

	if (nick) {
		imcb_rename_buddy(ic, who, nick);
	}

cleanup:
	xt_free_node(body);
}

void msn_auth_got_passport_token(struct im_connection *ic, const char *token, const char *error)
{
	struct msn_data *md;
	char *payload = NULL;

	/* Dead connection? */
	if (g_slist_find(msn_connections, ic) == NULL) {
		return;
	}

	md = ic->proto_data;

	if (!token) {
		imcb_error(ic, "Error during Passport authentication: %s", error);
		imc_logout(ic, TRUE);
	}

	// ATH
	payload = g_markup_printf_escaped(
		"<user><ssl-compact-ticket>%s</ssl-compact-ticket>"
		"<ssl-site-name>chatservice.live.com</ssl-site-name></user>",
		md->tokens[0]);

	msn_ns_write_cmd(ic, "ATH", "CON\\USER", payload);

	g_free(payload);

	/* continues in msn_ns_command */
}

void msn_auth_got_contact_list(struct im_connection *ic)
{
	/* Dead connection? */
	if (g_slist_find(msn_connections, ic) == NULL) {
		return;
	}

	msn_ns_send_adl_start(ic);
	msn_ns_finish_login(ic);
}

static gboolean msn_ns_send_adl_1(gpointer key, gpointer value, gpointer data)
{
	struct xt_node *adl = data, *d, *c, *s;
	struct bee_user *bu = value;
	struct msn_buddy_data *bd = bu->data;
	struct msn_data *md = bu->ic->proto_data;
	char handle[strlen(bu->handle) + 1];
	char *domain;
	char l[4];

	if ((bd->flags & 7) == 0 || (bd->flags & MSN_BUDDY_ADL_SYNCED)) {
		return FALSE;
	}

	strcpy(handle, bu->handle);
	if ((domain = strchr(handle, '@')) == NULL) {    /* WTF */
		return FALSE;
	}
	*domain = '\0';
	domain++;

	if ((d = adl->children) == NULL ||
	    g_strcasecmp(xt_find_attr(d, "n"), domain) != 0) {
		d = xt_new_node("d", NULL, NULL);
		xt_add_attr(d, "n", domain);
		xt_insert_child(adl, d);
	}

	g_snprintf(l, sizeof(l), "%d", bd->flags & 7);
	c = xt_new_node("c", NULL, NULL);
	xt_add_attr(c, "n", handle);
	xt_add_attr(c, "t", "1");   /* FIXME: Network type, i.e. 32 for Y!MSG */
	s = xt_new_node("s", NULL, NULL);
	xt_add_attr(s, "n", "IM");
	xt_add_attr(s, "l", l);
	xt_insert_child(c, s);
	xt_insert_child(d, c);

	/* Do this in batches of 100. */
	bd->flags |= MSN_BUDDY_ADL_SYNCED;
	return (--md->adl_todo % 140) == 0;
}

static void msn_ns_send_adl(struct im_connection *ic)
{
	struct xt_node *adl;
	struct msn_data *md = ic->proto_data;
	char *adls;

	adl = xt_new_node("ml", NULL, NULL);
	xt_add_attr(adl, "l", "1");
	g_tree_foreach(md->domaintree, msn_ns_send_adl_1, adl);
	if (adl->children == NULL) {
		/* This tells the caller that we're done now. */
		md->adl_todo = -1;
		xt_free_node(adl);
		return;
	}

	adls = xt_to_string(adl);
	xt_free_node(adl);
	msn_ns_write_cmd(ic, "PUT", "MSGR\\CONTACTS", adls);
	g_free(adls);
}

static void msn_ns_send_adl_start(struct im_connection *ic)
{
	struct msn_data *md;
	GSList *l;

	/* Dead connection? */
	if (g_slist_find(msn_connections, ic) == NULL) {
		return;
	}

	md = ic->proto_data;
	md->adl_todo = 0;
	for (l = ic->bee->users; l; l = l->next) {
		bee_user_t *bu = l->data;
		struct msn_buddy_data *bd = bu->data;

		if (bu->ic != ic || (bd->flags & 7) == 0) {
			continue;
		}

		bd->flags &= ~MSN_BUDDY_ADL_SYNCED;
		md->adl_todo++;
	}

	msn_ns_send_adl(ic);
}

int msn_ns_finish_login(struct im_connection *ic)
{
	struct msn_data *md = ic->proto_data;

	if (ic->flags & OPT_LOGGED_IN) {
		return 1;
	}

	if (md->adl_todo < 0) {
		md->flags |= MSN_DONE_ADL;
	}

	if ((md->flags & MSN_DONE_ADL) && (md->flags & MSN_GOT_PROFILE)) {
		imcb_connected(ic);
	}

	return 1;
}

// TODO: typing notifications, nudges lol, etc
int msn_ns_sendmessage(struct im_connection *ic, bee_user_t *bu, const char *text)
{
	struct msn_data *md = ic->proto_data;
	int retval = 0;
	char *buf;

	if (strncmp(text, "\r\r\r", 3) == 0) {
		/* Err. Shouldn't happen but I guess it can. Don't send others
		   any of the "SHAKE THAT THING" messages. :-D */
		return 1;
	}

	buf = g_strdup_printf(MSN_MESSAGE_HEADERS, bu->handle, ic->acc->user, md->uuid, strlen(text), text);
	retval = msn_ns_write_cmd(ic, "SDG", "MSGR", buf);
	g_free(buf);
	return retval;
}
