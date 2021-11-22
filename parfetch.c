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
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>
#include <event2/event.h>
#include <libias/array.h>
#include <libias/color.h>
#include <libias/flow.h>
#include <libias/io.h>
#include <libias/map.h>
#include <libias/mempool.h>
#include <libias/mempool/file.h>
#include <libias/queue.h>
#include <libias/set.h>
#include <libias/str.h>

#define MAX_PARALLEL 3 /* number of simultaneous transfers */

enum FetchDistfileNextReason {
	FETCH_DISTFILE_NEXT_MIRROR,
	FETCH_DISTFILE_NEXT_SIZE_MISMATCH,
};

enum ParfetchMode {
	PARFETCH_DO_FETCH,
	PARFETCH_FETCH_LIST,
	PARFETCH_FETCH_URL_LIST_INT,
	PARFETCH_MAKESUM,
};

enum SitesType {
	MASTER_SITES,
	PATCH_SITES,
};

struct DistinfoEntry {
	const char *name;
	const char *checksum;
	curl_off_t size;
};

struct Distfile {
	enum SitesType sites_type;
	const char *name;
	bool fetched;
	struct Array *groups;
	struct Queue *queue;
	FILE *fh;
	struct DistinfoEntry *distinfo;
};

struct DistfileQueueEntry {
	struct Distfile *distfile;
	const char *filename;
	const char *url;
	curl_off_t size;
};

struct CurlContext {
	struct CurlData *this;
	struct event *event;
	curl_socket_t sockfd;
};

struct CurlData {
	CURLM *cm;
	struct event_base *base;
	struct event *timeout;
};

// Prototypes
struct Distfile *parse_distfile_arg(struct Mempool *, struct Map *, enum SitesType, const char *);
static struct Map *load_distinfo(struct Mempool *);
static void create_distsubdirs(struct Mempool *, enum ParfetchMode, struct Array *);
static void queue_distfiles(struct Mempool *, enum ParfetchMode, struct Array *);
static void fetch_distfile(CURLM *, struct Queue *);
static void fetch_distfile_next_mirror(struct DistfileQueueEntry *, CURLM *, enum FetchDistfileNextReason, const char *);
static size_t fetch_distfile_write_cb(char *, size_t, size_t, void *);
static struct CurlContext *curl_context_new(curl_socket_t, struct CurlData *);
static void curl_context_free(struct CurlContext *);
static void curl_perform(int, short, void *);
static void check_multi_info(CURLM *);
static void on_timeout(evutil_socket_t, short, void *);
static int start_timeout(CURLM *, long, void *);
static int handle_socket(CURL *, curl_socket_t, int, void *, void *);

struct Distfile *
parse_distfile_arg(struct Mempool *pool, struct Map *distinfo, enum SitesType sites_type, const char *arg)
{
	struct Distfile *distfile = mempool_alloc(pool, sizeof(struct Distfile));
	distfile->sites_type = sites_type;
	distfile->queue = mempool_queue(pool);
	distfile->name = str_dup(pool, arg);
	distfile->fetched = false;
	char *groups = strrchr(distfile->name, ':');
	if (groups) {
		*groups = 0;
		distfile->groups = str_split(pool, groups + 1, ",");
	} else {
		distfile->groups = MEMPOOL_ARRAY(pool, "DEFAULT");
	}

	distfile->distinfo = map_get(distinfo, distfile->name);
	unless (distfile->distinfo) {
		err(1, "missing distinfo entry for %s", distfile->name);
	}

	return distfile;
}

struct Map *
load_distinfo(struct Mempool *extpool)
{
	SCOPE_MEMPOOL(pool);

	const char *distinfo_file = getenv("dp_DISTINFO_FILE");
	unless (distinfo_file) {
		errx(1, "dp_DISTINFO_FILE not set in the environment");
	}

	FILE *f = mempool_fopenat(pool, AT_FDCWD, distinfo_file, "r", 0);
	unless (f) {
		errx(1, "could not open %s", distinfo_file);
	}

	struct Map *distinfo = mempool_map(extpool, str_compare, NULL);
	LINE_FOREACH(f, line) {
		struct Array *fields = str_split(pool, line, " ");
		if (array_len(fields) != 4) {
			continue;
		}
		char *file = NULL;
		const char *checksum = NULL;
		const char *size = NULL;
		if (strcmp(array_get(fields, 0), "SHA256") == 0) {
			file = array_get(fields, 1);
			file++;
			file[strlen(file) - 1] = 0;
			checksum = array_get(fields, 3);
		} else if (strcmp(array_get(fields, 0), "SIZE") == 0) {
			file = array_get(fields, 1);
			file++;
			file[strlen(file) - 1] = 0;
			size = array_get(fields, 3);
		}

		struct DistinfoEntry *entry = map_get(distinfo, file);
		unless (entry) {
			entry = mempool_alloc(extpool, sizeof(struct DistinfoEntry));
			entry->name = str_dup(extpool, file);
			map_add(distinfo, entry->name, entry);
		}
		if (checksum) {
			entry->checksum = str_dup(extpool, checksum);
		} else if (size) {
			const char *errstr = NULL;
			entry->size = strtonum(size, 1, INT64_MAX, &errstr);
			if (errstr) {
				errx(1, "size for %s: %s", entry->name, errstr);
			}
		}
	}

	return distinfo;
}


void
create_distsubdirs(struct Mempool *pool, enum ParfetchMode mode, struct Array *distfiles)
{
	// Create subdirs once under dp_DISTDIR
	struct Set *distdirs = mempool_set(pool, str_compare, NULL);
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		SCOPE_MEMPOOL(_pool);
		if (strstr(distfile->name, "/")) {
			char *path = mempool_take(pool, dirname(str_dup(_pool, distfile->name)));
			set_add(distdirs, path);
		}
	}

	switch (mode) {
		case PARFETCH_DO_FETCH:
		case PARFETCH_MAKESUM:
			break;
		case PARFETCH_FETCH_LIST:
		case PARFETCH_FETCH_URL_LIST_INT:
			SET_FOREACH(distdirs, const char *, dir) {
				fprintf(stdout, "mkdir -p \"%s\"\n", dir);
			}
			break;
	}
}

void
queue_distfiles(struct Mempool *pool, enum ParfetchMode mode, struct Array *distfiles)
{
	// collect MASTER_SITES / PATCH_SITES per group and create mirror queues
	struct Map *groupsites[2];
	groupsites[MASTER_SITES] = mempool_map(pool, str_compare, NULL);
	groupsites[PATCH_SITES] = mempool_map(pool, str_compare, NULL);
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		const char *env_prefix[] = { "_MASTER_SITES_" , "_PATCH_SITES_" };
		ARRAY_FOREACH(distfile->groups, const char *, group) {
			struct Array *sites = map_get(groupsites[distfile->sites_type], group);
			unless (sites) {
				const char *sitesenv = getenv(str_printf(pool, "%s%s", env_prefix[distfile->sites_type], group));
				if (sitesenv == NULL) {
					errx(1, "cannot find %s%s for %s group", env_prefix[distfile->sites_type], group, group);
				}
				sites = str_split(pool, str_dup(pool, sitesenv), " ");
				const char *master_site_backup = getenv("dp_MASTER_SITE_BACKUP");
				if (master_site_backup && strcmp(master_site_backup, "") != 0) {
					ARRAY_JOIN(sites, str_split(pool, str_dup(pool, master_site_backup), " "));
				}
				map_add(groupsites[distfile->sites_type], group, sites);
			}

			ARRAY_FOREACH(sites, const char *, site) {
				struct DistfileQueueEntry *e = mempool_alloc(pool, sizeof(struct DistfileQueueEntry));
				e->distfile = distfile;
				e->filename = str_dup(pool, distfile->name);
				e->url = str_printf(pool, "%s%s", site, distfile->name);
				queue_push(distfile->queue, e);

				switch (mode) {
				case PARFETCH_DO_FETCH:
				case PARFETCH_MAKESUM:
					break;
				case PARFETCH_FETCH_LIST:
				case PARFETCH_FETCH_URL_LIST_INT:
					fprintf(stdout, "-o %s %s\n", e->filename, e->url);
					break;
				}
			}
		}
	}
}

void
fetch_distfile(CURLM *cm, struct Queue *distfile_queue)
{
	struct DistfileQueueEntry *queue_entry = queue_pop(distfile_queue);
	if (queue_entry) {
		if (queue_entry->distfile->fh) {
			fclose(queue_entry->distfile->fh);
		}
		queue_entry->distfile->fh = fopen(queue_entry->filename, "wb");
		unless (queue_entry->distfile->fh) {
			errx(1, "fopen: %s", queue_entry->filename);
		}
		CURL *eh = curl_easy_init();
		curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, fetch_distfile_write_cb);
		curl_easy_setopt(eh, CURLOPT_WRITEDATA, queue_entry);
		curl_easy_setopt(eh, CURLOPT_PRIVATE, queue_entry);
		curl_easy_setopt(eh, CURLOPT_URL, queue_entry->url);
		if (queue_entry->distfile->distinfo->size) {
			curl_easy_setopt(eh, CURLOPT_MAXFILESIZE_LARGE, queue_entry->distfile->distinfo->size);
		}
		curl_multi_add_handle(cm, eh);
		fprintf(stdout, ANSI_COLOR_BLUE "%-8s" ANSI_COLOR_RESET "%s\n",
			"queued", queue_entry->distfile->name);
		fprintf(stdout, "%8s%s\n", "", queue_entry->url);
	}
}

size_t
fetch_distfile_write_cb(char *data, size_t size, size_t nmemb, void *userdata)
{
	struct DistfileQueueEntry *queue_entry = userdata;
	size_t written = fwrite(data, size, nmemb, queue_entry->distfile->fh);
	queue_entry->size += written;
	return written;
}

struct CurlContext *
curl_context_new(curl_socket_t sockfd, struct CurlData *this)
{
	struct CurlContext *context = mempool_alloc(NULL, sizeof(struct CurlContext));
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
	curl_multi_socket_action(cm, context->sockfd, flags, &running_handles);
	check_multi_info(cm);
}

void
fetch_distfile_next_mirror(struct DistfileQueueEntry *queue_entry, CURLM *cm, enum FetchDistfileNextReason reason, const char *msg)
{
	const char *next_mirror_msg = "Trying next mirror...";
	if (queue_len(queue_entry->distfile->queue) == 0) {
		next_mirror_msg = "No more mirrors left!";
	}

	// Try to delete the file
	unlink(queue_entry->distfile->name);
	queue_entry->distfile->fetched = false;

	fprintf(stdout, ANSI_COLOR_RED "%-8s" ANSI_COLOR_RESET "%s\n", "error", queue_entry->distfile->name);
	fprintf(stdout, "%8s%s\n", "", queue_entry->url);

	switch (reason) {
	case FETCH_DISTFILE_NEXT_MIRROR:
		break;
	case FETCH_DISTFILE_NEXT_SIZE_MISMATCH:
		fprintf(stdout, "%8s" ANSI_COLOR_RED "size mismatch (expected: %zu, actual: %zu)" ANSI_COLOR_RESET "\n",
			"", queue_entry->distfile->distinfo->size, queue_entry->size);
		break;
	}
	if (msg) {
		fprintf(stdout, "%8s" ANSI_COLOR_RED "%s" ANSI_COLOR_RESET "\n", "", msg);
	}

	// queue next mirror for file
	fprintf(stdout, "%8s%s\n", "", next_mirror_msg);
	fetch_distfile(cm, queue_entry->distfile->queue);
}

void
check_multi_info(CURLM *cm)
{
	int pending;
	CURLMsg *message;
	while ((message = curl_multi_info_read(cm, &pending))) {
		switch (message->msg) {
		case CURLMSG_DONE: {
			struct DistfileQueueEntry *queue_entry = NULL;
			curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &queue_entry);
			if (queue_entry->distfile->fh) {
				fclose(queue_entry->distfile->fh);
			}
			switch (message->data.result) {
			case CURLE_OK: // no error
				if (queue_entry->size == queue_entry->distfile->distinfo->size) {
					queue_entry->distfile->fetched = true;
					fprintf(stdout, ANSI_COLOR_GREEN "%-8s" ANSI_COLOR_RESET "%s\n", "done", queue_entry->distfile->name);
				} else if (queue_entry->distfile->distinfo->size) {
					fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_SIZE_MISMATCH, NULL);
				}
				break;
			default: // error
				fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_MIRROR, curl_easy_strerror(message->data.result));
				break;
			}
			curl_multi_remove_handle(cm, message->easy_handle);
			curl_easy_cleanup(message->easy_handle);
			break;
		} default:
			fprintf(stdout, ANSI_COLOR_RED "%-8s" ANSI_COLOR_RESET "%d\n", "error", message->msg);
		break;
		}
	}
}

void
on_timeout(evutil_socket_t fd, short events, void *userdata)
{
	struct CurlData *this = userdata;
	int running_handles;
	curl_multi_socket_action(this->cm, CURL_SOCKET_TIMEOUT, 0, &running_handles);
	check_multi_info(this);
}

int
start_timeout(CURLM *multi, long timeout_ms, void *userdata)
{
	struct CurlData *this = userdata;

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
	struct CurlData *this = userdata;

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

int
main(int argc, char *argv[])
{
	struct Mempool *pool = mempool_new(); // XXX: pool might outlive main() via libcurl!

	const char *distdir = getenv("dp_DISTDIR");
	unless (distdir) {
		errx(1, "dp_DISTDIR not set in the environment");
	}
	if (chdir(distdir) == -1) {
		err(1, "chdir: %s", distdir);
	}

	struct Map *distinfo = load_distinfo(pool);
	struct Array *distfiles = mempool_array(pool);
	int ch;
	while ((ch = getopt(argc, argv, "d:p:")) != -1) {
		switch (ch) {
		case 'd':
			array_append(distfiles, parse_distfile_arg(pool, distinfo, MASTER_SITES, optarg));
			break;
		case 'p':
			array_append(distfiles, parse_distfile_arg(pool, distinfo, PATCH_SITES, optarg));
			break;
		case '?':
		default:
			errx(1, "unknown flag: %c\n", ch);
		}
	}
	argc -= optind;
	argv += optind;

	const char *target = getenv("dp_TARGET");
	unless (target) {
		errx(1, "dp_TARGET not set in the environment");
	}
	enum ParfetchMode mode = PARFETCH_DO_FETCH;
	if (strcmp(target, "fetch-list") == 0) {
		mode = PARFETCH_FETCH_LIST;
	} else if (strcmp(target, "fetch-url-list-int") == 0) {;
		mode = PARFETCH_FETCH_URL_LIST_INT;
	} else if (strcmp(target, "do-fetch") == 0) {
		mode = PARFETCH_DO_FETCH;
	} else if (strcmp(target, "makesum") == 0) {
		mode = PARFETCH_MAKESUM;
	} else {
		errx(1, "unknown dp_TARGET value: %s", target);
	}

	create_distsubdirs(pool, mode, distfiles);
	queue_distfiles(pool, mode, distfiles);

	// Check file existence and checksums if requested
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
	}

	if (curl_global_init(CURL_GLOBAL_ALL)) {
		errx(1, "could not init curl\n");
	}

	struct CurlData *this = mempool_alloc(pool, sizeof(struct CurlData));
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

	// max connects per host
	curl_multi_setopt(cm, CURLMOPT_MAXCONNECTS, (long)2);
	// max global connections
	curl_multi_setopt(cm, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)4);

	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		fetch_distfile(cm, distfile->queue);
	}

	// do the work
	event_base_dispatch(base);

	curl_multi_cleanup(cm);
	event_free(timeout);
	event_base_free(base);
	libevent_global_shutdown();
	curl_global_cleanup();

	// Close/flush all open files and check that we fetched all of them
	bool all_fetched = true;
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		if (distfile->fh) {
			fclose(distfile->fh);
		}
		all_fetched = all_fetched && distfile->fetched;
	}
	if (all_fetched) {
		return 0;
	} else {
		errx(1, "could not fetch all distfiles");
	}
}
