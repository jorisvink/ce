/*
 * Copyright (c) 2021 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <fts.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

#define DENTRY_FLAG_VISIBLE	0x0001

struct dentry {
	u_int16_t		flags;
	u_int16_t		info;
	int			level;
	off_t			size;
	size_t			index;
	char			*path;
	char			*name;
	TAILQ_ENTRY(dentry)	list;
};

union cp {
	const char		*cp;
	char			*p;
};

TAILQ_HEAD(dlist, dentry);

static void	dirlist_load(struct cebuf *, const char *);
static void	dirlist_tobuf(struct cebuf *, const char *);
static int	dirlist_cmp(const FTSENT **, const FTSENT **);

void
ce_dirlist_path(struct cebuf *buf, const char *path)
{
	struct dlist		*list;

	if (buf->intdata != NULL)
		fatal("%s: intdata for '%s' not NULL", __func__, buf->name);

	if ((list = calloc(1, sizeof(*list))) == NULL)
		fatal("%s: calloc failed while allocating list", __func__);

	TAILQ_INIT(list);

	buf->intdata = list;
	buf->flags |= CE_BUFFER_RO;

	dirlist_load(buf, path);
	dirlist_tobuf(buf, NULL);
}

const char *
ce_dirlist_select(struct cebuf *buf, size_t index)
{
	struct dlist		*list;
	struct dentry		*entry;

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	list = buf->intdata;

	TAILQ_FOREACH(entry, list, list) {
		if (!(entry->flags & DENTRY_FLAG_VISIBLE))
			continue;

		if (entry->index == index)
			return (entry->path);
	}

	return (NULL);
}

void
ce_dirlist_rescan(struct cebuf *buf)
{
	ce_dirlist_close(buf);
	ce_dirlist_path(buf, buf->path);
}

void
ce_dirlist_close(struct cebuf *buf)
{
	struct dlist		*list;
	struct dentry		*entry;

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	list = buf->intdata;
	buf->intdata = NULL;

	while ((entry = TAILQ_FIRST(list)) != NULL) {
		TAILQ_REMOVE(list, entry, list);
		free(entry->path);
		free(entry->name);
		free(entry);
	}

	free(list);
}

void
ce_dirlist_narrow(struct cebuf *buf, const char *pattern)
{
	dirlist_tobuf(buf, pattern);
}

static void
dirlist_load(struct cebuf *buf, const char *path)
{
	FTS			*fts;
	FTSENT			*ent;
	struct dlist		*list;
	struct dentry		*entry;
	union cp		cp = { .cp = path };
	char			*pathv[] = { cp.p, NULL };

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	fts = fts_open(pathv,
	    FTS_NOCHDIR | FTS_PHYSICAL | FTS_SEEDOT | FTS_XDEV, dirlist_cmp);
	if (fts == NULL) {
		ce_editor_message("cannot read '%s': %s", path, errno_s);
		return;
	}

	list = buf->intdata;

	while ((ent = fts_read(fts)) != NULL) {
		if ((entry = calloc(1, sizeof(*entry))) == NULL) {
			fatal("%s: calloc failued while allocating entry",
			    __func__);
		}

		entry->info = ent->fts_info;
		entry->level = ent->fts_level;
		entry->size = ent->fts_statp->st_size;

		if ((entry->path = strdup(ent->fts_accpath)) == NULL) {
			fatal("%s: strdup failed on '%s'", __func__,
			    ent->fts_accpath);
		}

		if ((entry->name = strdup(ent->fts_name)) == NULL) {
			fatal("%s: strdup failed on '%s'", __func__,
			    ent->fts_name);
		}

		TAILQ_INSERT_TAIL(list, entry, list);
	}

	fts_close(fts);

	ce_editor_message("loaded directory '%s'", path);
}

static void
dirlist_tobuf(struct cebuf *buf, const char *match)
{
	const char		*d;
	off_t			sz;
	char			type;
	size_t			index;
	struct dlist		*list;
	struct dentry		*entry;
	int			i, len, show, first;
	char			*rp, title[PATH_MAX], pattern[PATH_MAX];

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	if (match != NULL) {
		len = snprintf(pattern, sizeof(pattern), "*%s*", match);
		if (len == -1 || (size_t)len >= sizeof(pattern))
			fatal("%s: failed to construct pattern", __func__);
	}

	len = snprintf(title, sizeof(title), "<dir %s>", buf->path);
	if (len == -1 || (size_t)len >= sizeof(title))
		fatal("%s: failed to construct buf title", __func__);

	first = 1;
	index = 2;
	list = buf->intdata;

	ce_buffer_reset(buf);
	ce_buffer_setname(buf, title);

	TAILQ_FOREACH(entry, list, list) {
		if (first) {
			if ((rp = realpath(entry->path, NULL)) == NULL)
				fatal("%s: realpath: %s", __func__, errno_s);
			ce_buffer_appendf(buf,
			    "Directory listing for '%s'\n\n", rp);
			first = 0;
			free(rp);
			continue;
		}

		if (entry->level > 1 &&
		    (!strcmp(entry->name, ".") || !strcmp(entry->name, "..")))
			continue;

		switch (entry->info) {
		case FTS_DNR:
		case FTS_DP:
			continue;
		case FTS_F:
			type = 'f';
			break;
		case FTS_SL:
		case FTS_SLNONE:
			type = 'l';
			break;
		case FTS_D:
		case FTS_DOT:
			type = 'd';
			break;
		default:
			type = '?';
			break;
		}

		show = 0;

		if (match) {
			if (fnmatch(pattern,
			    entry->path, FNM_NOESCAPE | FNM_CASEFOLD) == 0)
				show = 1;
		} else {
			show = 1;
		}

		if (show == 0) {
			entry->flags = 0;
			continue;
		}

		entry->index = index++;
		entry->flags = DENTRY_FLAG_VISIBLE;

		if (match == NULL) {
			for (i = 1; i < entry->level; i++)
				ce_buffer_appendf(buf, "  ");
		}

		if (entry->size < 1024) {
			d = "B";
			sz = entry->size;
		} else {
			d = "KB";
			sz = entry->size / 1024;
		}

		ce_buffer_appendf(buf, "[%c] %s (%jd %s) (%d)\n", type,
		    entry->name, (intmax_t)sz, d, entry->info);
	}

	ce_buffer_populate_lines(buf);
	ce_editor_dirty();
}

static int
dirlist_cmp(const FTSENT **a1, const FTSENT **b1)
{
	const FTSENT	*a = *a1;
	const FTSENT	*b = *b1;

	return (strcmp(a->fts_name, b->fts_name));
}
