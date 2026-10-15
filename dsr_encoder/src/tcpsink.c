// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
/* dsr - Digital Satellite Radio (DSR) encoder — TCP stream sink */

#include "rf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int rf_tcp_open(void **out_private, const char *host, const char *port)
{
	if (!out_private || !host || !port)
		return -1;

	struct addrinfo hints, *res = NULL, *rp = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	int err = getaddrinfo(host, port, &hints, &res);
	if (err) {
		fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host, port, gai_strerror(err));
		return -1;
	}

	int sock = -1;
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock < 0)
			continue;
		if (connect(sock, rp->ai_addr, rp->ai_addrlen) != 0) {
			close(sock);
			sock = -1;
			continue;
		}
		break;
	}
	freeaddrinfo(res);

	if (sock < 0) {
		fprintf(stderr, "TCP connect(%s:%s) failed\n", host, port);
		return -1;
	}

	rf_tcp_t *t = (rf_tcp_t *)calloc(1, sizeof(*t));
	if (!t) {
		close(sock);
		return -1;
	}
	t->sock = sock;
	t->preview_done = 0;
	*out_private = t;
	return 0;
}

int rf_tcp_close(void *priv)
{
	rf_tcp_t *t = (rf_tcp_t *)priv;
	if (!t)
		return 0;
	if (t->sock >= 0)
		close(t->sock);
	free(t);
	return 0;
}
