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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

struct dentry {
	size_t			index;
	char			*path;
	LIST_ENTRY(dentry)	list;
};

union cp {
	const char		*cp;
	char			*p;
};

LIST_HEAD(dlist, dentry);

static void	dirlist_display(struct cebuf *, const char *);

void
ce_dirlist_path(struct cebuf *buf, const char *path)
{
	dirlist_display(buf, path);
}

const char *
ce_dirlist_select(struct cebuf *buf, size_t index)
{
	struct dlist		*list;
	struct dentry		*entry;

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	list = buf->intdata;

	LIST_FOREACH(entry, list, list) {
		if (entry->index == index)
			return (entry->path);
	}

	return (NULL);
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

	while ((entry = LIST_FIRST(list)) != NULL) {
		LIST_REMOVE(entry, list);
		free(entry->path);
		free(entry);
	}

	free(list);
}

static void
dirlist_display(struct cebuf *buf, const char *path)
{
	int			i;
	FTS			*fts;
	FTSENT			*ent;
	size_t			index;
	struct dlist		*list;
	struct dentry		*entry;
	union cp		cp = { .cp = path };
	char			type, *pathv[] = { cp.p, NULL };

	if (buf->intdata != NULL)
		fatal("%s: intdata for '%s' not NULL", __func__, buf->name);

	fts = fts_open(pathv, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (fts == NULL) {
		ce_editor_message("cannot read '%s': %s", path, errno_s);
		return;
	}

	if ((list = calloc(1, sizeof(*list))) == NULL)
		fatal("%s: calloc failed while allocating list", __func__);

	index = 0;
	LIST_INIT(list);

	buf->intdata = list;
	buf->flags |= CE_BUFFER_RO;

	ce_buffer_reset(buf);
	ce_buffer_setname(buf, "<directory list>");

	while ((ent = fts_read(fts)) != NULL) {
		switch (ent->fts_info) {
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
			type = 'd';
			break;
		default:
			type = '?';
			break;
		}

		for (i = 1; i < ent->fts_level; i++)
			ce_buffer_appendf(buf, "  ");

		ce_buffer_appendf(buf, "[%c] %s (%llu KB) (%d)\n", type,
		    ent->fts_name, ent->fts_statp->st_size / 1024,
		    ent->fts_info);

		if ((entry = calloc(1, sizeof(*entry))) == NULL) {
			fatal("%s: calloc failued while allocating entry",
			    __func__);
		}

		entry->index = index++;

		if ((entry->path = strdup(ent->fts_accpath)) == NULL) {
			fatal("%s: strdup failed on '%s'", __func__,
			    ent->fts_accpath);
		}

		LIST_INSERT_HEAD(list, entry, list);
	}

	fts_close(fts);
	ce_buffer_populate_lines(buf);

	ce_editor_message("loaded directory '%s'", path);
}
