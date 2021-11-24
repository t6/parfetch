// SPDX-License-Identifier: BSD-2-Clause-FreeBSD
//
// Copyright (c) 2021 Tobias Kortkamp <tobik@FreeBSD.org>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.

#include "config.h"

#if HAVE_ERR
# include <err.h>
#endif
#include <stdlib.h>

#include <curl/curl.h>
#include <event2/event.h>
#include <libias/mem.h>

#include "loop.h"

struct CurlContext {
	struct ParfetchCurl *this;
	struct event *event;
	curl_socket_t sockfd;
};

struct ParfetchCurl {
	CURLM *cm;
	struct event_base *base;
	struct event *timeout;
	void (*check_multi_info)(CURLM *);
};

// Prototypes
static struct CurlContext *curl_context_new(curl_socket_t, struct ParfetchCurl *);
static void curl_context_free(struct CurlContext *);
static void curl_perform(int, short, void *);
static void on_timeout(evutil_socket_t, short, void *);
static int start_timeout(CURLM *, long, void *);
static int handle_socket(CURL *, curl_socket_t, int, void *, void *);

struct ParfetchCurl *
parfetch_curl_init(void (*check_multi_info)(CURLM *))
{
	if (curl_global_init(CURL_GLOBAL_ALL)) {
		errx(1, "could not init curl\n");
	}

	struct ParfetchCurl *this = xmalloc(sizeof(struct ParfetchCurl));
	this->check_multi_info = check_multi_info;
	CURLM *cm = curl_multi_init();
	this->cm = cm;
	struct event_base *base = event_base_new();
	this->base = base;
	struct event *timeout = evtimer_new(base, on_timeout, this);
	this->timeout = timeout;

	curl_multi_setopt(cm, CURLMOPT_SOCKETFUNCTION, handle_socket);
	curl_multi_setopt(cm, CURLMOPT_SOCKETDATA, this);
	curl_multi_setopt(cm, CURLMOPT_TIMERFUNCTION, start_timeout);
	curl_multi_setopt(cm, CURLMOPT_TIMERDATA, this);

	return this;
}

void
parfetch_curl_loop(struct ParfetchCurl *this)
{
	event_base_dispatch(this->base);
	curl_multi_cleanup(this->cm);
	event_free(this->timeout);
	event_base_free(this->base);
	libevent_global_shutdown();
	curl_global_cleanup();
	free(this);
}

struct event_base *
parfetch_curl_event_base(struct ParfetchCurl *this)
{
	return this->base;
}

CURLM *
parfetch_curl_multi(struct ParfetchCurl *this)
{
	return this->cm;
}


struct CurlContext *
curl_context_new(curl_socket_t sockfd, struct ParfetchCurl *this)
{
	struct CurlContext *context = xmalloc(sizeof(struct CurlContext));
	context->this = this;
	context->sockfd = sockfd;
	context->event = event_new(this->base, sockfd, 0, curl_perform, context);
	return context;
}

void
curl_context_free(struct CurlContext *context)
{
	event_del(context->event);
	event_free(context->event);
	free(context);
}

void
curl_perform(int fd, short event, void *userdata)
{
	int flags = 0;

	if (event & EV_READ) {
		flags |= CURL_CSELECT_IN;
	}
	if (event & EV_WRITE) {
		flags |= CURL_CSELECT_OUT;
	}

	struct CurlContext *context = userdata;
	int running_handles;
	CURLM *cm = context->this->cm; // context might be invalid after curl_multi_socket_action()
	void (*check_multi_info)(CURLM *) = context->this->check_multi_info;
	curl_multi_socket_action(cm, context->sockfd, flags, &running_handles);
	if (check_multi_info) {
		check_multi_info(cm);
	}
}

void
on_timeout(evutil_socket_t fd, short events, void *userdata)
{
	struct ParfetchCurl *this = userdata;
	int running_handles;
	curl_multi_socket_action(this->cm, CURL_SOCKET_TIMEOUT, 0, &running_handles);
	if (this->check_multi_info) {
		this->check_multi_info(this);
	}
}

int
start_timeout(CURLM *multi, long timeout_ms, void *userdata)
{
	struct ParfetchCurl *this = userdata;

	if (timeout_ms < 0) {
		evtimer_del(this->timeout);
	} else {
		if (timeout_ms == 0) {
			timeout_ms = 1; // 0 means directly call socket_action, but we will do it in a bit
			struct timeval tv;
			tv.tv_sec = timeout_ms / 1000;
			tv.tv_usec = (timeout_ms % 1000) * 1000;
			evtimer_del(this->timeout);
			evtimer_add(this->timeout, &tv);
		}
	}

	return 0;
}

int
handle_socket(CURL *easy, curl_socket_t s, int action, void *userdata, void *socketp)
{
	struct ParfetchCurl *this = userdata;

	switch(action) {
	case CURL_POLL_IN:
	case CURL_POLL_OUT:
	case CURL_POLL_INOUT: {
		struct CurlContext *curl_context;
		if (socketp) {
			curl_context = socketp;
		} else {
			curl_context = curl_context_new(s, this);
		}
		curl_multi_assign(this->cm, s, curl_context);

		int events = 0;
		if (action != CURL_POLL_IN) {
			events |= EV_WRITE;
		}
		if (action != CURL_POLL_OUT) {
			events |= EV_READ;
		}
		events |= EV_PERSIST;

		event_del(curl_context->event);
		event_assign(curl_context->event, this->base, curl_context->sockfd, events, curl_perform, curl_context);
		event_add(curl_context->event, NULL);
		break;
	} case CURL_POLL_REMOVE:
		if (socketp) {
			struct CurlContext *curl_context = socketp;
			event_del(curl_context->event);
			curl_context_free(curl_context);
			curl_multi_assign(this->cm, s, NULL);
		}
		break;
	default:
		abort();
	}

	return 0;
}
