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
	const char *size;
};

struct Distfile {
	enum SitesType sites_type;
	bool fetched;
	const char *name;
	struct Array *groups;
	struct Queue *queue;
	FILE *fh;
};

struct DistfileQueueEntry {
	struct Distfile *distfile;
	const char *filename;
	const char *url;
};

// Prototypes
struct Distfile *parse_distfile_arg(struct Mempool *, enum SitesType, const char *);
static struct Map *load_distinfo(struct Mempool *);
static void create_distsubdirs(struct Mempool *, enum ParfetchMode, struct Array *);
static void queue_distfiles(struct Mempool *, enum ParfetchMode, struct Array *);
static void add_transfer(CURLM *, struct Queue *);
static size_t write_cb(char *, size_t, size_t, void *);

struct Distfile *
parse_distfile_arg(struct Mempool *pool, enum SitesType sites_type, const char *arg)
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
			entry->size = str_dup(extpool, size);
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
add_transfer(CURLM *cm, struct Queue *distfile_queue)
{
	struct DistfileQueueEntry *queue_entry = queue_pop(distfile_queue);
	if (queue_entry) {
		if (queue_entry->distfile->fh) {
			fclose(queue_entry->distfile->fh);
		}
		queue_entry->distfile->fh = fopen(queue_entry->filename, "w");
		unless (queue_entry->distfile->fh) {
			errx(1, "fopen: %s", queue_entry->filename);
		}
		CURL *eh = curl_easy_init();
		curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(eh, CURLOPT_WRITEDATA, queue_entry);
		curl_easy_setopt(eh, CURLOPT_PRIVATE, queue_entry);
		curl_easy_setopt(eh, CURLOPT_URL, queue_entry->url);
		curl_multi_add_handle(cm, eh);
		fprintf(stderr, ANSI_COLOR_BLUE "%-8s" ANSI_COLOR_RESET "%s \n%8s%s\n",
			"start", queue_entry->distfile->name, "", queue_entry->url);
	}
}

size_t
write_cb(char *data, size_t size, size_t nmemb, void *userdata)
{
	// XXX: since we aren't doing anything special here maybe
	// set CURLOPT_WRITEDATA to the FILE instead
	struct DistfileQueueEntry *queue_entry = userdata;
	size_t written = fwrite(data, size, nmemb, queue_entry->distfile->fh);
	return written;
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

	struct Array *distfiles = mempool_array(pool);
	int ch;
	while ((ch = getopt(argc, argv, "d:p:")) != -1) {
		switch (ch) {
		case 'd':
			array_append(distfiles, parse_distfile_arg(pool, MASTER_SITES, optarg));
			break;
		case 'p':
			array_append(distfiles, parse_distfile_arg(pool, PATCH_SITES, optarg));
			break;
		case '?':
		default:
			errx(1, "unknown flag: %c\n", ch);
		}
	}
	argc -= optind;
	argv += optind;

	struct Map *distinfo = load_distinfo(pool);

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

	curl_global_init(CURL_GLOBAL_ALL);
	CURLM *cm = curl_multi_init();

	// max connects per host
	curl_multi_setopt(cm, CURLMOPT_MAXCONNECTS, (long)2);
	// max global connections
	curl_multi_setopt(cm, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)4);

	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		add_transfer(cm, distfile->queue);
	}

	int still_alive = 1;
	do {
		curl_multi_perform(cm, &still_alive);

		CURLMsg *msg;
		int msgs_left = -1;
		while ((msg = curl_multi_info_read(cm, &msgs_left))) {
			puts("here");
			if (msg->msg == CURLMSG_DONE) {
				CURL *e = msg->easy_handle;
				struct DistfileQueueEntry *queue_entry = NULL;
				curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &queue_entry);
				if (msg->data.result == CURLE_OK) { // no error
					queue_entry->distfile->fetched = true;
					fprintf(stderr, ANSI_COLOR_GREEN "%-8s" ANSI_COLOR_RESET "%s\n", "ok", queue_entry->distfile->name);
				} else { // failed
					queue_entry->distfile->fetched = false;
					fprintf(stderr, ANSI_COLOR_RED "%-8s" ANSI_COLOR_RESET "%s \n%8s%s\n%8s" ANSI_COLOR_RED "%s" ANSI_COLOR_RESET "\n%8sTrying next mirror...\n",
						"error", queue_entry->distfile->name, "", queue_entry->url, "", curl_easy_strerror(msg->data.result), "");
					// queue next mirror for file
					add_transfer(cm, queue_entry->distfile->queue);
				}
				fclose(queue_entry->distfile->fh);
				curl_multi_remove_handle(cm, e);
				curl_easy_cleanup(e);
			} else {
				fprintf(stderr, ANSI_COLOR_RED "%-8s" ANSI_COLOR_RESET "%d\n", "error", msg->msg);
			}
		}

		// wait for activity, timeout or "nothing"
		curl_multi_wait(cm, NULL, 0, 1000, NULL);
	} while (still_alive);

	curl_multi_cleanup(cm);
	curl_global_cleanup();

	return 0;
}
