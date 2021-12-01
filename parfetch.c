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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>
#include <event2/event.h>
#include <rhash.h>

#include <libias/array.h>
#include <libias/color.h>
#include <libias/distinfo.h>
#include <libias/flow.h>
#include <libias/io.h>
#include <libias/map.h>
#include <libias/mempool.h>
#include <libias/mempool/file.h>
#include <libias/queue.h>
#include <libias/set.h>
#include <libias/str.h>

#include "loop.h"
#include "progress.h"

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

struct ParfetchOptions {
	FILE *out;

	const char *color_error;
	const char *color_info;
	const char *color_ok;
	const char *color_reset;
	const char *color_warning;

	const char *distdir;
	const char *dist_subdir;
	const char *distinfo_file;
	const char *target;

	long max_host_connections;
	long max_total_connections;
	bool disable_size;
	bool no_checksum;
	bool makesum;
	bool makesum_ephemeral;
	bool makesum_keep_timestamp;
	bool want_colors;
};

struct Distfile {
	struct Mempool *pool;
	enum SitesType sites_type;
	const char *name;
	bool fetched;
	struct Array *groups;
	struct Queue *queue;
	FILE *fh;
	struct DistinfoEntry *distinfo;
};

struct DistfileQueueEntry {
	struct Distinfo *distinfo;
	struct Progress *progress;
	struct Distfile *distfile;
	const char *filename;
	const char *url;
	rhash checksum_ctx;
	curl_off_t size;
	curl_off_t dltotal;
};

struct InitialDistfileCheckData {
	struct Mempool *pool;
	struct event_base *base;
	struct event *event;
	struct Queue *files_to_checksum;
	struct Array *finished_files;
	struct Distfile *distfile;
	int fd;
	int error;
	rhash checksum_ctx;
};

// Prototypes
static const char *makevar(const char *);
static void parfetch_init_options(void);
static struct Distfile *parse_distfile_arg(struct Mempool *, struct Distinfo *, enum SitesType, const char *);
static struct Distinfo *load_distinfo(struct Mempool *);
static bool check_checksum(struct Distinfo *, struct Distfile *, rhash);
static void prepare_distfile_queues(struct Mempool *, struct Distinfo *, struct Progress *, struct Array *);
static void initial_distfile_check(struct Distinfo *, struct Array *);
static void initial_distfile_check_queue_file(struct Mempool *, struct event_base *, struct Queue *, struct Array *);
static void initial_distfile_check_cb(evutil_socket_t, short, void *);
static void fetch_distfile(CURLM *, struct Queue *);
static void fetch_distfile_next_mirror(struct DistfileQueueEntry *, CURLM *, enum FetchDistfileNextReason, const char *);
static size_t fetch_distfile_progress_cb(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t);
static size_t fetch_distfile_write_cb(char *, size_t, size_t, void *);
static void check_multi_info(CURLM *);
static bool response_code_ok(long, long);

static struct ParfetchOptions opts;
// basically how many open files we have at a time
static const size_t INITIAL_DISTFILE_CHECK_QUEUE_SIZE = 64;

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

void
parfetch_init_options()
{
	opts.target = makevar("TARGET");
	unless (opts.target) {
		errx(1, "dp_TARGET not set in the environment");
	}
	unless (strcmp(opts.target, "do-fetch") == 0 || strcmp(opts.target, "checksum") == 0 || strcmp(opts.target, "makesum") == 0) {
		errx(1, "unsupported dp_TARGET value: %s", opts.target);
	}

	opts.out = stdout;
	opts.color_error = ANSI_COLOR_RED;
	opts.color_info = ANSI_COLOR_BLUE;
	opts.color_ok = ANSI_COLOR_GREEN;
	opts.color_reset = ANSI_COLOR_RESET;
	opts.color_warning = ANSI_COLOR_YELLOW;
	opts.want_colors = true;
	unless (can_use_colors(opts.out)) {
		opts.want_colors = false;
		opts.color_error = opts.color_info = opts.color_ok = opts.color_reset = opts.color_warning = "";
	}

	opts.distdir = makevar("DISTDIR");
	unless (opts.distdir) {
		errx(1, "dp_DISTDIR not set in the environment");
	}
	opts.distinfo_file = makevar("DISTINFO_FILE");
	unless (opts.distinfo_file) {
		errx(1, "dp_DISTINFO_FILE not set in the environment");
	}
	opts.dist_subdir = makevar("DIST_SUBDIR");

	opts.makesum = makevar("_PARFETCH_MAKESUM");
	opts.makesum_ephemeral = makevar("PARFETCH_MAKESUM_EPHEMERAL");
	opts.makesum_keep_timestamp = makevar("PARFETCH_MAKESUM_KEEP_TIMESTAMP");
	opts.disable_size = makevar("DISABLE_SIZE");
	opts.no_checksum = makevar("NO_CHECKSUM");

	opts.max_host_connections = 1;
	opts.max_total_connections = 4;
	const char *max_host_connections_env = makevar("PARFETCH_MAX_HOST_CONNECTIONS");
	if (max_host_connections_env && strcmp(max_host_connections_env , "") != 0) {
		const char *errstr = NULL;
		opts.max_host_connections = strtonum(max_host_connections_env , 1, LONG_MAX, &errstr);
		if (errstr) {
			errx(1, "PARFETCH_MAX_HOST_CONNECTIONS: %s", errstr);
		}
	}
	const char *max_total_connections_env = makevar("PARFETCH_MAX_TOTAL_CONNECTIONS");
	if (max_total_connections_env && strcmp(max_total_connections_env, "") != 0) {
		const char *errstr = NULL;
		opts.max_total_connections = strtonum(max_total_connections_env, 1, LONG_MAX, &errstr);
		if (errstr) {
			errx(1, "PARFETCH_MAX_TOTAL_CONNECTIONS: %s", errstr);
		}
	}
}

struct Distfile *
parse_distfile_arg(struct Mempool *pool, struct Distinfo *distinfo, enum SitesType sites_type, const char *arg)
{
	struct Distfile *distfile = mempool_alloc(pool, sizeof(struct Distfile));
	distfile->pool = pool;
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

	{
		SCOPE_MEMPOOL(pool);
		const char *fullname;
		if (opts.dist_subdir) {
			fullname = str_printf(pool, "%s/%s", opts.dist_subdir, distfile->name);
		} else {
			fullname = distfile->name;
		}
		distfile->distinfo = distinfo_entry(distinfo, fullname);
		if (!distfile->distinfo && opts.makesum) {
			// We add a new entry so update the timestamp
			unless (opts.makesum_keep_timestamp) {
				distinfo_set_timestamp(distinfo, time(NULL));
			}
			distinfo_add_entry(distinfo, &(struct DistinfoEntry){
				.filename = fullname,
			});
			distfile->distinfo = distinfo_entry(distinfo, fullname);
		}
		unless (distfile->distinfo) {
			if (!opts.no_checksum && !opts.disable_size) {
				errx(1, "missing distinfo entry for %s", fullname);
			}
		}
	}

	return distfile;
}

struct Distinfo *
load_distinfo(struct Mempool *extpool)
{
	SCOPE_MEMPOOL(pool);

	FILE *f = mempool_fopenat(pool, AT_FDCWD, opts.distinfo_file, "r", 0);
	unless (f) {
		if (opts.makesum) {
			struct Distinfo *distinfo = distinfo_new();
			distinfo_set_timestamp(distinfo, time(NULL));
			return distinfo;
		} else if (opts.no_checksum && opts.disable_size) {
			return NULL;
		} else {
			err(1, "could not open %s", opts.distinfo_file);
		}
	}

	struct Array *errors = NULL;
	struct Distinfo *distinfo = distinfo_parse(f, pool, &errors);
	unless (distinfo) {
		warnx("could not parse %s", opts.distinfo_file);
		ARRAY_FOREACH(errors, const char *, line) {
			fprintf(stderr, "%s:%s\n", opts.distinfo_file, line);
		}
		exit(1);
	}
	// Add a timestamp in case it is missing
	if (distinfo_timestamp(distinfo) == 0) {
		distinfo_set_timestamp(distinfo, time(NULL));
	}
	return distinfo;
}

bool
check_checksum(struct Distinfo *distinfo, struct Distfile *distfile, rhash ctx)
{
	if (opts.no_checksum && !opts.makesum) {
		return true;
	} else if (distfile->distinfo) {
		unless (rhash_final(ctx, NULL) == 0) {
			if (opts.makesum) {
				err(1, "could not checksum %s", distfile->name);
			} else {
				return false;
			}
		}
		char checksum[130];
		if (rhash_print(checksum, ctx, RHASH_SHA256, RHPR_HEX) == 0) {
			if (opts.makesum) {
				errx(1, "could not checksum %s", distfile->name);
			} else {
				return false;
			}
		}
		if (opts.makesum) {
			if (distfile->distinfo->digest) {
				if (strcmp(distfile->distinfo->digest, checksum) != 0) {
					unless (opts.makesum_keep_timestamp) {
						distinfo_set_timestamp(distinfo, time(NULL));
					}
					distfile->distinfo->digest = str_dup(distfile->pool, checksum);
				}
			} else {
				unless (opts.makesum_keep_timestamp) {
					distinfo_set_timestamp(distinfo, time(NULL));
				}
				distfile->distinfo->digest = str_dup(distfile->pool, checksum);
			}
			return true;
		} else {
			return strcasecmp(checksum, distfile->distinfo->digest) == 0;
		}
	} else {
		errx(1, "NO_CHECKSUM not set but distinfo not loaded");
	}
}

void
prepare_distfile_queues(struct Mempool *pool, struct Distinfo *distinfo, struct Progress *progress, struct Array *distfiles)
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
				sites = mempool_array(pool);
				// Prepend MASTER_SITE_OVERRIDE if it is set
				const char *master_site_override = makevar("MASTER_SITE_OVERRIDE");
				if (master_site_override) {
					array_append(sites, str_dup(pool, master_site_override));
				}
				const char *sitesenv = getenv(str_printf(pool, "%s%s", env_prefix[distfile->sites_type], group));
				if (sitesenv == NULL) {
					errx(1, "cannot find %s%s for %s group", env_prefix[distfile->sites_type], group, group);
				}
				ARRAY_JOIN(sites, str_split(pool, str_dup(pool, sitesenv), " "))
				const char *master_site_backup = makevar("MASTER_SITE_BACKUP");
				if (master_site_backup) {
					ARRAY_JOIN(sites, str_split(pool, str_dup(pool, master_site_backup), " "));
				}
				map_add(groupsites[distfile->sites_type], group, sites);
			}

			ARRAY_FOREACH(sites, const char *, site) {
				struct DistfileQueueEntry *e = mempool_alloc(pool, sizeof(struct DistfileQueueEntry));
				e->distinfo = distinfo;
				e->progress = progress;
				e->distfile = distfile;
				e->filename = str_dup(pool, distfile->name);
				e->url = str_printf(pool, "%s%s", site, distfile->name);
				e->checksum_ctx = mempool_add(pool, rhash_init(RHASH_SHA256), rhash_free);
				queue_push(distfile->queue, e);
			}
		}
	}
}

void
initial_distfile_check_cb(evutil_socket_t fd, short what, void *userdata)
{
	struct InitialDistfileCheckData *this = userdata;
	uint8_t buf[65536];
	ssize_t nread = read(fd, buf, sizeof(buf));
	if (nread < 0) {
		unless (errno == EAGAIN) {
			this->error = errno;
			event_del(this->event);
			close(this->fd);
			this->fd = -1;
			array_append(this->finished_files, this);
		}
		return;
	} else if (nread != 0) {
		rhash_update(this->checksum_ctx, buf, nread);
	}

	if (nread == 0 || nread != sizeof(buf)) {
		event_del(this->event);
		close(this->fd);
		this->fd = -1;
		array_append(this->finished_files, this);
		initial_distfile_check_queue_file(this->pool, this->base, this->files_to_checksum, this->finished_files);
	}
}

void
initial_distfile_check_queue_file(struct Mempool *pool, struct event_base *base, struct Queue *files_to_checksum, struct Array *finished_files)
{
	struct Distfile *distfile = queue_pop(files_to_checksum);
	unless (distfile) {
		return;
	}
	struct InitialDistfileCheckData *this = mempool_alloc(pool, sizeof(struct InitialDistfileCheckData));
	this->checksum_ctx = mempool_add(pool, rhash_init(RHASH_SHA256), rhash_free);
	this->pool = pool;
	this->base = base;
	this->distfile = distfile;
	this->files_to_checksum = files_to_checksum;
	this->finished_files = finished_files;
	this->fd = open(this->distfile->name, O_RDONLY | O_NONBLOCK);
	if (this->fd == -1) {
		this->error = errno;
		array_append(finished_files, this);
	} else {
		this->event = event_new(base, this->fd, EV_READ | EV_PERSIST, initial_distfile_check_cb, this);
		event_add(this->event, NULL);
	}
}

void
initial_distfile_check(struct Distinfo *distinfo, struct Array *distfiles)
{
	SCOPE_MEMPOOL(pool);

	// Check file existence and checksums if requested
	struct Queue *files_to_checksum = mempool_queue(pool);
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		struct stat st;
		if (stat(distfile->name, &st) >= 0) {
			if (opts.makesum) {
				distfile->fetched = true;
				if (distfile->distinfo->size != st.st_size) {
					unless (opts.makesum_keep_timestamp) {
						distinfo_set_timestamp(distinfo, time(NULL));
					}
					distfile->distinfo->size = st.st_size;
				}
				queue_push(files_to_checksum, distfile);
			} else if (opts.disable_size) {
				if (opts.no_checksum) {
					distfile->fetched = true;
				} else {
					queue_push(files_to_checksum, distfile);
				}
			} else if (distfile->distinfo) {
				if (distfile->distinfo->size == st.st_size) {
					if (opts.no_checksum) {
						distfile->fetched = true;
					} else {
						queue_push(files_to_checksum, distfile);
					}
				} else {
					fprintf(opts.out, "%s%-8s%s%s %ssize mismatch (expected: %zu, actual: %zu)%s\n", opts.color_error, "error", opts.color_reset, distfile->name,
						opts.color_error, distfile->distinfo->size, st.st_size, opts.color_reset);
					fprintf(opts.out, "%s%-8s%s%s\n", opts.color_warning, "unlink", opts.color_reset, distfile->name);
					unlink(distfile->name);
					distfile->fetched = false;
				}
			} else {
				err(1, "DISABLE_SIZE not set but distinfo not loaded");
			}
		} else { // missing
			distfile->fetched = false;
		}
	}

	struct event_base *base = mempool_add(pool, event_base_new(), event_base_free);
	struct Array *finished_files = mempool_array(pool);
	for (size_t i = 0; i < INITIAL_DISTFILE_CHECK_QUEUE_SIZE && queue_len(files_to_checksum) != 0; i++) {
		initial_distfile_check_queue_file(pool, base, files_to_checksum, finished_files);
	}
	event_base_dispatch(base);

	// We finish the checksumming afterwards to avoid pauses.
	// It might take a while and could block the event loop.
	ARRAY_FOREACH(finished_files, struct InitialDistfileCheckData *, this) {
		if (this->error != 0) {
			fprintf(opts.out, "%s%-8s%s%s could not checksum: %s%s%s\n",
				opts.color_error, "error", opts.color_reset, this->distfile->name,
				opts.color_error, strerror(this->error), opts.color_reset);
			fprintf(opts.out, "%s%-8s%s%s\n", opts.color_warning, "unlink", opts.color_reset, this->distfile->name);
			unlink(this->distfile->name);
			this->distfile->fetched = false;
		} else if (check_checksum(distinfo, this->distfile, this->checksum_ctx)) {
			this->distfile->fetched = true;
			fprintf(opts.out, "%s%-8s%s%s\n", opts.color_ok, "ok", opts.color_reset, this->distfile->name);
		} else if (opts.makesum) {
			panic("check_checksum() returned with failure in makesum mode");
		} else {
			fprintf(opts.out, "%s%-8s%s%s %s%s%s\n",
				opts.color_error, "error", opts.color_reset, this->distfile->name,
				opts.color_error, "checksum mismatch", opts.color_reset);
			fprintf(opts.out, "%s%-8s%s%s\n", opts.color_warning, "unlink", opts.color_reset, this->distfile->name);
			unlink(this->distfile->name);
			this->distfile->fetched = false;
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
		if (opts.makesum && opts.makesum_ephemeral) {
			queue_entry->distfile->fh = NULL;
		} else {
			SCOPE_MEMPOOL(pool);
			char *dir = dirname(str_dup(pool, queue_entry->filename));
			unless (mkdirp(dir)) {
				err(1, "mkdirp: %s", dir);
			}
			queue_entry->distfile->fh = fopen(queue_entry->filename, "wb");
			unless (queue_entry->distfile->fh) {
				errx(1, "could not open: %s", queue_entry->filename);
			}
		}
		CURL *eh = curl_easy_init();
		curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, fetch_distfile_write_cb);
		curl_easy_setopt(eh, CURLOPT_WRITEDATA, queue_entry);
		curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(eh, CURLOPT_XFERINFOFUNCTION, fetch_distfile_progress_cb);
		curl_easy_setopt(eh, CURLOPT_XFERINFODATA, queue_entry);
		curl_easy_setopt(eh, CURLOPT_PRIVATE, queue_entry);
		curl_easy_setopt(eh, CURLOPT_URL, queue_entry->url);
		if (opts.disable_size) {
			// nothing
		} else if (queue_entry->distfile->distinfo) {
			curl_easy_setopt(eh, CURLOPT_MAXFILESIZE_LARGE, queue_entry->distfile->distinfo->size);
		}
		const char *fetch_env = makevar("FETCH_ENV");
		if (fetch_env) {
			SCOPE_MEMPOOL(pool);
			ARRAY_FOREACH(str_split(pool, fetch_env, ""), const char *, value) {
				if (strcmp(value, "SSL_NO_VERIFY_PEER=1") != 0) {
					curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 0L);
				} else if (strcmp(value, "SSL_NO_VERIFY_HOSTNAME=1") != 0) {
					curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 0L);
				} else {
					warnx("unhandled value in FETCH_ENV: %s", value);
				}
			}
		}
		curl_multi_add_handle(cm, eh);
		fprintf(opts.out, "%s%-8s%s%s\n", opts.color_info, "queued", opts.color_reset, queue_entry->url);
	}
}

size_t
fetch_distfile_progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	struct DistfileQueueEntry *queue_entry = userdata;
	if (opts.makesum) {
		// In makesum mode we don't know the size upfront
		// so once curl knows update the total number of
		// bytes.
		if (dltotal != queue_entry->dltotal) {
			progress_update_total(queue_entry->progress, -queue_entry->dltotal);
			progress_update_total(queue_entry->progress, dltotal);
			queue_entry->dltotal = dltotal;
		}
	}
	return 0;
}

size_t
fetch_distfile_write_cb(char *data, size_t size, size_t nmemb, void *userdata)
{
	struct DistfileQueueEntry *queue_entry = userdata;
	size_t written;
	if (queue_entry->distfile->fh) {
		written = fwrite(data, size, nmemb, queue_entry->distfile->fh);
	} else {
		written = size * nmemb;
	}
	queue_entry->size += written;
	progress_update(queue_entry->progress, written, queue_entry->distfile->name);
	rhash_update(queue_entry->checksum_ctx, data, written);
	return written;
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
	progress_update(queue_entry->progress, -queue_entry->size, NULL);
	queue_entry->size = 0;

	fprintf(opts.out, "%s%-8s%s%s", opts.color_error, "error", opts.color_reset, queue_entry->url);

	switch (reason) {
	case FETCH_DISTFILE_NEXT_MIRROR:
		fputc('\n', opts.out);
		break;
	case FETCH_DISTFILE_NEXT_CHECKSUM_MISMATCH:
		fprintf(opts.out, " %s%s%s\n", opts.color_error, "checksum mismatch", opts.color_reset);
		break;
	case FETCH_DISTFILE_NEXT_SIZE_MISMATCH:
		if (queue_entry->distfile->distinfo) {
			fprintf(opts.out, " %ssize mismatch (expected: %zu, actual: %zu)%s\n",
				opts.color_error, queue_entry->distfile->distinfo->size, queue_entry->size, opts.color_reset);
		} else {
			fputc('\n', opts.out);
		}
		break;
	case FETCH_DISTFILE_NEXT_HTTP_ERROR:
		fputc('\n', opts.out);
		break;
	}
	if (msg) {
		fprintf(opts.out, "%8s%s%s%s\n", "", opts.color_error, msg, opts.color_reset);
	}

	// queue next mirror for file
	fprintf(opts.out, "%8s%s\n", "", next_mirror_msg);

	fprintf(opts.out, "%s%-8s%s%s\n", opts.color_warning, "unlink", opts.color_reset, queue_entry->distfile->name);
	fetch_distfile(cm, queue_entry->distfile->queue);
}

bool
response_code_ok(long code, long protocol)
{
	switch (protocol) {
	case CURLPROTO_FTP:
	case CURLPROTO_FTPS:
		if (code == 226) {
			return true;
		}
		break;
	case CURLPROTO_HTTP:
	case CURLPROTO_HTTPS:
		if (code == 200) {
			return true;
		}
		break;
	default:
		errx(1, "unsupported protocol: %ld", protocol);
	}

	return false;
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
			// message becomes invalid after curl_easy_cleanup() or curl_multi_remove_handle()!
			CURL *easy_handle = message->easy_handle;
			curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &queue_entry);
			if (queue_entry->distfile->fh) {
				fclose(queue_entry->distfile->fh);
				queue_entry->distfile->fh = NULL;
			}
			long response_code = 0;
			if (CURLE_OK != curl_easy_getinfo(easy_handle, CURLINFO_RESPONSE_CODE, &response_code) || response_code == 0) {
				goto general_curl_error;
			}
			long protocol = 0;
			if (CURLE_OK != curl_easy_getinfo(easy_handle, CURLINFO_PROTOCOL, &protocol) || protocol == 0) {
				goto general_curl_error;
			}
			if (response_code_ok(response_code, protocol) && message->data.result == CURLE_OK) { // no error
				if (opts.disable_size) {
					if (opts.makesum && queue_entry->distfile->distinfo->size != queue_entry->size) {
						unless (opts.makesum_keep_timestamp) {
							distinfo_set_timestamp(queue_entry->distinfo, time(NULL));
						}
						queue_entry->distfile->distinfo->size = queue_entry->size;
					}
					if (check_checksum(queue_entry->distinfo, queue_entry->distfile, queue_entry->checksum_ctx)) {
						queue_entry->distfile->fetched = true;
						fprintf(opts.out, "%s%-8s%s%s\n", opts.color_ok, "done", opts.color_reset, queue_entry->distfile->name);
					} else {
						fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_CHECKSUM_MISMATCH, NULL);
					}
				} else if (queue_entry->distfile->distinfo) {
					if (queue_entry->size == queue_entry->distfile->distinfo->size) {
						if (check_checksum(queue_entry->distinfo, queue_entry->distfile, queue_entry->checksum_ctx)) {
							queue_entry->distfile->fetched = true;
							fprintf(opts.out, "%s%-8s%s%s\n", opts.color_ok, "done", opts.color_reset, queue_entry->distfile->name);
						} else {
							fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_CHECKSUM_MISMATCH, NULL);
						}
					} else {
						fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_SIZE_MISMATCH, NULL);
					}
				} else {
					errx(1, "DISABLE_SIZE not set but distinfo not loaded");
				}
			} else if (response_code_ok(response_code, protocol)) { // curl error but ok response
				fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_MIRROR, curl_easy_strerror(message->data.result));
			} else if (response_code > 0) { // bad response code
				SCOPE_MEMPOOL(pool);
				const char *msg = str_printf(pool, "status %ld", response_code);
				fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_HTTP_ERROR, msg);
			} else { // general curl error
general_curl_error:
				fetch_distfile_next_mirror(queue_entry, cm, FETCH_DISTFILE_NEXT_MIRROR, curl_easy_strerror(message->data.result));
			}
			curl_multi_remove_handle(cm, easy_handle);
			curl_easy_cleanup(easy_handle);
			break;
		} default:
			fprintf(opts.out, "%s%-8s%s%d\n", opts.color_error, "error", opts.color_reset, message->msg);
			break;
		}
	}
}

int
main(int argc, char *argv[])
{
	SCOPE_MEMPOOL(pool);

	parfetch_init_options();
	rhash_library_init();

	unless (opts.makesum && opts.makesum_ephemeral) {
		unless (mkdirp(opts.distdir)) {
			err(1, "mkdirp: %s", opts.distdir);
		}
		if (chdir(opts.distdir) == -1) {
			err(1, "chdir: %s", opts.distdir);
		}
	}

	struct Distinfo *distinfo = load_distinfo(pool);
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

	if (curl_global_init(CURL_GLOBAL_ALL)) {
		errx(1, "could not init curl");
	}

	CURLM *cm = curl_multi_init();
	curl_multi_setopt(cm, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
	curl_multi_setopt(cm, CURLMOPT_MAX_HOST_CONNECTIONS, opts.max_host_connections);
	curl_multi_setopt(cm, CURLMOPT_MAX_TOTAL_CONNECTIONS, opts.max_total_connections);

	struct event_base *base = event_base_new();
	struct Progress *progress = progress_new(base, opts.out);
	struct ParfetchCurl *loop = parfetch_curl_new(cm, base, check_multi_info, progress_stop, progress);
	unless (opts.makesum) {
		ARRAY_FOREACH(distinfo_entries(distinfo, pool), struct DistinfoEntry *, entry) {
			progress_update_total(progress, entry->size);
		}
	}

	prepare_distfile_queues(pool, distinfo, progress, distfiles);
	initial_distfile_check(distinfo, distfiles);

	// do the work if needed
	bool fetch = false;
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		unless (distfile->fetched) {
			fetch = true;
			fetch_distfile(cm, distfile->queue);
		}
	}
	if (fetch) {
		event_base_dispatch(base);
	}

	// cleanup
	parfetch_curl_free(loop);
	progress_free(progress);
	event_base_free(base);
	curl_multi_cleanup(cm);
	curl_global_cleanup();
	libevent_global_shutdown();

	// Close/flush all open files and check that we fetched all of them
	bool all_fetched = true;
	ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
		if (distfile->fh) {
			fclose(distfile->fh);
		}
		all_fetched = all_fetched && distfile->fetched;
	}
	if (all_fetched) {
		if (opts.makesum) {
			SCOPE_MEMPOOL(pool);
			FILE *f = mempool_fopenat(pool, AT_FDCWD, opts.distinfo_file, "w", 0644);
			unless (f) {
				err(1, "could not open %s", opts.distinfo_file);
			}
			fprintf(f, "TIMESTAMP = %ju\n", (uintmax_t)distinfo_timestamp(distinfo));
			ARRAY_FOREACH(distfiles, struct Distfile *, distfile) {
				distinfo_entry_serialize(distfile->distinfo, f);
			}
			fprintf(opts.out, "%s%-8s%s%s\n", opts.color_info, "updated", opts.color_reset, opts.distinfo_file);
		}
		return 0;
	} else {
		errx(1, "could not fetch all distfiles");
	}
}
