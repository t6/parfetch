#pragma once

struct Progress;
struct Array;
struct event_base;

struct Progress *progress_new(struct event_base *, FILE *);
void progress_free(struct Progress *);
void progress_update(struct Progress *, off_t, const char *);
void progress_update_total(struct Progress *, off_t);
void progress_stop(struct Progress *);
