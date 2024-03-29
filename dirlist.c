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
#include <sys/stat.h>

#include <fts.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

#define DENTRY_FLAG_HIDDEN	(1 << 1)

struct dentry {
	char			*path;
	const char		*vpath;
	u_int16_t		flags;
	mode_t			mode;
	mode_t			vmode;
};

struct dlist {
	size_t			nelm;
	char			*path;
	struct dentry		*entries;
};

union cp {
	const char		*cp;
	char			*p;
};

static void	dirlist_load(struct cebuf *, const char *);
static void	dirlist_tobuf(struct cebuf *, const char *);
static int	dirlist_cmp(const FTSENT **, const FTSENT **);

static const char *ignored[] = {
	"*.git*",
	"*.svn*",
	"*.o",
	NULL,
};

void
ce_dirlist_path(struct cebuf *buf, const char *path)
{
	struct dlist		*list;

	if (buf->intdata != NULL)
		fatal("%s: intdata for '%s' not NULL", __func__, buf->name);

	if ((list = calloc(1, sizeof(*list))) == NULL)
		fatal("%s: calloc failed while allocating list", __func__);

	list->path = ce_strdup(path);

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
	size_t			idx;
	struct dlist		*list;
	struct dentry		*entry;

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	list = buf->intdata;
	buf->intdata = NULL;

	for (idx = 0; idx < list->nelm; idx++) {
		entry = &list->entries[idx];
		free(entry->path);
	}

	free(list->entries);
	free(list->path);
	free(list);
}

void
ce_dirlist_narrow(struct cebuf *buf, const char *pattern)
{
	dirlist_tobuf(buf, pattern);
}

void
ce_dirlist_rmfile(const void *arg)
{
	struct stat		st;
	const char		*fp = arg;

	if (stat(fp, &st) == -1) {
		ce_editor_message("stat(%s): %s", fp, errno_s);
		return;
	}

	if (!S_ISREG(st.st_mode)) {
		ce_editor_message("refusing to delete directory %s", fp);
		return;
	}

	if (unlink(fp) == -1) {
		ce_editor_message("unlink(%s): %s", fp, errno_s);
		return;
	}
}

const char *
ce_dirlist_index2path(struct cebuf *buf, size_t index)
{
	int		len;
	struct dlist	*list;
	char		fpath[PATH_MAX];

	list = buf->intdata;

	if (index >= list->nelm)
		return (0);

	len = snprintf(fpath, sizeof(fpath), "%s/%s",
	    buf->path, list->entries[index].vpath);
	if (len == -1 || (size_t)len >= sizeof(fpath)) {
		fatal("%s: failed to construct %s/%s",
		    __func__, buf->path, list->entries[index].vpath);
	}

	return (ce_editor_fullpath(fpath));
}

mode_t
ce_dirlist_index2mode(struct cebuf *buf, size_t index)
{
	struct dlist	*list;

	list = buf->intdata;

	if (index >= list->nelm)
		return (0);

	return (list->entries[index].vmode);
}

static void
dirlist_load(struct cebuf *buf, const char *path)
{
	FTS			*fts;
	FTSENT			*ent;
	const char		*name;
	struct dlist		*list;
	union cp		cp = { .cp = path };
	size_t			i, rootlen, cnt, len;
	char			*pathv[] = { cp.p, NULL };

	if (buf->intdata == NULL)
		fatal("%s: no dirlist attached to '%s'", __func__, buf->name);

	fts = fts_open(pathv,
	    FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, dirlist_cmp);
	if (fts == NULL) {
		ce_editor_message("cannot read '%s': %s", path, errno_s);
		return;
	}

	cnt = 0;
	list = buf->intdata;
	rootlen = strlen(path) + 1;

	while ((ent = fts_read(fts)) != NULL) {
		if (!strcmp(ent->fts_accpath, path))
			continue;
		if (S_ISDIR(ent->fts_statp->st_mode))
			continue;

		name = ent->fts_path + rootlen;

		for (i = 0; ignored[i] != NULL; i++) {
			if (fnmatch(ignored[i],
			    name, FNM_NOESCAPE | FNM_CASEFOLD) == 0)
				break;
		}

		if (ignored[i] != NULL)
			continue;

		if (cnt >= list->nelm) {
			len = (list->nelm + 64) * sizeof(struct dentry);
			list->entries = realloc(list->entries, len);
			if (list->entries == NULL)
				fatal("%s: calloc (%zu)", __func__, len);

			list->nelm += 64;
		}

		list->entries[cnt].path = ce_strdup(name);
		list->entries[cnt].mode = ent->fts_statp->st_mode;

		list->entries[cnt].vpath = list->entries[cnt].path;
		list->entries[cnt].vmode = list->entries[cnt].mode;

		cnt++;
	}

	list->nelm = cnt;
	fts_close(fts);

	ce_editor_message("loaded directory '%s'", path);
}

static void
dirlist_tobuf(struct cebuf *buf, const char *match)
{
	int			len;
	struct dlist		*list;
	struct dentry		*entry;
	size_t			idx, line;
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

	ce_buffer_erase(buf);
	ce_buffer_setname(buf, title);

	list = buf->intdata;

	len = snprintf(title, sizeof(title), "Directory listing for '%s'\n",
	    list->path);
	if (len == -1 || (size_t)len >= sizeof(title))
		fatal("%s: failed to construct title", __func__);

	ce_buffer_appendl(buf, title, len);

	if (match) {
		len = snprintf(title, sizeof(title), "filter '%s'\n", match);
		if (len == -1 || (size_t)len >= sizeof(title))
			fatal("%s: failed to construct title", __func__);

		ce_buffer_appendl(buf, title, len);
		ce_buffer_appendl(buf, "\n", 1);
	} else {
		ce_buffer_appendl(buf, "\n", 1);
		ce_buffer_appendl(buf, "\n", 1);
	}

	line = 0;

	for (idx = 0; idx < list->nelm; idx++) {
		entry = &list->entries[idx];

		if (match) {
			if (fnmatch(pattern,
			    entry->path, FNM_NOESCAPE | FNM_CASEFOLD) == 0)
				entry->flags &= ~DENTRY_FLAG_HIDDEN;
			else
				entry->flags = DENTRY_FLAG_HIDDEN;
		} else {
			entry->flags &= ~DENTRY_FLAG_HIDDEN;
		}

		if (entry->flags & DENTRY_FLAG_HIDDEN)
			continue;

		len = snprintf(title, sizeof(title), "%o %s\n",
		    entry->mode, entry->path);
		if (len == -1 || (size_t)len >= sizeof(title))
			fatal("%s: snprintf failed", __func__);

		ce_buffer_appendl(buf, title, len);

		list->entries[line].vpath = entry->path;
		list->entries[line].vmode = entry->mode;

		line++;
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
