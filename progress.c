#include "config.h"

#include <sys/ioctl.h>
#include <sys/param.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <event2/event.h>

#include <libias/array.h>
#include <libias/flow.h>
#include <libias/mem.h>
#include <libias/mempool.h>
#include <libias/str.h>

#include "progress.h"

struct Progress {
	struct event_base *base;
	struct event *timeout;
	struct event *sigint_event;
	struct event *sigwinch_event;
	FILE *out;
	char *current_file;
	off_t current_bytes;
	off_t total_bytes;
	struct winsize winsize;
	bool initialized;
	bool progressbar;
};

// Prototypes
static void on_sigint(evutil_socket_t, short, void *);
static void on_sigwinch(evutil_socket_t, short, void *);
static void on_timeout(evutil_socket_t, short, void *);
static void progress_go_to_bar_row(struct Progress *);
static void progress_set_winsize(struct Progress *, unsigned short);
static void progress_step(struct Progress *);

static const size_t PROGRESS_BAR_WIDTH = 15;

static const char *cursor_up = "\e[1A";
static const char *cursor_save = "\e7";
static const char *cursor_restore = "\e8";
static const char *erase_below = "\e[0J";
static const char *erase_line_all = "\e[2K\r";

void
on_sigint(evutil_socket_t fd, short events, void *userdata)
{
	struct Progress *this = userdata;
	progress_set_winsize(this, this->winsize.ws_row);
	fprintf(this->out, "interrupted by user\n");
	exit(1);
}

void
on_sigwinch(evutil_socket_t fd, short events, void *userdata)
{
	struct Progress *this = userdata;
	ioctl(fileno(this->out), TIOCGWINSZ, &this->winsize);
	progress_set_winsize(this, this->winsize.ws_row - 1);
}

void
on_timeout(evutil_socket_t fd, short events, void *userdata)
{
	struct Progress *this = userdata;
	progress_step(this);
}

struct Progress *
progress_new(struct event_base *base, FILE *out)
{
	struct Progress *this = xmalloc(sizeof(struct Progress));
	this->base = base;
	this->current_file = str_dup(NULL, "");
	this->out = out;
	if (isatty(fileno(this->out))) {
		this->progressbar = true;
	}
	this->timeout = event_new(base, 0, EV_PERSIST, on_timeout, this);
	struct timeval time;
	time.tv_sec = 1;
	time.tv_usec = 0;
	evtimer_add(this->timeout, &time);

	this->sigint_event = evsignal_new(base, SIGINT, on_sigint, this);
	evsignal_add(this->sigint_event, NULL);
	this->sigwinch_event = evsignal_new(base, SIGWINCH, on_sigwinch, this);
	evsignal_add(this->sigwinch_event, NULL);

	return this;
}

void
progress_free(struct Progress *this)
{
	progress_stop(this);
	progress_set_winsize(this, this->winsize.ws_row);
	free(this->current_file);
	event_free(this->timeout);
	event_free(this->sigint_event);
	event_free(this->sigwinch_event);
	free(this);
}

void
progress_update(struct Progress *this, off_t delta, const char *current_file)
{
	this->current_bytes += delta;
	if (this->current_bytes < 0) {
		this->current_bytes = 0;
	}
	if (current_file) {
		free(this->current_file);
		this->current_file = str_dup(NULL, current_file);
	}
}

void
progress_update_total(struct Progress *this, off_t delta)
{
	this->total_bytes += delta;
	if (this->total_bytes < 0) {
		this->total_bytes = 0;
	}
}

void
progress_go_to_bar_row(struct Progress *this)
{
	fprintf(this->out, "\e[%d;0H", this->winsize.ws_row + 1);
}

void
progress_set_winsize(struct Progress *this, unsigned short row)
{
	fputs("\n", this->out);
	fputs(cursor_save, this->out);
	// set Scrolling Region
	fprintf(this->out, "\e[0;%dr", row);
	fputs(cursor_restore, this->out);
	fputs(cursor_up, this->out);
	fputs(erase_below, this->out);
	fflush(this->out);
}

void
progress_step(struct Progress *this)
{
	int progress = 0;
	if (this->total_bytes > 0) {
		// in makesum mode total_bytes is an estimation
		// and CURLOPT_MAXFILESIZE_LARGE is not set
		// either, so this might go over 100%
		progress = MIN(100, 100.0 * this->current_bytes / this->total_bytes);
	}

	unless (this->initialized) {
		if (this->progressbar) {
			ioctl(fileno(this->out), TIOCGWINSZ, &this->winsize);
			progress_set_winsize(this, this->winsize.ws_row - 1);
		}
		this->initialized = true;
	}

	if (this->progressbar && this->winsize.ws_col <= (PROGRESS_BAR_WIDTH + strlen("[100%] [] "))) {
		if (this->winsize.ws_col >= 4) {
			fputs(cursor_save, this->out);
			progress_go_to_bar_row(this);
			fputs(erase_line_all, this->out);
			fprintf(this->out, "%3d%%", progress);
			fputs(cursor_restore, this->out);
			fflush(this->out);
		}
		return;
	}

	SCOPE_MEMPOOL(pool);
	int full = PROGRESS_BAR_WIDTH * progress / 100;
	char *bar = mempool_alloc(pool, PROGRESS_BAR_WIDTH + 1);
	memset(bar, ' ', PROGRESS_BAR_WIDTH);
	memset(bar, '=', full);
	if (full > 0) {
		bar[full - 1] = '>';
	}

	ssize_t filename_width = this->winsize.ws_col - strlen("[100%] [] ") - PROGRESS_BAR_WIDTH;
	const char *filename = this->current_file;
	if (filename_width > 0) {
		filename = str_slice(pool, this->current_file, 0, filename_width);
	}
	if (this->progressbar) {
		fputs(cursor_save, this->out);
		progress_go_to_bar_row(this);
		fputs(erase_line_all, this->out);
		fprintf(this->out, "%3d%% [%s] ", progress, bar);
		fputs(filename, this->out);
		fputs(cursor_restore, this->out);
	} else {
		fprintf(this->out, "%3d%% [%s] %s\n", progress, bar, filename);
	}
	fflush(this->out);
}

void
progress_stop(struct Progress *this)
{
	event_del(this->timeout);
	event_del(this->sigint_event);
	event_del(this->sigwinch_event);
}
