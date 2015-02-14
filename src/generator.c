/*
 * Copyright 2014-2015 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <jansson.h>
#include <string.h>
#include <unistd.h>

#include "ckpool.h"
#include "libckpool.h"
#include "generator.h"
#include "bitcoin.h"
#include "uthash.h"
#include "utlist.h"

struct notify_instance {
	/* Hash table data */
	UT_hash_handle hh;
	int id;

	char prevhash[68];
	char *jobid;
	char *coinbase1;
	char *coinbase2;
	int coinb1len;
	int merkles;
	char merklehash[16][68];
	char nbit[12];
	char ntime[12];
	char bbversion[12];
	bool clean;

	time_t notify_time;
};

typedef struct notify_instance notify_instance_t;

struct share_msg {
	UT_hash_handle hh;
	int64_t id; // Our own id for submitting upstream

	int client_id;
	int msg_id; // Stratum message id from client
	time_t submit_time;
};

typedef struct share_msg share_msg_t;

struct stratum_msg {
	struct stratum_msg *next;
	struct stratum_msg *prev;

	json_t *json_msg;
	int client_id;
};

typedef struct stratum_msg stratum_msg_t;

struct pass_msg {
	connsock_t *cs;
	char *msg;
};

typedef struct pass_msg pass_msg_t;

typedef struct proxy_instance proxy_instance_t;

/* Per proxied pool instance data */
struct proxy_instance {
	UT_hash_handle hh;
	proxy_instance_t *next; /* For dead proxy list */
	proxy_instance_t *prev; /* For dead proxy list */

	ckpool_t *ckp;
	connsock_t *cs;
	server_instance_t *si;
	bool passthrough;
	int id; /* Proxy server id, or subproxy id if this is a subproxy */

	const char *auth;
	const char *pass;

	char *enonce1;
	char *enonce1bin;
	int nonce1len;
	char *sessionid;
	int nonce2len;

	tv_t last_message;

	double diff;
	tv_t last_share;

	bool no_sessionid; /* Doesn't support session id resume on subscribe */
	bool no_params; /* Doesn't want any parameters on subscribe */

	bool disabled; /* Subproxy no longer to be used */
	bool reconnect; /* We need to drop and reconnect */
	bool alive;

	pthread_mutex_t notify_lock;
	notify_instance_t *notify_instances;
	notify_instance_t *current_notify;

	pthread_t pth_precv;
	pthread_t pth_psend;
	pthread_mutex_t psend_lock;
	pthread_cond_t psend_cond;

	stratum_msg_t *psends;

	pthread_mutex_t share_lock;
	share_msg_t *shares;
	int64_t share_id;

	ckmsgq_t *passsends;	// passthrough sends

	char_entry_t *recvd_lines; /* Linked list of unprocessed messages */

	time_t reconnect_time;

	int epfd; /* Epoll fd used by the parent proxy */

	pthread_mutex_t proxy_lock; /* Lock protecting hashlist of proxies */
	int64_t clients_per_proxy; /* How many clients can connect to each subproxy */
	int64_t client_headroom; /* How many more clients can we connect */
	proxy_instance_t *proxy; /* Parent proxy of subproxies */
	proxy_instance_t *subproxies; /* Hashlist of subproxies of this proxy */
	int subproxy_count; /* Number of subproxies */
};

/* Private data for the generator */
struct generator_data {
	ckpool_t *ckp;
	pthread_mutex_t lock; /* Lock protecting linked lists */
	proxy_instance_t *proxies; /* Hash list of all proxies */
	proxy_instance_t *proxy; /* Current proxy */
	proxy_instance_t *dead_proxies; /* Disabled proxies */
	int proxy_notify_id;	// Globally increasing notify id
	ckmsgq_t *srvchk;	// Server check message queue
};

typedef struct generator_data gdata_t;

static bool server_alive(ckpool_t *ckp, server_instance_t *si, bool pinging)
{
	char *userpass = NULL;
	bool ret = false;
	connsock_t *cs;
	gbtbase_t *gbt;

	cs = &si->cs;
	/* Has this server already been reconnected? */
	if (cs->fd > 0)
		return true;
	if (!extract_sockaddr(si->url, &cs->url, &cs->port)) {
		LOGWARNING("Failed to extract address from %s", si->url);
		return ret;
	}
	userpass = strdup(si->auth);
	realloc_strcat(&userpass, ":");
	realloc_strcat(&userpass, si->pass);
	cs->auth = http_base64(userpass);
	dealloc(userpass);
	if (!cs->auth) {
		LOGWARNING("Failed to create base64 auth from %s", userpass);
		return ret;
	}

	cs->fd = connect_socket(cs->url, cs->port);
	if (cs->fd < 0) {
		if (!pinging)
			LOGWARNING("Failed to connect socket to %s:%s !", cs->url, cs->port);
		return ret;
	}

	/* Test we can connect, authorise and get a block template */
	gbt = ckzalloc(sizeof(gbtbase_t));
	si->data = gbt;
	if (!gen_gbtbase(cs, gbt)) {
		if (!pinging) {
			LOGINFO("Failed to get test block template from %s:%s!",
				cs->url, cs->port);
		}
		goto out;
	}
	clear_gbtbase(gbt);
	if (!validate_address(cs, ckp->btcaddress)) {
		LOGWARNING("Invalid btcaddress: %s !", ckp->btcaddress);
		goto out;
	}
	ret = true;
out:
	if (!ret) {
		/* Close and invalidate the file handle */
		Close(cs->fd);
	} else
		keep_sockalive(cs->fd);
	return ret;
}

/* Find the highest priority server alive and return it */
static server_instance_t *live_server(ckpool_t *ckp)
{
	server_instance_t *alive = NULL;
	connsock_t *cs;
	int i;

	LOGDEBUG("Attempting to connect to bitcoind");
retry:
	if (!ping_main(ckp))
		goto out;

	for (i = 0; i < ckp->btcds; i++) {
		server_instance_t *si = ckp->servers[i];

		if (server_alive(ckp, si, false)) {
			alive = si;
			break;
		}
	}
	if (!alive) {
		LOGWARNING("CRITICAL: No bitcoinds active!");
		sleep(5);
		goto retry;
	}
	cs = &alive->cs;
	LOGINFO("Connected to live server %s:%s", cs->url, cs->port);
out:
	send_proc(ckp->connector, alive ? "accept" : "reject");
	return alive;
}

static void kill_server(server_instance_t *si)
{
	connsock_t *cs;

	if (!si) // This shouldn't happen
		return;

	LOGNOTICE("Killing server");
	cs = &si->cs;
	Close(cs->fd);
	empty_buffer(cs);
	dealloc(cs->url);
	dealloc(cs->port);
	dealloc(cs->auth);
	dealloc(si->data);
}

static int gen_loop(proc_instance_t *pi)
{
	int sockd = -1, ret = 0, selret;
	server_instance_t *si = NULL;
	bool reconnecting = false;
	unixsock_t *us = &pi->us;
	ckpool_t *ckp = pi->ckp;
	gdata_t *gdata = ckp->data;
	bool started = false;
	char *buf = NULL;
	connsock_t *cs;
	gbtbase_t *gbt;
	char hash[68];

reconnect:
	Close(sockd);
	if (si) {
		kill_server(si);
		reconnecting = true;
	}
	si = live_server(ckp);
	if (!si)
		goto out;

	gbt = si->data;
	cs = &si->cs;
	if (reconnecting) {
		LOGWARNING("Failed over to bitcoind: %s:%s", cs->url, cs->port);
		reconnecting = false;
	}

retry:
	Close(sockd);
	ckmsgq_add(gdata->srvchk, si);

	do {
		selret = wait_read_select(us->sockd, 5);
		if (!selret && !ping_main(ckp)) {
			LOGEMERG("Generator failed to ping main process, exiting");
			ret = 1;
			goto out;
		}
	} while (selret < 1);

	if (unlikely(cs->fd < 0)) {
		LOGWARNING("Bitcoind socket invalidated, will attempt failover");
		goto reconnect;
	}

	sockd = accept(us->sockd, NULL, NULL);
	if (sockd < 0) {
		LOGEMERG("Failed to accept on generator socket");
		ret = 1;
		goto out;
	}

	dealloc(buf);
	buf = recv_unix_msg(sockd);
	if (!buf) {
		LOGWARNING("Failed to get message in gen_loop");
		goto retry;
	}
	LOGDEBUG("Generator received request: %s", buf);
	if (cmdmatch(buf, "shutdown")) {
		ret = 0;
		goto out;
	}
	if (cmdmatch(buf, "getbase")) {
		if (!gen_gbtbase(cs, gbt)) {
			LOGWARNING("Failed to get block template from %s:%s",
				   cs->url, cs->port);
			send_unix_msg(sockd, "Failed");
			goto reconnect;
		} else {
			char *s = json_dumps(gbt->json, JSON_NO_UTF8);

			send_unix_msg(sockd, s);
			free(s);
			clear_gbtbase(gbt);
		}
	} else if (cmdmatch(buf, "getbest")) {
		if (si->notify)
			send_unix_msg(sockd, "notify");
		else if (!get_bestblockhash(cs, hash)) {
			LOGINFO("No best block hash support from %s:%s",
				cs->url, cs->port);
			send_unix_msg(sockd, "failed");
		} else {
			if (unlikely(!started)) {
				started = true;
				LOGWARNING("%s generator ready", ckp->name);
			}
			send_unix_msg(sockd, hash);
		}
	} else if (cmdmatch(buf, "getlast")) {
		int height;

		if (si->notify)
			send_unix_msg(sockd, "notify");
		else if ((height = get_blockcount(cs)) == -1) {
			send_unix_msg(sockd,  "failed");
			goto reconnect;
		} else {
			LOGDEBUG("Height: %d", height);
			if (!get_blockhash(cs, height, hash)) {
				send_unix_msg(sockd, "failed");
				goto reconnect;
			} else {
				if (unlikely(!started)) {
					started = true;
					LOGWARNING("%s generator ready", ckp->name);
				}

				send_unix_msg(sockd, hash);
				LOGDEBUG("Hash: %s", hash);
			}
		}
	} else if (cmdmatch(buf, "submitblock:")) {
		char blockmsg[80];
		bool ret;

		LOGNOTICE("Submitting block data!");
		ret = submit_block(cs, buf + 12 + 64 + 1);
		memset(buf + 12 + 64, 0, 1);
		sprintf(blockmsg, "%sblock:%s", ret ? "" : "no", buf + 12);
		send_proc(ckp->stratifier, blockmsg);
	} else if (cmdmatch(buf, "checkaddr:")) {
		if (validate_address(cs, buf + 10))
			send_unix_msg(sockd, "true");
		else
			send_unix_msg(sockd, "false");
	} else if (cmdmatch(buf, "reconnect")) {
		goto reconnect;
	} else if (cmdmatch(buf, "loglevel")) {
		sscanf(buf, "loglevel=%d", &ckp->loglevel);
	} else if (cmdmatch(buf, "ping")) {
		LOGDEBUG("Generator received ping request");
		send_unix_msg(sockd, "pong");
	}
	goto retry;

out:
	kill_server(si);
	dealloc(buf);
	return ret;
}

static bool send_json_msg(connsock_t *cs, json_t *json_msg)
{
	int len, sent;
	char *s;

	s = json_dumps(json_msg, JSON_ESCAPE_SLASH | JSON_EOL);
	LOGDEBUG("Sending json msg: %s", s);
	len = strlen(s);
	sent = write_socket(cs->fd, s, len);
	dealloc(s);
	if (sent != len) {
		LOGNOTICE("Failed to send %d bytes sent %d in send_json_msg", len, sent);
		return false;
	}
	return true;
}

static bool connect_proxy(connsock_t *cs)
{
	cs->fd = connect_socket(cs->url, cs->port);
	if (cs->fd < 0) {
		LOGINFO("Failed to connect socket to %s:%s in connect_proxy",
			cs->url, cs->port);
		return false;
	}
	keep_sockalive(cs->fd);
	return true;
}

/* Decode a string that should have a json message and return just the contents
 * of the result key or NULL. */
static json_t *json_result(json_t *val)
{
	json_t *res_val = NULL, *err_val;

	res_val = json_object_get(val, "result");
	/* (null) is a valid result while no value is an error, so mask out
	 * (null) and only handle lack of result */
	if (json_is_null(res_val))
		res_val = NULL;
	else if (!res_val) {
		char *ss;

		err_val = json_object_get(val, "error");
		if (err_val)
			ss = json_dumps(err_val, 0);
		else
			ss = strdup("(unknown reason)");

		LOGWARNING("JSON-RPC decode failed: %s", ss);
		free(ss);
	}
	return res_val;
}

/* Return the error value if one exists */
static json_t *json_errval(json_t *val)
{
	json_t *err_val = json_object_get(val, "error");

	return err_val;
}

/* Parse a string and return the json value it contains, if any, and the
 * result in res_val. Return NULL if no result key is found. */
static json_t *json_msg_result(char *msg, json_t **res_val, json_t **err_val)
{
	json_error_t err;
	json_t *val;

	*res_val = NULL;
	val = json_loads(msg, 0, &err);
	if (!val) {
		LOGWARNING("Json decode failed(%d): %s", err.line, err.text);
		goto out;
	}
	*res_val = json_result(val);
	*err_val = json_errval(val);

out:
	return val;
}

/* For some reason notify is buried at various different array depths so use
 * a reentrant function to try and find it. */
static json_t *find_notify(json_t *val)
{
	int arr_size, i;
	json_t *ret = NULL;
	const char *entry;

	if (!json_is_array(val))
		return NULL;
	arr_size = json_array_size(val);
	entry = json_string_value(json_array_get(val, 0));
	if (cmdmatch(entry, "mining.notify"))
		return val;
	for (i = 0; i < arr_size; i++) {
		json_t *arr_val;

		arr_val = json_array_get(val, i);
		ret = find_notify(arr_val);
		if (ret)
			break;
	}
	return ret;
}

/* Get stored line in the proxy linked list of messages if any exist or NULL */
static char *cached_proxy_line(proxy_instance_t *proxi)
{
	char *buf = NULL;

	if (proxi->recvd_lines) {
		char_entry_t *char_t = proxi->recvd_lines;

		DL_DELETE(proxi->recvd_lines, char_t);
		buf = char_t->buf;
		free(char_t);
	}
	return buf;
}

/* Get next line in the proxy linked list of messages or a new line from the
 * connsock if there are none. */
static char *next_proxy_line(connsock_t *cs, proxy_instance_t *proxi)
{
	char *buf = cached_proxy_line(proxi);

	if (!buf && read_socket_line(cs, 5) > 0)
		buf = strdup(cs->buf);
	return buf;
}

/* For appending a line to the proxy recv list */
static void append_proxy_line(proxy_instance_t *proxi, const char *buf)
{
	char_entry_t *char_t = ckalloc(sizeof(char_entry_t));
	char_t->buf = strdup(buf);
	DL_APPEND(proxi->recvd_lines, char_t);
}

/* Get a new line from the connsock and return a copy of it */
static char *new_proxy_line(connsock_t *cs)
{
	char *buf = NULL;

	if (read_socket_line(cs, 5) < 1)
		goto out;
	buf = strdup(cs->buf);
out:
	return buf;
}

static inline bool parent_proxy(proxy_instance_t *proxy)
{
	return (proxy->proxy == proxy);
}

static bool parse_subscribe(connsock_t *cs, proxy_instance_t *proxi)
{
	json_t *val = NULL, *res_val, *notify_val, *tmp;
	bool parsed, ret = false;
	int retries = 0, size;
	const char *string;
	char *buf, *old;

retry:
	parsed = true;
	if (!(buf = new_proxy_line(cs))) {
		LOGNOTICE("Proxy %d:%s failed to receive line in parse_subscribe",
			   proxi->id, proxi->si->url);
		goto out;
	}
	LOGDEBUG("parse_subscribe received %s", buf);
	/* Ignore err_val here stored in &tmp */
	val = json_msg_result(buf, &res_val, &tmp);
	if (!val || !res_val) {
		LOGINFO("Failed to get a json result in parse_subscribe, got: %s", buf);
		parsed = false;
	}
	if (!json_is_array(res_val)) {
		LOGINFO("Result in parse_subscribe not an array");
		parsed = false;
	}
	size = json_array_size(res_val);
	if (size < 3) {
		LOGINFO("Result in parse_subscribe array too small");
		parsed = false;
	}
	notify_val = find_notify(res_val);
	if (!notify_val) {
		LOGINFO("Failed to find notify in parse_subscribe");
		parsed = false;
	}
	if (!parsed) {
		if (++retries < 3) {
			/* We don't want this response so put it on the proxy
			 * recvd list to be parsed later */
			append_proxy_line(proxi, buf);
			buf = NULL;
			goto retry;
		}
		LOGNOTICE("Proxy %d:%s failed to parse subscribe response in parse_subscribe",
			  proxi->id, proxi->si->url);
		goto out;
	}

	/* Free up old data in place if we are re-subscribing */
	old = proxi->sessionid;
	proxi->sessionid = NULL;
	if (!proxi->no_params && !proxi->no_sessionid && json_array_size(notify_val) > 1) {
		/* Copy the session id if one exists. */
		string = json_string_value(json_array_get(notify_val, 1));
		if (string)
			proxi->sessionid = strdup(string);
	}
	free(old);
	tmp = json_array_get(res_val, 1);
	if (!tmp || !json_is_string(tmp)) {
		LOGWARNING("Failed to parse enonce1 in parse_subscribe");
		goto out;
	}
	string = json_string_value(tmp);
	old = proxi->enonce1;
	proxi->enonce1 = strdup(string);
	free(old);
	proxi->nonce1len = strlen(proxi->enonce1) / 2;
	if (proxi->nonce1len > 15) {
		LOGWARNING("Nonce1 too long at %d", proxi->nonce1len);
		goto out;
	}
	old = proxi->enonce1bin;
	proxi->enonce1bin = ckalloc(proxi->nonce1len);
	free(old);
	hex2bin(proxi->enonce1bin, proxi->enonce1, proxi->nonce1len);
	tmp = json_array_get(res_val, 2);
	if (!tmp || !json_is_integer(tmp)) {
		LOGWARNING("Failed to parse nonce2len in parse_subscribe");
		goto out;
	}
	size = json_integer_value(tmp);
	if (size < 1 || size > 8) {
		LOGWARNING("Invalid nonce2len %d in parse_subscribe", size);
		goto out;
	}
	if (size < 3) {
		LOGWARNING("Proxy %d:%s Nonce2 length %d too small to be able to proxy",
			   proxi->id, proxi->si->url, size);
		goto out;
	}
	proxi->nonce2len = size;
	if (parent_proxy(proxi)) {
		/* Set the number of clients per proxy on the parent proxy */
		proxi->clients_per_proxy = 1ll << ((size - 3) * 8);
		proxi->client_headroom = proxi->clients_per_proxy;
		LOGNOTICE("Proxy %d:%s clients per proxy: %"PRId64, proxi->id, proxi->si->url,
			proxi->clients_per_proxy);
	}

	LOGINFO("Found notify with enonce %s nonce2len %d", proxi->enonce1,
		proxi->nonce2len);
	ret = true;

out:
	if (val)
		json_decref(val);
	free(buf);
	return ret;
}

static bool subscribe_stratum(connsock_t *cs, proxy_instance_t *proxi)
{
	bool ret = false;
	json_t *req;

retry:
	/* Attempt to reconnect if the pool supports resuming */
	if (proxi->sessionid) {
		JSON_CPACK(req, "{s:i,s:s,s:[s,s]}",
				"id", 0,
				"method", "mining.subscribe",
				"params", PACKAGE"/"VERSION, proxi->sessionid);
	/* Then attempt to connect with just the client description */
	} else if (!proxi->no_params) {
		JSON_CPACK(req, "{s:i,s:s,s:[s]}",
				"id", 0,
				"method", "mining.subscribe",
				"params", PACKAGE"/"VERSION);
	/* Then try without any parameters */
	} else {
		JSON_CPACK(req, "{s:i,s:s,s:[]}",
				"id", 0,
				"method", "mining.subscribe",
				"params");
	}
	ret = send_json_msg(cs, req);
	json_decref(req);
	if (!ret) {
		LOGNOTICE("Proxy %d:%s failed to send message in subscribe_stratum",
			   proxi->id, proxi->si->url);
		goto out;
	}
	ret = parse_subscribe(cs, proxi);
	if (ret)
		goto out;

	if (proxi->no_params) {
		LOGNOTICE("Proxy %d:%s failed all subscription options in subscribe_stratum",
			   proxi->id, proxi->si->url);
		goto out;
	}
	if (proxi->sessionid) {
		LOGINFO("Proxy %d:%s failed sessionid reconnect in subscribe_stratum, retrying without",
			proxi->id, proxi->si->url);
		proxi->no_sessionid = true;
		dealloc(proxi->sessionid);
	} else {
		LOGINFO("Proxy %d:%s failed connecting with parameters in subscribe_stratum, retrying without",
			proxi->id, proxi->si->url);
		proxi->no_params = true;
	}
	ret = connect_proxy(cs);
	if (!ret) {
		LOGNOTICE("Proxy %d:%s failed to reconnect in subscribe_stratum",
			   proxi->id, proxi->si->url);
		goto out;
	}
	goto retry;

out:
	if (!ret)
		Close(cs->fd);
	return ret;
}

static bool passthrough_stratum(connsock_t *cs, proxy_instance_t *proxi)
{
	json_t *req, *val = NULL, *res_val, *err_val;
	bool ret = false;

	JSON_CPACK(req, "{s:s,s:[s]}",
			"method", "mining.passthrough",
			"params", PACKAGE"/"VERSION);
	ret = send_json_msg(cs, req);
	json_decref(req);
	if (!ret) {
		LOGWARNING("Failed to send message in passthrough_stratum");
		goto out;
	}
	if (read_socket_line(cs, 5) < 1) {
		LOGWARNING("Failed to receive line in passthrough_stratum");
		goto out;
	}
	/* Ignore err_val here since we should always get a result from an
	 * upstream passthrough server */
	val = json_msg_result(cs->buf, &res_val, &err_val);
	if (!val || !res_val) {
		LOGWARNING("Failed to get a json result in passthrough_stratum, got: %s",
			   cs->buf);
		goto out;
	}
	ret = json_is_true(res_val);
	if (!ret) {
		LOGWARNING("Denied passthrough for stratum");
		goto out;
	}
	proxi->passthrough = true;
out:
	if (val)
		json_decref(val);
	if (!ret)
		Close(cs->fd);
	return ret;
}

static bool parse_notify(proxy_instance_t *proxi, json_t *val)
{
	const char *prev_hash, *bbversion, *nbit, *ntime;
	proxy_instance_t *proxy = proxi->proxy;
	char *job_id, *coinbase1, *coinbase2;
	gdata_t *gdata = proxi->ckp->data;
	bool clean, ret = false;
	notify_instance_t *ni;
	int merkles, i;
	json_t *arr;

	arr = json_array_get(val, 4);
	if (!arr || !json_is_array(arr))
		goto out;

	merkles = json_array_size(arr);
	job_id = json_array_string(val, 0);
	prev_hash = __json_array_string(val, 1);
	coinbase1 = json_array_string(val, 2);
	coinbase2 = json_array_string(val, 3);
	bbversion = __json_array_string(val, 5);
	nbit = __json_array_string(val, 6);
	ntime = __json_array_string(val, 7);
	clean = json_is_true(json_array_get(val, 8));
	if (!job_id || !prev_hash || !coinbase1 || !coinbase2 || !bbversion || !nbit || !ntime) {
		if (job_id)
			free(job_id);
		if (coinbase1)
			free(coinbase1);
		if (coinbase2)
			free(coinbase2);
		goto out;
	}

	LOGDEBUG("New notify");
	ni = ckzalloc(sizeof(notify_instance_t));
	ni->jobid = job_id;
	LOGDEBUG("Job ID %s", job_id);
	ni->coinbase1 = coinbase1;
	LOGDEBUG("Coinbase1 %s", coinbase1);
	ni->coinb1len = strlen(coinbase1) / 2;
	ni->coinbase2 = coinbase2;
	LOGDEBUG("Coinbase2 %s", coinbase2);
	memcpy(ni->prevhash, prev_hash, 65);
	LOGDEBUG("Prevhash %s", prev_hash);
	memcpy(ni->bbversion, bbversion, 9);
	LOGDEBUG("BBVersion %s", bbversion);
	memcpy(ni->nbit, nbit, 9);
	LOGDEBUG("Nbit %s", nbit);
	memcpy(ni->ntime, ntime, 9);
	LOGDEBUG("Ntime %s", ntime);
	ni->clean = clean;
	LOGDEBUG("Clean %s", clean ? "true" : "false");
	LOGDEBUG("Merkles %d", merkles);
	for (i = 0; i < merkles; i++) {
		const char *merkle = __json_array_string(arr, i);

		LOGDEBUG("Merkle %d %s", i, merkle);
		memcpy(&ni->merklehash[i][0], merkle, 65);
	}
	ni->merkles = merkles;
	ret = true;
	ni->notify_time = time(NULL);

	/* Add the notify instance to the parent proxy list, not the subproxy */
	mutex_lock(&proxy->notify_lock);
	ni->id = gdata->proxy_notify_id++;
	HASH_ADD_INT(proxy->notify_instances, id, ni);
	/* Now set the subproxy's current notify to this */
	proxi->current_notify = ni;
	mutex_unlock(&proxy->notify_lock);

out:
	return ret;
}

static bool parse_diff(proxy_instance_t *proxi, json_t *val)
{
	double diff = json_number_value(json_array_get(val, 0));

	if (diff == 0 || diff == proxi->diff)
		return true;
	proxi->diff = diff;
	return true;
}

static bool send_version(proxy_instance_t *proxi, json_t *val)
{
	json_t *json_msg, *id_val = json_object_dup(val, "id");
	connsock_t *cs = proxi->cs;
	bool ret;

	JSON_CPACK(json_msg, "{sossso}", "id", id_val, "result", PACKAGE"/"VERSION,
			     "error", json_null());
	ret = send_json_msg(cs, json_msg);
	json_decref(json_msg);
	return ret;
}

static bool show_message(json_t *val)
{
	const char *msg;

	if (!json_is_array(val))
		return false;
	msg = json_string_value(json_array_get(val, 0));
	if (!msg)
		return false;
	LOGNOTICE("Pool message: %s", msg);
	return true;
}

static bool send_pong(proxy_instance_t *proxi, json_t *val)
{
	json_t *json_msg, *id_val = json_object_dup(val, "id");
	connsock_t *cs = proxi->cs;
	bool ret;

	JSON_CPACK(json_msg, "{sossso}", "id", id_val, "result", "pong",
			     "error", json_null());
	ret = send_json_msg(cs, json_msg);
	json_decref(json_msg);
	return ret;
}

static void prepare_proxy(proxy_instance_t *proxi);
static proxy_instance_t *create_subproxy(gdata_t *gdata, proxy_instance_t *proxi);
static bool recruit_subproxy(gdata_t *gdata, proxy_instance_t *proxi);

static void add_subproxy(proxy_instance_t *proxi, proxy_instance_t *subproxy)
{
	mutex_lock(&proxi->proxy_lock);
	proxi->subproxy_count++;
	HASH_ADD_INT(proxi->subproxies, id, subproxy);
	proxi->client_headroom += proxi->clients_per_proxy;
	mutex_unlock(&proxi->proxy_lock);
}

static proxy_instance_t *__subproxy_by_id(proxy_instance_t *proxy, const int id)
{
	proxy_instance_t *subproxy;

	HASH_FIND_INT(proxy->subproxies, &id, subproxy);
	return subproxy;
}

/* Add to the dead list to be recycled if possible */
static void store_proxy(gdata_t *gdata, proxy_instance_t *proxy)
{
	mutex_lock(&gdata->lock);
	DL_APPEND(gdata->dead_proxies, proxy);
	mutex_unlock(&gdata->lock);
}

/* Remove the subproxy from the proxi list and put it on the dead list */
static void disable_subproxy(gdata_t *gdata, proxy_instance_t *proxi, proxy_instance_t *subproxy)
{
	mutex_lock(&proxi->proxy_lock);
	subproxy->disabled = true;
	/* Make sure subproxy is still in the list */
	subproxy = __subproxy_by_id(proxi, subproxy->id);
	if (subproxy) {
		HASH_DEL(proxi->subproxies, subproxy);
		proxi->client_headroom -= proxi->clients_per_proxy;
	}
	mutex_unlock(&proxi->proxy_lock);

	if (subproxy)
		store_proxy(gdata, subproxy);

	if (proxi->client_headroom < 42 && proxi->alive)
		recruit_subproxy(gdata, proxi);
}

static bool parse_reconnect(proxy_instance_t *proxi, json_t *val)
{
	server_instance_t *newsi, *si = proxi->si;
	proxy_instance_t *newproxi;
	ckpool_t *ckp = proxi->ckp;
	gdata_t *gdata = ckp->data;
	const char *new_url;
	bool ret = false;
	int new_port;
	char *url;

	new_url = json_string_value(json_array_get(val, 0));
	new_port = json_integer_value(json_array_get(val, 1));
	/* See if we have an invalid entry listing port as a string instead of
	 * integer and handle that. */
	if (!new_port) {
		const char *newport_string = json_string_value(json_array_get(val, 1));

		if (newport_string)
			sscanf(newport_string, "%d", &new_port);
	}
	if (new_url && strlen(new_url) && new_port) {
		char *dot_pool, *dot_reconnect;
		int len;

		dot_pool = strchr(si->url, '.');
		if (!dot_pool) {
			LOGWARNING("Denied stratum reconnect request from server without domain %s",
				   si->url);
			goto out;
		}
		dot_reconnect = strchr(new_url, '.');
		if (!dot_reconnect) {
			LOGWARNING("Denied stratum reconnect request to url without domain %s",
				   new_url);
			goto out;
		}
		len = strlen(dot_reconnect);
		if (strncmp(dot_pool, dot_reconnect, len)) {
			LOGWARNING("Denied stratum reconnect request from %s to non-matching domain %s",
				   si->url, new_url);
			goto out;
		}
		ASPRINTF(&url, "%s:%d", new_url, new_port);
	} else
		url = strdup(si->url);
	LOGINFO("Processing reconnect request to %s", url);

	ret = true;
	/* If this isn't a parent proxy, add a new subproxy to the parent */
	if (proxi != proxi->proxy) {
		newproxi = create_subproxy(gdata, proxi);
		add_subproxy(proxi, newproxi);
		goto out;
	}

	newsi = ckzalloc(sizeof(server_instance_t));

	mutex_lock(&gdata->lock);
	HASH_DEL(gdata->proxies, proxi);
	newsi->id = si->id; /* Inherit the old connection's id */
	si->id = ckp->proxies++; /* Give the old connection the lowest id */
	ckp->servers = realloc(ckp->servers, sizeof(server_instance_t *) * ckp->proxies);
	ckp->servers[newsi->id] = newsi;
	newsi->url = url;
	newsi->auth = strdup(si->auth);
	newsi->pass = strdup(si->pass);
	proxi->reconnect = true;

	newproxi = ckzalloc(sizeof(proxy_instance_t));
	newsi->data = newproxi;
	newproxi->auth = newsi->auth;
	newproxi->pass = newsi->pass;
	newproxi->si = newsi;
	newproxi->ckp = ckp;
	newproxi->cs = &newsi->cs;
	newproxi->cs->ckp = ckp;
	newproxi->id = newsi->id;
	HASH_ADD_INT(gdata->proxies, id, proxi);
	HASH_ADD_INT(gdata->proxies, id, newproxi);
	mutex_unlock(&gdata->lock);

	prepare_proxy(newproxi);
out:
	return ret;
}

static void send_diff(ckpool_t *ckp, proxy_instance_t *proxi)
{
	proxy_instance_t *proxy = proxi->proxy;
	json_t *json_msg;
	char *msg, *buf;

	/* Not set yet */
	if (!proxi->diff)
		return;

	JSON_CPACK(json_msg, "{sisisf}",
		   "proxy", proxy->id,
		   "subproxy", proxi->id,
		   "diff", proxi->diff);
	msg = json_dumps(json_msg, JSON_NO_UTF8);
	json_decref(json_msg);
	ASPRINTF(&buf, "diff=%s", msg);
	free(msg);
	send_proc(ckp->stratifier, buf);
	free(buf);
}

static void send_notify(ckpool_t *ckp, proxy_instance_t *proxi)
{
	proxy_instance_t *proxy = proxi->proxy;
	json_t *json_msg, *merkle_arr;
	notify_instance_t *ni;
	char *msg, *buf;
	int i;

	merkle_arr = json_array();

	mutex_lock(&proxy->notify_lock);
	ni = proxi->current_notify;
	if (unlikely(!ni)) {
		mutex_unlock(&proxy->notify_lock);
		LOGNOTICE("Proxi %d not ready to send notify", proxi->id);
		return;
	}
	for (i = 0; i < ni->merkles; i++)
		json_array_append_new(merkle_arr, json_string(&ni->merklehash[i][0]));
	/* Use our own jobid instead of the server's one for easy lookup */
	JSON_CPACK(json_msg, "{sisisisssisssssosssssssb}",
			     "proxy", proxy->id, "subproxy", proxi->id,
			     "jobid", ni->id, "prevhash", ni->prevhash, "coinb1len", ni->coinb1len,
			     "coinbase1", ni->coinbase1, "coinbase2", ni->coinbase2,
			     "merklehash", merkle_arr, "bbversion", ni->bbversion,
			     "nbit", ni->nbit, "ntime", ni->ntime,
			     "clean", ni->clean);
	mutex_unlock(&proxy->notify_lock);

	msg = json_dumps(json_msg, JSON_NO_UTF8);
	json_decref(json_msg);
	ASPRINTF(&buf, "notify=%s", msg);
	free(msg);
	send_proc(ckp->stratifier, buf);
	free(buf);

	/* Send diff now as stratifier will not accept diff till it has a
	 * valid workbase */
	send_diff(ckp, proxi);
}

static bool parse_method(ckpool_t *ckp, proxy_instance_t *proxi, const char *msg)
{
	json_t *val = NULL, *method, *err_val, *params;
	json_error_t err;
	bool ret = false;
	const char *buf;

	memset(&err, 0, sizeof(err));
	val = json_loads(msg, 0, &err);
	if (!val) {
		LOGWARNING("JSON decode failed(%d): %s", err.line, err.text);
		goto out;
	}

	method = json_object_get(val, "method");
	if (!method) {
		LOGDEBUG("Failed to find method in json for parse_method");
		goto out;
	}
	err_val = json_object_get(val, "error");
	params = json_object_get(val, "params");

	if (err_val && !json_is_null(err_val)) {
		char *ss;

		if (err_val)
			ss = json_dumps(err_val, 0);
		else
			ss = strdup("(unknown reason)");

		LOGINFO("JSON-RPC method decode failed: %s", ss);
		free(ss);
		goto out;
	}

	if (!json_is_string(method)) {
		LOGINFO("Method is not string in parse_method");
		goto out;
	}
	buf = json_string_value(method);
	if (!buf || strlen(buf) < 1) {
		LOGINFO("Invalid string for method in parse_method");
		goto out;
	}

	if (cmdmatch(buf, "mining.notify")) {
		ret = parse_notify(proxi, params);
		if (ret)
			send_notify(ckp, proxi);
		goto out;
	}

	if (cmdmatch(buf, "mining.set_difficulty")) {
		ret = parse_diff(proxi, params);
		if (likely(ret))
			send_diff(ckp, proxi);
		goto out;
	}

	if (cmdmatch(buf, "client.reconnect")) {
		ret = parse_reconnect(proxi, params);
		goto out;
	}

	if (cmdmatch(buf, "client.get_version")) {
		ret =  send_version(proxi, val);
		goto out;
	}

	if (cmdmatch(buf, "client.show_message")) {
		ret = show_message(params);
		goto out;
	}

	if (cmdmatch(buf, "mining.ping")) {
		ret = send_pong(proxi, val);
		goto out;
	}
out:
	if (val)
		json_decref(val);
	return ret;
}

static bool auth_stratum(ckpool_t *ckp, connsock_t *cs, proxy_instance_t *proxi)
{
	json_t *val = NULL, *res_val, *req, *err_val;
	char *buf = NULL;
	bool ret;

	JSON_CPACK(req, "{s:i,s:s,s:[s,s]}",
			"id", 42,
			"method", "mining.authorize",
			"params", proxi->auth, proxi->pass);
	ret = send_json_msg(cs, req);
	json_decref(req);
	if (!ret) {
		LOGNOTICE("Proxy %d:%s failed to send message in auth_stratum",
			  proxi->id, proxi->si->url);
		Close(cs->fd);
		goto out;
	}

	/* Read and parse any extra methods sent. Anything left in the buffer
	 * should be the response to our auth request. */
	do {
		free(buf);
		buf = next_proxy_line(cs, proxi);
		if (!buf) {
			LOGNOTICE("Proxy %d:%s failed to receive line in auth_stratum",
				  proxi->id, proxi->si->url);
			ret = false;
			goto out;
		}
		ret = parse_method(ckp, proxi, buf);
	} while (ret);

	val = json_msg_result(buf, &res_val, &err_val);
	if (!val) {
		LOGWARNING("Proxy %d:%s failed to get a json result in auth_stratum, got: %s",
			   proxi->id, proxi->si->url, buf);
		goto out;
	}

	if (err_val && !json_is_null(err_val)) {
		LOGWARNING("Proxy %d:%s failed to authorise in auth_stratum due to err_val, got: %s",
			   proxi->id, proxi->si->url, buf);
		goto out;
	}
	if (res_val) {
		ret = json_is_true(res_val);
		if (!ret) {
			LOGWARNING("Proxy %d:%s failed to authorise in auth_stratum",
				   proxi->id, proxi->si->url);
			goto out;
		}
	} else {
		/* No result and no error but successful val means auth success */
		ret = true;
	}
	LOGINFO("Proxy %d:%s auth success in auth_stratum", proxi->id, proxi->si->url);
out:
	if (val)
		json_decref(val);
	if (ret) {
		/* Now parse any cached responses so there are none in the
		 * queue and they can be managed one at a time from now on. */
		while(42) {
			dealloc(buf);
			buf = cached_proxy_line(proxi);
			if (!buf)
				break;
			parse_method(ckp, proxi, buf);
		};
	}
	return ret;
}

static proxy_instance_t *proxy_by_id(gdata_t *gdata, const int id)
{
	proxy_instance_t *proxi;

	mutex_lock(&gdata->lock);
	HASH_FIND_INT(gdata->proxies, &id, proxi);
	mutex_unlock(&gdata->lock);

	return proxi;
}

static void send_subscribe(ckpool_t *ckp, proxy_instance_t *proxi)
{
	json_t *json_msg;
	char *msg, *buf;

	JSON_CPACK(json_msg, "{sisisssi}",
			     "proxy", proxi->proxy->id,
			     "subproxy", proxi->id,
			     "enonce1", proxi->enonce1,
			     "nonce2len", proxi->nonce2len);
	msg = json_dumps(json_msg, JSON_NO_UTF8);
	json_decref(json_msg);
	ASPRINTF(&buf, "subscribe=%s", msg);
	free(msg);
	send_proc(ckp->stratifier, buf);
	free(buf);
}

static proxy_instance_t *subproxy_by_id(proxy_instance_t *proxy, const int id)
{
	proxy_instance_t *subproxy;

	mutex_lock(&proxy->proxy_lock);
	subproxy = __subproxy_by_id(proxy, id);
	if (subproxy && subproxy->disabled)
		subproxy = NULL;
	mutex_unlock(&proxy->proxy_lock);

	return subproxy;
}

static void stratifier_drop_client(ckpool_t *ckp, int64_t id)
{
	char buf[256];

	sprintf(buf, "dropclient=%"PRId64, id);
	send_proc(ckp->stratifier, buf);
}

static void submit_share(gdata_t *gdata, json_t *val)
{
	proxy_instance_t *proxy, *proxi;
	ckpool_t *ckp = gdata->ckp;
	stratum_msg_t *msg;
	share_msg_t *share;
	int64_t client_id;
	int id, subid;

	/* Get the client id so we can tell the stratifier to drop it if the
	 * proxy it's bound to is not functional */
	json_getdel_int64(&client_id, val, "client_id");
	json_getdel_int(&id, val, "proxy");
	proxy = proxy_by_id(gdata, id);
	if (unlikely(!proxy)) {
		LOGWARNING("Failed to find proxy %d to send share to", id);
		stratifier_drop_client(ckp, client_id);
		return json_decref(val);
	}
	json_get_int(&subid, val, "subproxy");
	proxi = subproxy_by_id(proxy, subid);
	if (unlikely(!proxi)) {
		LOGNOTICE("Failed to find proxy %d:%d to send share to", id, subid);
		stratifier_drop_client(ckp, client_id);
		return json_decref(val);
	}
	if (!proxi->alive) {
		LOGNOTICE("Client %"PRId64" attempting to send shares to dead proxy %d, dropping",
			  client_id, id);
		stratifier_drop_client(ckp, client_id);
		return json_decref(val);
	}

	msg = ckzalloc(sizeof(stratum_msg_t));
	share = ckzalloc(sizeof(share_msg_t));
	share->submit_time = time(NULL);
	share->client_id = client_id;
	json_getdel_int(&share->msg_id, val, "msg_id");
	msg->json_msg = val;

	/* Add new share entry to the share hashtable */
	mutex_lock(&proxi->share_lock);
	share->id = proxi->share_id++;
	HASH_ADD_I64(proxi->shares, id, share);
	mutex_unlock(&proxi->share_lock);

	json_object_set_nocheck(val, "id", json_integer(share->id));

	/* Add the new message to the psend list */
	mutex_lock(&proxy->psend_lock);
	DL_APPEND(proxy->psends, msg);
	pthread_cond_signal(&proxy->psend_cond);
	mutex_unlock(&proxy->psend_lock);
}

static void clear_notify(notify_instance_t *ni)
{
	free(ni->jobid);
	free(ni->coinbase1);
	free(ni->coinbase2);
	free(ni);
}

/* FIXME: Return something useful to the stratifier based on this result */
static bool parse_share(proxy_instance_t *proxi, const char *buf)
{
	json_t *val = NULL, *idval;
	share_msg_t *share;
	bool ret = false;
	int64_t id;

	val = json_loads(buf, 0, NULL);
	if (!val) {
		LOGINFO("Failed to parse json msg: %s", buf);
		goto out;
	}
	idval = json_object_get(val, "id");
	if (!idval) {
		LOGINFO("Failed to find id in json msg: %s", buf);
		goto out;
	}
	id = json_integer_value(idval);

	mutex_lock(&proxi->share_lock);
	HASH_FIND_I64(proxi->shares, &id, share);
	if (share)
		HASH_DEL(proxi->shares, share);
	mutex_unlock(&proxi->share_lock);

	if (!share) {
		LOGINFO("Failed to find matching share to result: %s", buf);
		goto out;
	}
	ret = true;
	LOGDEBUG("Found share from client %d with msg_id %d", share->client_id,
		 share->msg_id);
	free(share);
out:
	if (val)
		json_decref(val);
	return ret;
}

/* For processing and sending shares. proxy refers to parent proxy here */
static void *proxy_send(void *arg)
{
	proxy_instance_t *proxy = (proxy_instance_t *)arg;
	connsock_t *cs = proxy->cs;
	gdata_t *gdata = cs->ckp->data;

	rename_proc("proxysend");

	while (42) {
		proxy_instance_t *subproxy;
		notify_instance_t *ni;
		stratum_msg_t *msg;
		char *jobid = NULL;
		bool ret = true;
		int subid = 0;
		json_t *val;
		uint32_t id;

		mutex_lock(&proxy->psend_lock);
		if (!proxy->psends)
			pthread_cond_wait(&proxy->psend_cond, &proxy->psend_lock);
		msg = proxy->psends;
		if (likely(msg))
			DL_DELETE(proxy->psends, msg);
		mutex_unlock(&proxy->psend_lock);

		if (unlikely(!msg))
			continue;

		json_getdel_int(&subid, msg->json_msg, "subproxy");
		json_uintcpy(&id, msg->json_msg, "jobid");

		mutex_lock(&proxy->notify_lock);
		HASH_FIND_INT(proxy->notify_instances, &id, ni);
		if (ni)
			jobid = strdup(ni->jobid);
		mutex_unlock(&proxy->notify_lock);

		subproxy = subproxy_by_id(proxy, subid);

		if (jobid && subproxy) {
			cs = subproxy->cs;
			JSON_CPACK(val, "{s[ssooo]soss}", "params", proxy->auth, jobid,
					json_object_dup(msg->json_msg, "nonce2"),
					json_object_dup(msg->json_msg, "ntime"),
					json_object_dup(msg->json_msg, "nonce"),
					"id", json_object_dup(msg->json_msg, "id"),
					"method", "mining.submit");
			ret = send_json_msg(cs, val);
			json_decref(val);
		} else {
			LOGNOTICE("Proxy %d:%s failed to find matching jobid in proxysend",
				  proxy->id, proxy->si->url);
		}
		free(jobid);
		json_decref(msg->json_msg);
		free(msg);
		if (!ret && subproxy && cs->fd > 0) {
			LOGWARNING("Proxy %d:%s failed to send msg in proxy_send, dropping to reconnect",
				   proxy->id, proxy->si->url);
			Close(cs->fd);
			if (!parent_proxy && !subproxy->disabled)
				disable_subproxy(gdata, proxy, subproxy);
		}
	}
	return NULL;
}

static void passthrough_send(ckpool_t __maybe_unused *ckp, pass_msg_t *pm)
{
	int len, sent;

	LOGDEBUG("Sending upstream json msg: %s", pm->msg);
	len = strlen(pm->msg);
	sent = write_socket(pm->cs->fd, pm->msg, len);
	if (sent != len) {
		/* FIXME: Do something about this? */
		LOGWARNING("Failed to passthrough %d bytes of message %s", len, pm->msg);
	}
	free(pm->msg);
	free(pm);
}

static void passthrough_add_send(proxy_instance_t *proxi, const char *msg)
{
	pass_msg_t *pm = ckzalloc(sizeof(pass_msg_t));

	pm->cs = proxi->cs;
	ASPRINTF(&pm->msg, "%s\n", msg);
	ckmsgq_add(proxi->passsends, pm);
}

static bool proxy_alive(ckpool_t *ckp, server_instance_t *si, proxy_instance_t *proxi,
			connsock_t *cs, bool pinging, int epfd)
{
	gdata_t *gdata = ckp->data;
	struct epoll_event event;
	bool ret = false;

	/* Has this proxy already been reconnected? */
	if (cs->fd > 0)
		return true;
	if (!extract_sockaddr(si->url, &cs->url, &cs->port)) {
		LOGWARNING("Failed to extract address from %s", si->url);
		return ret;
	}
	if (!connect_proxy(cs)) {
		if (!pinging) {
			LOGINFO("Failed to connect to %s:%s in proxy_mode!",
				cs->url, cs->port);
		}
		return ret;
	}
	if (ckp->passthrough) {
		if (!passthrough_stratum(cs, proxi)) {
			LOGWARNING("Failed initial passthrough to %s:%s !",
				   cs->url, cs->port);
			goto out;
		}
		ret = true;
		goto out;
	}
	/* Test we can connect, authorise and get stratum information */
	if (!subscribe_stratum(cs, proxi)) {
		if (!pinging) {
			LOGWARNING("Failed initial subscribe to %s:%s !",
				   cs->url, cs->port);
		}
		goto out;
	}
	if (!ckp->passthrough)
		send_subscribe(ckp, proxi);
	if (!auth_stratum(ckp, cs, proxi)) {
		if (!pinging) {
			LOGWARNING("Failed initial authorise to %s:%s with %s:%s !",
				   cs->url, cs->port, si->auth, si->pass);
		}
		goto out;
	}
	ret = true;
out:
	if (!ret) {
		/* Close and invalidate the file handle */
		Close(cs->fd);
	} else {
		keep_sockalive(cs->fd);
		event.events = EPOLLIN;
		event.data.ptr = proxi;
		/* Add this connsock_t to the epoll list */
		if (unlikely(epoll_ctl(epfd, EPOLL_CTL_ADD, cs->fd, &event) == -1)) {
			LOGERR("Failed to add fd %d to epfd %d to epoll_ctl in proxy_alive",
			       cs->fd, epfd);
			return false;
		}
		if (!ckp->passthrough && parent_proxy(proxi)) {
			/* We recruit enough proxies to begin with and then
			 * recruit extra when asked by the stratifier. */
			while (proxi->client_headroom < 42) {
				/* Note recursive call of proxy_alive here */
				if (!recruit_subproxy(gdata, proxi)) {
					LOGWARNING("Unable to recruit extra subproxies after just %"PRId64,
						   proxi->client_headroom);
					break;
				}
				LOGWARNING("Proxy %d:%s recruited extra subproxy!",
					   proxi->id, cs->url);
			}
		}
	}
	proxi->alive = ret;
	return ret;
}

/* Creates a duplicate instance or proxi to be used as a subproxy, ignoring
 * fields we don't use in the subproxy. */
static proxy_instance_t *create_subproxy(gdata_t *gdata, proxy_instance_t *proxi)
{
	proxy_instance_t *subproxy;

	mutex_lock(&gdata->lock);
	if (gdata->dead_proxies) {
		/* Recycle an old proxy instance if one exists */
		subproxy = gdata->dead_proxies;
		DL_DELETE(gdata->dead_proxies, subproxy);
		subproxy->disabled = false;
	} else {
		subproxy = ckzalloc(sizeof(proxy_instance_t));
		subproxy->cs = ckzalloc(sizeof(connsock_t));
		mutex_init(&subproxy->share_lock);
	}
	mutex_unlock(&gdata->lock);

	subproxy->cs->ckp = subproxy->ckp = proxi->ckp;
	subproxy->si = proxi->si;
	subproxy->id = proxi->subproxy_count;
	subproxy->auth = proxi->auth;
	subproxy->pass = proxi->pass;
	subproxy->proxy = proxi;
	subproxy->epfd = proxi->epfd;
	return subproxy;
}

static bool recruit_subproxy(gdata_t *gdata, proxy_instance_t *proxi)
{
	proxy_instance_t *subproxy = create_subproxy(gdata, proxi);
	int epfd = proxi->epfd;

	if (!proxy_alive(subproxy->ckp, subproxy->si, subproxy, subproxy->cs, false, epfd)) {
		LOGNOTICE("Subproxy failed proxy_alive testing");
		store_proxy(gdata, subproxy);
		return false;
	}

	add_subproxy(proxi, subproxy);

	return true;
}

/* For receiving messages from an upstream pool to pass downstream. Responsible
 * for setting up the connection and testing pool is live. */
static void *passthrough_recv(void *arg)
{
	proxy_instance_t *proxi = (proxy_instance_t *)arg;
	server_instance_t *si = proxi->si;
	connsock_t *cs = proxi->cs;
	ckpool_t *ckp = proxi->ckp;
	struct epoll_event event;
	bool alive;
	int epfd;

	rename_proc("passrecv");

	proxi->epfd = epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0){
		LOGEMERG("FATAL: Failed to create epoll in passrecv");
		return NULL;
	}

	if (proxy_alive(ckp, si, proxi, cs, false, epfd)) {
		send_proc(ckp->generator, "reconnect");
		LOGWARNING("Proxy %d:%s connection established",
			   proxi->id, proxi->si->url);
	}
	alive = proxi->alive;

	while (42) {
		int ret;

		while (!proxy_alive(ckp, si, proxi, cs, true, epfd)) {
			if (alive) {
				alive = false;
				send_proc(ckp->generator, "reconnect");
			}
			sleep(5);
		}
		if (!alive)
			send_proc(ckp->generator, "reconnect");

		/* Make sure we receive a line within 90 seconds */
		ret = epoll_wait(epfd, &event, 1, 90000);
		if (likely(ret > 0))
			ret = read_socket_line(cs, 60);
		if (ret < 1) {
			LOGWARNING("Proxy %d:%s failed to read_socket_line in proxy_recv, attempting reconnect",
				   proxi->id, proxi->si->url);
			alive = proxi->alive = false;
			send_proc(ckp->generator, "reconnect");
			continue;
		}
		/* Simply forward the message on, as is, to the connector to
		 * process. Possibly parse parameters sent by upstream pool
		 * here */
		send_proc(ckp->connector, cs->buf);
	}
	return NULL;
}

static proxy_instance_t *best_proxy(ckpool_t *ckp, gdata_t *gdata);

#if 0
static proxy_instance_t *current_proxy(gdata_t *gdata)
{
	proxy_instance_t *ret;

	mutex_lock(&gdata->lock);
	ret = gdata->proxy;
	mutex_unlock(&gdata->lock);

	return ret;
}
#endif

/* For receiving messages from the upstream proxy, also responsible for setting
 * up the connection and testing it's alive. */
static void *proxy_recv(void *arg)
{
	proxy_instance_t *proxi = (proxy_instance_t *)arg;
	server_instance_t *si = proxi->si;
	connsock_t *cs = proxi->cs;
	ckpool_t *ckp = proxi->ckp;
	gdata_t *gdata = ckp->data;
	struct epoll_event event;
	bool alive;
	int epfd;

	rename_proc("proxyrecv");

	proxi->epfd = epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0){
		LOGEMERG("FATAL: Failed to create epoll in proxyrecv");
		return NULL;
	}

	if (proxy_alive(ckp, si, proxi, cs, false, epfd)) {
		send_proc(ckp->generator, "reconnect");
		LOGWARNING("Proxy %d:%s connection established",
			   proxi->id, proxi->si->url);
	}
	alive = proxi->alive;

	while (42) {
		proxy_instance_t *subproxy = proxi;
		share_msg_t *share, *tmpshare;
		notify_instance_t *ni, *tmp;
		time_t now;
		int ret;

		while (!proxy_alive(ckp, si, proxi, proxi->cs, true, epfd)) {
			if (alive) {
				alive = false;
				send_proc(ckp->generator, "reconnect");
			}
			sleep(5);
			proxi->reconnect_time = time(NULL);
		}
		/* Wait 90 seconds before declaring this upstream pool alive
		 * to prevent switching to unstable pools. */
		if (!alive && (!best_proxy(ckp, gdata) ||
		    time(NULL) - proxi->reconnect_time > 90)) {
			LOGWARNING("Proxy %d:%s recovered", proxi->id, proxi->si->url);
			proxi->reconnect_time = 0;
			send_proc(ckp->generator, "reconnect");
		}
		alive = true;

		now = time(NULL);

		/* Age old notifications older than 10 mins old */
		mutex_lock(&proxi->notify_lock);
		HASH_ITER(hh, proxi->notify_instances, ni, tmp) {
			if (HASH_COUNT(proxi->notify_instances) < 3)
				break;
			if (ni->notify_time < now - 600) {
				HASH_DEL(proxi->notify_instances, ni);
				clear_notify(ni);
			}
		}
		mutex_unlock(&proxi->notify_lock);

		/* Similary with shares older than 2 mins without response */
		mutex_lock(&proxi->share_lock);
		HASH_ITER(hh, proxi->shares, share, tmpshare) {
			if (share->submit_time < now - 120) {
				HASH_DEL(proxi->shares, share);
			}
		}
		mutex_unlock(&proxi->share_lock);

		/* If we don't get an update within 10 minutes the upstream pool
		 * has likely stopped responding. */
		ret = epoll_wait(epfd, &event, 1, 600000);
		if (likely(ret > 0)) {
			subproxy = event.data.ptr;
			cs = subproxy->cs;
			ret = read_socket_line(cs, 5);
		}
		if (ret < 1) {
			if (alive) {
				alive = false;
				LOGWARNING("Proxy %d:%s failed to epoll/read_socket_line in proxy_recv, attempting reconnect",
					   subproxy->id, subproxy->si->url);
			}
			continue;
		}
		if (parse_method(ckp, subproxy, cs->buf)) {
			if (subproxy->reconnect) {
				/* Call this proxy dead to allow us to fail
				 * over to a backup pool until the reconnect
				 * pool is up */
				subproxy->reconnect = false;
				alive = subproxy->alive = false;
				send_proc(ckp->generator, "reconnect");
				LOGWARNING("Proxy %d:%s reconnect issue, dropping existing connection",
					   subproxy->id, subproxy->si->url);
				Close(cs->fd);
				break;
			}
			continue;
		}
		if (parse_share(subproxy, cs->buf)) {
			continue;
		}
		/* If it's not a method it should be a share result */
		LOGWARNING("Unhandled stratum message: %s", cs->buf);
	}
	return NULL;
}

static void prepare_proxy(proxy_instance_t *proxi)
{
	proxi->proxy = proxi;
	mutex_init(&proxi->proxy_lock);
	add_subproxy(proxi, proxi);
	mutex_init(&proxi->psend_lock);
	cond_init(&proxi->psend_cond);
	create_pthread(&proxi->pth_psend, proxy_send, proxi);
	create_pthread(&proxi->pth_precv, proxy_recv, proxi);
}

static void setup_proxies(ckpool_t *ckp, gdata_t *gdata)
{
	int i;

	for (i = 0; i < ckp->proxies; i++) {
		proxy_instance_t *proxi;
		server_instance_t *si;

		si = ckp->servers[i];
		proxi = si->data;
		proxi->id = i;
		HASH_ADD_INT(gdata->proxies, id, proxi);
		if (ckp->passthrough) {
			create_pthread(&proxi->pth_precv, passthrough_recv, proxi);
			proxi->passsends = create_ckmsgq(ckp, "passsend", &passthrough_send);
		} else {
			prepare_proxy(proxi);
		}
	}
}

static proxy_instance_t *best_proxy(ckpool_t *ckp, gdata_t *gdata)
{
	proxy_instance_t *ret = NULL, *proxi, *tmp;

	while (42) {
		if (!ping_main(ckp))
			break;

		mutex_lock(&gdata->lock);
		HASH_ITER(hh, gdata->proxies, proxi, tmp) {
			if (proxi->alive) {
				if (!ret) {
					ret = proxi;
					continue;
				}
				if (proxi->id < ret->id)
					ret = proxi;
			}
		}
		gdata->proxy = ret;
		mutex_unlock(&gdata->lock);

		if (ret)
			break;
		sleep(1);
	}
	send_proc(ckp->connector, ret ? "accept" : "reject");
	return ret;
}

static int proxy_loop(proc_instance_t *pi)
{
	proxy_instance_t *proxi = NULL, *cproxy;
	int sockd = -1, ret = 0, selret;
	unixsock_t *us = &pi->us;
	ckpool_t *ckp = pi->ckp;
	gdata_t *gdata = ckp->data;
	char *buf = NULL;

	setup_proxies(ckp, gdata);
reconnect:
	/* This does not necessarily mean we reconnect, but a change has
	 * occurred and we need to reexamine the proxies. */
	cproxy = best_proxy(ckp, gdata);
	if (!cproxy)
		goto out;
	if (proxi != cproxy) {
		proxi = cproxy;
		if (!ckp->passthrough) {
			connsock_t *cs = proxi->cs;
			LOGWARNING("Successfully connected to proxy %d %s:%s as proxy",
				   proxi->id, cs->url, cs->port);
			dealloc(buf);
			ASPRINTF(&buf, "proxy=%d", proxi->id);
			send_proc(ckp->stratifier, buf);
		}
	}
retry:
	do {
		selret = wait_read_select(us->sockd, 5);
		if (!selret && !ping_main(ckp)) {
			LOGEMERG("Generator failed to ping main process, exiting");
			ret = 1;
			goto out;
		}
	} while (selret < 1);

	if (unlikely(proxi->cs->fd < 0)) {
		LOGWARNING("Upstream proxy %d:%s socket invalidated, will attempt failover",
			   proxi->id, proxi->cs->url);
		proxi->alive = false;
		proxi = NULL;
		goto reconnect;
	}

	sockd = accept(us->sockd, NULL, NULL);
	if (sockd < 0) {
		LOGEMERG("Failed to accept on proxy socket");
		ret = 1;
		goto out;
	}
	dealloc(buf);
	buf = recv_unix_msg(sockd);
	if (!buf) {
		LOGWARNING("Failed to get message in proxy_loop");
		Close(sockd);
		goto retry;
	}
	LOGDEBUG("Proxy received request: %s", buf);
	if (cmdmatch(buf, "shutdown")) {
		ret = 0;
		goto out;
	} else if (cmdmatch(buf, "reconnect")) {
		goto reconnect;
	} else if (cmdmatch(buf, "submitblock:")) {
		LOGNOTICE("Submitting likely block solve share to upstream pool");
	} else if (cmdmatch(buf, "loglevel")) {
		sscanf(buf, "loglevel=%d", &ckp->loglevel);
	} else if (cmdmatch(buf, "ping")) {
		LOGDEBUG("Proxy received ping request");
		send_unix_msg(sockd, "pong");
	} else if (cmdmatch(buf, "recruit")) {
		recruit_subproxy(gdata, proxi);
	} else if (ckp->passthrough) {
		/* Anything remaining should be stratum messages */
		passthrough_add_send(proxi, buf);
	} else {
		/* Anything remaining should be share submissions */
		json_t *val = json_loads(buf, 0, NULL);

		if (unlikely(!val))
			LOGWARNING("Generator received unrecognised message: %s", buf);
		else
			submit_share(gdata, val);
	}
	Close(sockd);
	goto retry;
out:
	Close(sockd);
	return ret;
}

static int server_mode(ckpool_t *ckp, proc_instance_t *pi)
{
	server_instance_t *si;
	int i, ret;

	ckp->servers = ckalloc(sizeof(server_instance_t *) * ckp->btcds);
	for (i = 0; i < ckp->btcds; i++) {
		ckp->servers[i] = ckzalloc(sizeof(server_instance_t));
		si = ckp->servers[i];
		si->url = ckp->btcdurl[i];
		si->auth = ckp->btcdauth[i];
		si->pass = ckp->btcdpass[i];
		si->notify = ckp->btcdnotify[i];
	}

	ret = gen_loop(pi);

	for (i = 0; i < ckp->btcds; i++) {
		si = ckp->servers[i];
		kill_server(si);
		dealloc(si);
	}
	dealloc(ckp->servers);
	return ret;
}

static int proxy_mode(ckpool_t *ckp, proc_instance_t *pi)
{
	gdata_t *gdata = ckp->data;
	proxy_instance_t *proxi;
	server_instance_t *si;
	int i, ret;

	mutex_init(&gdata->lock);

	/* Create all our proxy structures and pointers */
	ckp->servers = ckalloc(sizeof(server_instance_t *) * ckp->proxies);
	for (i = 0; i < ckp->proxies; i++) {
		ckp->servers[i] = ckzalloc(sizeof(server_instance_t));
		si = ckp->servers[i];
		si->id = i;
		si->url = strdup(ckp->proxyurl[i]);
		si->auth = strdup(ckp->proxyauth[i]);
		si->pass = strdup(ckp->proxypass[i]);
		proxi = ckzalloc(sizeof(proxy_instance_t));
		si->data = proxi;
		proxi->auth = si->auth;
		proxi->pass = si->pass;
		proxi->si = si;
		proxi->ckp = ckp;
		proxi->cs = &si->cs;
		proxi->cs->ckp = ckp;
		mutex_init(&proxi->notify_lock);
		mutex_init(&proxi->share_lock);
	}

	LOGWARNING("%s generator ready", ckp->name);

	ret = proxy_loop(pi);

	mutex_lock(&gdata->lock);
	for (i = 0; i < ckp->proxies; i++) {
		si = ckp->servers[i];
		Close(si->cs.fd);
		proxi = si->data;
		free(proxi->enonce1);
		free(proxi->enonce1bin);
		free(proxi->sessionid);
		pthread_cancel(proxi->pth_psend);
		pthread_cancel(proxi->pth_precv);
		join_pthread(proxi->pth_psend);
		join_pthread(proxi->pth_precv);
		dealloc(si->data);
		dealloc(si->url);
		dealloc(si->auth);
		dealloc(si->pass);
		dealloc(si);
	}
	mutex_unlock(&gdata->lock);

	dealloc(ckp->servers);
	return ret;
}

/* Tell the watchdog what the current server instance is and decide if we
 * should check to see if the higher priority servers are alive and fallback */
static void server_watchdog(ckpool_t *ckp, server_instance_t *cursi)
{
	static time_t last_t = 0;
	bool alive = false;
	time_t now_t;
	int i;

	/* Rate limit to checking only once every 5 seconds */
	now_t = time(NULL);
	if (now_t <= last_t + 5)
		return;

	last_t = now_t;

	/* Is this the highest priority server already? */
	if (!cursi->id)
		return;

	for (i = 0; i < ckp->btcds; i++) {
		server_instance_t *si  = ckp->servers[i];

		/* Have we reached the current server? */
		if (si == cursi)
			return;

		alive = server_alive(ckp, si, true);
		if (alive)
			break;
	}
	if (alive)
		send_proc(ckp->generator, "reconnect");
}

int generator(proc_instance_t *pi)
{
	ckpool_t *ckp = pi->ckp;
	gdata_t *gdata;
	int ret;

	LOGWARNING("%s generator starting", ckp->name);
	gdata = ckzalloc(sizeof(gdata_t));
	ckp->data = gdata;
	gdata->ckp = ckp;
	if (ckp->proxy) {
		ret = proxy_mode(ckp, pi);
	} else {
		gdata->srvchk = create_ckmsgq(ckp, "srvchk", &server_watchdog);
		ret = server_mode(ckp, pi);
	}

	dealloc(ckp->data);
	return process_exit(ckp, pi, ret);
}
