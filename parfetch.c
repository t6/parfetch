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

#include <sys/stat.h>
#include <sys/types.h>
#if HAVE_ERR
# include <err.h>
#endif
#include <fcntl.h>
#include <libgen.h>
#if HAVE_SHA2
# include <sha2.h>
#endif
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

#include "mkdirs.h"

#define MAX_PARALLEL 3 /* number of simultaneous transfers */

enum FetchDistfileNextReason {
	FETCH_DISTFILE_NEXT_MIRROR,
	FETCH_DISTFILE_NEXT_CHECKSUM_MISMATCH,
	FETCH_DISTFILE_NEXT_SIZE_MISMATCH,
	FETCH_DISTFILE_NEXT_HTTP_ERROR,
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
	SHA2_CTX checksum_ctx;
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
static const char *makevar(const char *);
static struct Distfile *parse_distfile_arg(struct Mempool *, struct Map *, enum SitesType, const char *);
static struct Map *load_distinfo(struct Mempool *);
static void prepare_distfile_queues(struct Mempool *, struct Array *);
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

const char *
makevar(const char *var)
{
	SCOPE_MEMPOOL(pool);
	const char *key = str_printf(pool, "dp_%s", var);
	const char *env = getenv(key);
	if (env && strcmp(env, "") != 0) {
		return env;
	} else {
		return NULL;
	}
}

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

	if (!makevar("NO_CHECKSUM") && !makevar("DISABLE_SIZE")) {
		SCOPE_MEMPOOL(pool);
		const char *fullname;
		const char *dist_subdir = getenv("dp_DIST_SUBDIR");
		if (dist_subdir && strcmp(dist_subdir, "") != 0) {
			fullname = str_printf(pool, "%s/%s", dist_subdir, distfile->name);
		} else {
			fullname = distfile->name;
		}
		distfile->distinfo = map_get(distinfo, fullname);
		unless (distfile->distinfo) {
			errx(1, "missing distinfo entry for %s", fullname);
		}
	}

	return distfile;
}

struct Map *
load_distinfo(struct Mempool *extpool)
{
	SCOPE_MEMPOOL(pool);

	const char *distinfo_file = makevar("DISTINFO_FILE");
	unless (distinfo_file) {
		errx(1, "dp_DISTINFO_FILE not set in the environment");
	}

	struct Map *distinfo = mempool_map(extpool, str_compare, NULL);
	FILE *f = mempool_fopenat(pool, AT_FDCWD, distinfo_file, "r", 0);
	unless (f) {
		if (makevar("NO_CHECKSUM") && makevar("DISABLE_SIZE")) {
			warn("could not open %s", distinfo_file);
			return distinfo;
		} else {
			err(1, "could not open %s", distinfo_file);
		}
	}

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
prepare_distfile_queues(struct Mempool *pool, struct Array *distfiles)
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
				const char *master_site_backup = makevar("MASTER_SITE_BACKUP");
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
				SHA256Init(&e->checksum_ctx);
				queue_push(distfile->queue, e);
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
		{
			SCOPE_MEMPOOL(pool);
			char *dir = dirname(str_dup(pool, queue_entry->filename));
			unless (mkdirs(dir)) {
				err(1, "mkdir: %s", dir);
			}
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
		unless (makevar("DISABLE_SIZE")) {
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
	SHA256Update(&queue_entry->checksum_ctx, (u_int8_t *)data, written);
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
	case FETCH_DISTFILE_NEXT_CHECKSUM_MISMATCH:
		fprintf(stdout, "%8s" ANSI_COLOR_RED "%s" ANSI_COLOR_RESET "\n", "", "checksum mismatch");
		break;
	case FETCH_DISTFILE_NEXT_SIZE_MISMATCH:
		fprintf(stdout, "%8s" ANSI_COLOR_RED "size mismatch (expected: %zu, actual: %zu)" ANSI_COLOR_RESET "\n",
			"", queue_entry->distfile->distinfo->size, queue_entry->size);
		break;
	case FETCH_DISTFILE_NEXT_HTTP_ERROR:
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
				queue_entry->distfile->fh = NULL;
			}
			long response_code = 0;
			curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &response_code);
			if (response_code == 200 && message->data.result == CURLE_OK) { // no error
				if (makevar("DISABLE_SIZE") || queue_entry->size == queue_entry->distfile->distinfo->size) {
					if (makevar("NO_CHECKSUM")) {
						queue_entry->distfile->fetched = true;
						fprintf(stdout, ANSI_COLOR_GREEN "%-8s" ANSI_COLOR_RESET "%s\n", "done", queue_entry->distfile->name);
					} else {
						char digest[SHA256_DIGEST_STRING_LENGTH + 1];
						char *checksum = SHA256End(&queue_entry->checksum_ctx, digest);
						if (checksum && strcmp(checksum, queue_entry->distfile->distinfo->checksum) != 0) {
							fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_CHECKSUM_MISMATCH, NULL);
						} else {
							queue_entry->distfile->fetched = true;
							fprintf(stdout, ANSI_COLOR_GREEN "%-8s" ANSI_COLOR_RESET "%s\n", "done", queue_entry->distfile->name);
						}
					}
				} else unless (makevar("DISABLE_SIZE")) {
					fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_SIZE_MISMATCH, NULL);
				}
			} else { // error
				if (response_code > 0) {
					SCOPE_MEMPOOL(pool);
					const char *msg = str_printf(pool, "%ld", response_code);
					fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_HTTP_ERROR, msg);
				} else {
					fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_MIRROR, curl_easy_strerror(message->data.result));
				}
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

	const char *distdir = makevar("DISTDIR");
	unless (distdir) {
		errx(1, "dp_DISTDIR not set in the environment");
	}
	if (chdir(distdir) == -1) {
		err(1, "chdir: %s", distdir);
	}

	long max_connects_per_host = 1;
	long max_total_connections = 4;
	{
		const char *max_connects_per_host_env = makevar("PARFETCH_MAX_CONNECTS_PER_HOST");
		if (max_connects_per_host_env && strcmp(max_connects_per_host_env, "") != 0) {
			const char *errstr = NULL;
			max_connects_per_host = strtonum(max_connects_per_host_env, 1, LONG_MAX, &errstr);
			if (errstr) {
				errx(1, "PARFETCH_MAX_CONNECTS_PER_HOST: %s", errstr);
			}
		}
		const char *max_total_connections_env = makevar("PARFETCH_MAX_TOTAL_CONNECTIONS");
		if (max_total_connections_env && strcmp(max_total_connections_env, "") != 0) {
			const char *errstr = NULL;
			max_total_connections = strtonum(max_total_connections_env, 1, LONG_MAX, &errstr);
			if (errstr) {
				errx(1, "PARFETCH_MAX_TOTAL_CONNECTIONS: %s", errstr);
			}
		}
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

	const char *target = makevar("TARGET");
	unless (target) {
		errx(1, "dp_TARGET not set in the environment");
	}
	unless (strcmp(target, "do-fetch") == 0 || strcmp(target, "checksum") == 0 || strcmp(target, "makesum") == 0) {
		errx(1, "unsupported dp_TARGET value: %s", target);
	}

	prepare_distfile_queues(pool, distfiles);

	// Check file existence and checksums if requested
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		struct stat st;
		if (stat(distfile->name, &st) >= 0) {
			if (makevar("DISABLE_SIZE") || distfile->distinfo->size == st.st_size) {
				if (makevar("NO_CHECKSUM")) {
					distfile->fetched = true;
				} else {
					char buf[SHA256_DIGEST_STRING_LENGTH];
					char *checksum = SHA256File(distfile->name, buf);
					if (checksum && strcmp(checksum, distfile->distinfo->checksum) == 0) {
						distfile->fetched = true;
						fprintf(stdout, ANSI_COLOR_GREEN "%-8s" ANSI_COLOR_RESET "%s\n", "sha256", distfile->name);
					} else {
						distfile->fetched = false;
						fprintf(stdout, ANSI_COLOR_RED "%-8s" ANSI_COLOR_RESET "%s\n", "error", distfile->name);
						fprintf(stdout, "%8s" ANSI_COLOR_RED "%s" ANSI_COLOR_RESET "\n", "", "checksum mismatch");
						fprintf(stdout, "%8s%s\n", "", "Refetching...");
						unlink(distfile->name);
					}
				}
			} else {
				distfile->fetched = false;
				fprintf(stdout, ANSI_COLOR_RED "%-8s" ANSI_COLOR_RESET "%s\n", "error", distfile->name);
				fprintf(stdout, "%8s" ANSI_COLOR_RED "size mismatch (expected: %zu, actual: %zu)" ANSI_COLOR_RESET "\n",
					"", distfile->distinfo->size, st.st_size);
				fprintf(stdout, "%8s%s\n", "", "Refetching...");
				unlink(distfile->name);
			}
		} else {
			distfile->fetched = false;
			fprintf(stdout, ANSI_COLOR_BLUE "%-8s" ANSI_COLOR_RESET "%s\n", "missing", distfile->name);
		}
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
	curl_multi_setopt(cm, CURLMOPT_MAXCONNECTS, max_connects_per_host);
	curl_multi_setopt(cm, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_total_connections);

	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		unless (distfile->fetched) {
			fetch_distfile(cm, distfile->queue);
		}
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
