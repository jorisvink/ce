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

struct dentry {
	u_int16_t		info;
	int			level;
	off_t			size;
	char			*path;
	TAILQ_ENTRY(dentry)	list;
};

struct dlist {
	char			*path;
	TAILQ_HEAD(, dentry)	entries;
};

union cp {
	const char		*cp;
	char			*p;
};

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

	if ((list->path = strdup(path)) == NULL)
		fatal("%s: strdup: %s", __func__, errno_s);

	TAILQ_INIT(&list->entries);

	buf->intdata = list;
	buf->flags |= CE_BUFFER_RO;

	dirlist_load(buf, path);
	dirlist_tobuf(buf, NULL);
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

	while ((entry = TAILQ_FIRST(&list->entries)) != NULL) {
		TAILQ_REMOVE(&list->entries, entry, list);
		free(entry->path);
		free(entry);
	}

	free(list->path);
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
	const char		*p;
	FTS			*fts;
	FTSENT			*ent;
	struct dlist		*list;
	struct dentry		*entry;
	union cp		cp = { .cp = path };
	char			*pathv[] = { cp.p, NULL };

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	fts = fts_open(pathv,
	    FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, dirlist_cmp);
	if (fts == NULL) {
		ce_editor_message("cannot read '%s': %s", path, errno_s);
		return;
	}

	list = buf->intdata;

	while ((ent = fts_read(fts)) != NULL) {
		if ((entry = calloc(1, sizeof(*entry))) == NULL) {
			fatal("%s: calloc failed while allocating entry",
			    __func__);
		}

		entry->info = ent->fts_info;
		entry->level = ent->fts_level;
		entry->size = ent->fts_statp->st_size;

		p = ce_editor_shortpath(ent->fts_accpath);

		if ((entry->path = strdup(p)) == NULL) {
			fatal("%s: strdup failed on '%s'", __func__,
			    ent->fts_accpath);
		}

		TAILQ_INSERT_TAIL(&list->entries, entry, list);
	}

	fts_close(fts);

	ce_editor_message("loaded directory '%s'", path);
}

static void
dirlist_tobuf(struct cebuf *buf, const char *match)
{
	struct dlist		*list;
	struct dentry		*entry;
	int			len, show;
	char			title[PATH_MAX], pattern[PATH_MAX];

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

	list = buf->intdata;

	free(buf->lines);
	buf->lcnt = 0;
	buf->lines = NULL;

	ce_buffer_reset(buf);
	ce_buffer_setname(buf, title);

	len = snprintf(title, sizeof(title), "Directory listing for '%s'\n",
	    list->path);
	if (len == -1 || (size_t)len >= sizeof(title))
		fatal("%s: failed to construct title", __func__);

	ce_buffer_appendl(buf, title, len);
	ce_buffer_appendl(buf, "\n", 1);

	TAILQ_FOREACH(entry, &list->entries, list) {
		show = 0;

		if (match) {
			if (fnmatch(pattern,
			    entry->path, FNM_NOESCAPE | FNM_CASEFOLD) == 0)
				show = 1;
		} else {
			show = 1;
		}

		if (show == 0)
			continue;

		len = snprintf(title, sizeof(title), "%s\n", entry->path);
		if (len == -1 || (size_t)len >= sizeof(title))
			fatal("%s: snprintf failed", __func__);

		ce_buffer_appendl(buf, title, len);
	}

	ce_editor_dirty();
	ce_buffer_jump_line(buf, TERM_CURSOR_MIN, TERM_CURSOR_MIN);
}

static int
dirlist_cmp(const FTSENT **a1, const FTSENT **b1)
{
	const FTSENT	*a = *a1;
	const FTSENT	*b = *b1;

	return (strcmp(a->fts_name, b->fts_name));
}
