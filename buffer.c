/*
 * Copyright (c) 2020 Joris Vink <joris@coders.se>
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ce.h"

/* Conservative. */
#define BUFFER_MAX_IOVEC		64

#define BUFFER_SEARCH_FORWARD		1
#define BUFFER_SEARCH_REVERSE		2

static void		buffer_grow(struct cebuf *, size_t);
static void		buffer_resize_lines(struct cebuf *, size_t);
static void		buffer_next_character(struct cebuf *, struct celine *);
static void		buffer_prev_character(struct cebuf *, struct celine *);

static void		buffer_seterr(const char *, ...)
			    __attribute__((format (printf, 1, 2)));

static void		*buffer_find(struct celine *, const void *, size_t);
static void		*buffer_search(struct cebuf *, int, const void *,
			    size_t, size_t, size_t, size_t *);

static void		buffer_update_cursor(struct cebuf *);
static void		buffer_update_cursor_line(struct cebuf *);
static void		buffer_line_column_to_data(struct cebuf *);
static void		buffer_update_cursor_column(struct cebuf *);
static size_t		buffer_line_data_to_columns(const void *, size_t);
static size_t		buffer_line_span(struct cebuf *, struct celine *);
static void		buffer_line_erase_character(struct cebuf *,
			    struct celine *, int);
static void		buffer_line_insert_byte(struct cebuf *,
			    struct celine *, u_int8_t);

static struct cebuflist		buffers;
static struct cebuflist		internals;
static char			*errstr = NULL;
static struct cebuf		*active = NULL;
static struct cebuf		*scratch = NULL;
static u_int16_t		cursor_column = TERM_CURSOR_MIN;

void
ce_buffer_init(int argc, char **argv)
{
	int			i;
	char			*ep;
	struct cebuf		*last;
	long			linenr;

	last = NULL;

	TAILQ_INIT(&buffers);
	TAILQ_INIT(&internals);

	scratch = ce_buffer_internal("scratch");
	scratch->mode = 0644;
	active = scratch;
	ce_term_update_title();

	for (i = 0; i < argc; i++) {
		if (*argv[i] == '+' && last != NULL) {
			errno = 0;
			linenr = strtol(argv[i], &ep, 10);
			if (errno != 0 || *ep != '\0')
				fatal("%s is a bad line number", argv[i]);

			ce_buffer_jump_line(last, linenr, 0);
			continue;
		}

		if ((last = ce_buffer_file(argv[i])) == NULL)
			ce_editor_message("%s", ce_buffer_strerror());
	}

	if ((active = TAILQ_FIRST(&buffers)) == NULL) {
		active = scratch;
		ce_term_update_title();
		ce_editor_show_splash();
		ce_editor_settings(active);
	}
}

void
ce_buffer_cleanup(void)
{
	struct cebuf	*buf;

	while ((buf = TAILQ_FIRST(&buffers)) != NULL)
		ce_buffer_free(buf);

	while ((buf = TAILQ_FIRST(&internals)) != NULL)
		ce_buffer_free_internal(buf);
}

void
ce_buffer_close_nonactive(void)
{
	struct cebuf	*buf, *next;

	for (buf = TAILQ_FIRST(&buffers); buf != NULL; buf = next) {
		next = TAILQ_NEXT(buf, list);
		if (buf == active || buf->buftype == CE_BUF_TYPE_SHELLCMD)
			continue;
		ce_buffer_free(buf);
	}

	active->prev = scratch;
}

const char *
ce_buffer_strerror(void)
{
	return (errstr);
}

void
ce_buffer_setname(struct cebuf *buf, const char *name)
{
	free(buf->name);
	buf->name = ce_strdup(name);
}

void
ce_buffer_resize(void)
{
	struct cebuf	*buf;

	TAILQ_FOREACH(buf, &buffers, list) {
		buf->width = ce_term_width();
		buf->height = ce_term_height() - 3;
	}

	TAILQ_FOREACH(buf, &internals, list) {
		buf->width = ce_term_width();
		buf->height = ce_term_height() - 3;
	}
}

struct cebuf *
ce_buffer_internal(const char *name)
{
	struct cebuf	*buf;

	buf = ce_buffer_alloc(1);
	ce_buffer_setname(buf, name);

	buf->maxsz = 1024;
	if ((buf->data = calloc(1, buf->maxsz)) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, buf->maxsz, errno_s);

	ce_buffer_line_alloc_empty(buf);

	return (buf);
}

struct cebuf *
ce_buffer_dirlist(const char *path)
{
	char		*rp;
	struct cebuf	*buf;

	if ((rp = realpath(path, NULL)) == NULL) {
		ce_editor_message("cannot dirlist '%s': %s", path, errno_s);
		return (NULL);
	}

	TAILQ_FOREACH(buf, &buffers, list) {
		if (buf->buftype != CE_BUF_TYPE_DIRLIST)
			continue;

		if (!strcmp(buf->path, rp)) {
			active = buf;
			ce_editor_dirty();
			ce_dirlist_narrow(buf, NULL);
			return (buf);
		}
	}

	buf = ce_buffer_alloc(0);

	buf->path = rp;
	buf->flags |= CE_BUFFER_RO;
	buf->buftype = CE_BUF_TYPE_DIRLIST;

	ce_dirlist_path(buf, buf->path);
	ce_buffer_activate(buf);

	return (buf);
}

struct cebuf *
ce_buffer_file(const char *path)
{
	int			fd;
	struct stat		st;
	char			*rp;
	ssize_t			bytes;
	struct cebuf		*buf, *ret;

	fd = -1;
	ret = NULL;

	if ((rp = realpath(path, NULL)) != NULL) {
		TAILQ_FOREACH(buf, &buffers, list) {
			if (buf->buftype != CE_BUF_TYPE_DEFAULT)
				continue;
			if (!strcmp(buf->path, rp)) {
				active = buf;
				ce_editor_settings(active);
				return (buf);
			}
		}
	}

	buf = ce_buffer_alloc(0);
	ce_buffer_setname(buf, rp ? rp : path);

	buf->path = rp;
	if (buf->path == NULL) {
		if (errno != ENOENT) {
			buffer_seterr("%s: %s", path, errno_s);
			goto cleanup;
		}

		buf->path = ce_strdup(path);
		buf->mode = S_IRUSR | S_IWUSR;

		goto finalize;
	}

	if (access(buf->path, R_OK) == -1) {
		if (errno == ENOENT) {
			buf->mode = S_IRUSR | S_IWUSR;
			goto finalize;
		}

		buffer_seterr("%s: no read access: %s", path, errno_s);
		goto cleanup;
	}

	if (access(buf->path, W_OK) == -1) {
		buf->flags |= CE_BUFFER_RO;
		ce_editor_message("%s opened in read-only mode", path);
	}

	if ((fd = open(path, O_RDONLY)) == -1) {
		buffer_seterr("%s: open: %s", path, errno_s);
		goto cleanup;
	}

	if (fstat(fd, &st) == -1) {
		buffer_seterr("%s: fstat: %s", path, errno_s);
		goto cleanup;
	}

	if (!S_ISREG(st.st_mode)) {
		buffer_seterr("%s: not a file", path);
		goto cleanup;
	}

	if ((uintmax_t)st.st_size > CE_MAX_FILE_SIZE) {
		buffer_seterr("%s: too large (> %u bytes)",
		    path, CE_MAX_FILE_SIZE);
		goto cleanup;
	}

	buf->maxsz = (size_t)st.st_size;
	buf->length = buf->maxsz;
	buf->mode = st.st_mode;
	buf->mtime = st.st_mtime;

	if (buf->maxsz > 0) {
		if ((buf->data = calloc(1, buf->maxsz)) == NULL) {
			fatal("%s: calloc(%zu): %s", __func__, buf->maxsz,
			    errno_s);
		}

		if ((bytes = read(fd, buf->data, buf->maxsz)) == -1) {
			buffer_seterr("%s: read failed: %s", __func__, errno_s);
			goto cleanup;
		}

		if ((size_t)bytes != buf->maxsz) {
			buffer_seterr("%s: only read %zd/%zu bytes",
			    __func__, bytes, buf->maxsz);
			goto cleanup;
		}
	}

finalize:
	ce_file_type_detect(buf);
	ce_buffer_populate_lines(buf);

	ret = buf;
	ce_buffer_activate(buf);

	buf = NULL;

cleanup:
	if (fd != -1)
		(void)close(fd);

	if (buf != NULL)
		ce_buffer_free(buf);

	return (ret);
}

struct cebuf *
ce_buffer_active(void)
{
	return (active);
}

struct cebuf *
ce_buffer_first_dirty(void)
{
	struct cebuf		*buf;

	TAILQ_FOREACH(buf, &buffers, list) {
		if (buf->flags & CE_BUFFER_DIRTY)
			return (buf);
	}

	return (NULL);
}

void
ce_buffer_free(struct cebuf *buf)
{
	size_t			idx;
	struct cebuf		*bp;
	struct celine		*line;

	if (buf->internal)
		return;

	if (buf->buftype == CE_BUF_TYPE_DIRLIST)
		ce_dirlist_close(buf);

	TAILQ_REMOVE(&buffers, buf, list);

	if (active == buf) {
		active = buf->prev;
		ce_editor_settings(active);
	}

	TAILQ_FOREACH(bp, &buffers, list) {
		if (bp->prev == buf)
			bp->prev = active;
	}

	free(buf->data);

	for (idx = 0; idx < buf->lcnt; idx++) {
		line = &buf->lines[idx];
		if (line->flags & CE_LINE_ALLOCATED)
			free(line->data);
	}

	free(buf->lines);
	free(buf->path);
	free(buf->name);
	free(buf);
}

void
ce_buffer_free_internal(struct cebuf *buf)
{
	size_t			idx;
	struct celine		*line;

	if (buf->internal == 0) {
		fatal("%s: called on non internal buffer '%s'",
		    __func__, buf->name);
	}

	TAILQ_REMOVE(&internals, buf, list);

	if (active == buf) {
		active = buf->prev;
		ce_editor_settings(active);
	}

	for (idx = 0; idx < buf->lcnt; idx++) {
		line = &buf->lines[idx];
		if (line->flags & CE_LINE_ALLOCATED)
			free(line->data);
	}

	free(buf->lines);
	free(buf->data);
	free(buf->path);
	free(buf->name);
	free(buf);
}

void
ce_buffer_reset(struct cebuf *buf)
{
	buf->top = 0;
	buf->length = 0;
	buf->line = buf->orig_line;
	buf->column = buf->orig_column;
	buf->cursor_line = buf->orig_line;
}

void
ce_buffer_restore(void)
{
	if (active->prev == NULL)
		return;

	active = active->prev;

	ce_term_update_title();
	ce_editor_dirty();
	ce_editor_settings(active);
}

void
ce_buffer_activate(struct cebuf *buf)
{
	if (active == buf)
		return;

	buf->prev = active;
	active = buf;

	ce_editor_dirty();
	ce_term_update_title();
	ce_editor_settings(active);
}

void
ce_buffer_activate_index(size_t index)
{
	size_t			idx;
	struct cebuf		*buf;

	if (index == 0) {
		active = scratch;
		ce_term_update_title();
		ce_editor_dirty();
		ce_editor_settings(active);
		return;
	}

	idx = 1;

	TAILQ_FOREACH_REVERSE(buf, &buffers, cebuflist, list) {
		if (idx++ == index) {
			active = buf;
			ce_term_update_title();
			ce_editor_dirty();
			ce_editor_settings(active);
			return;
		}
	}

	ce_editor_dirty();
}

const char *
ce_buffer_as_string(struct cebuf *buf)
{
	const char	null = '\0';

	ce_buffer_append(buf, &null, sizeof(null));

	return (buf->data);
}

void
ce_buffer_map(struct cebuf *buf)
{
	size_t		idx, line, towrite;

	ce_syntax_init();

	line = buf->orig_line;
	ce_term_setpos(buf->orig_line, buf->orig_column);

	for (idx = buf->top; idx < buf->lcnt; idx++) {
		ce_term_setpos(line, TERM_CURSOR_MIN);

		towrite = (buf->height- (line - 1)) * buf->width;
		if (towrite > buf->lines[idx].length)
			towrite = buf->lines[idx].length;

		ce_syntax_write(buf, &buf->lines[idx], idx, towrite);
		line += buffer_line_span(buf, &buf->lines[idx]);

		if (line > buf->height)
			break;
	}

	ce_syntax_finalize();

	ce_term_writestr(TERM_SEQUENCE_ATTR_BOLD);
	ce_term_color(TERM_COLOR_BLUE + TERM_COLOR_FG);

	while (line <= buf->height)  {
		ce_term_setpos(line, TERM_CURSOR_MIN);
		ce_term_writestr("~");
		line++;
	}

	ce_term_attr_off();
	ce_term_setpos(buf->cursor_line, buf->column);
}

int
ce_buffer_word_cursor(struct cebuf *buf, const u_int8_t **word, size_t *len)
{
	const u_int8_t		*ptr;
	struct celine		*line;
	size_t			end, start;

	if (buf->lcnt == 0)
		return (-1);

	line = ce_buffer_line_current(buf);

	ptr = line->data;

	for (start = buf->loff; start > 0; start--) {
		if (ce_editor_word_byte(ptr[start]) == 0)
			break;
	}

	if (ce_editor_word_byte(ptr[start]) == 0)
		start++;

	for (end = buf->loff; end < line->length - 1; end++) {
		if (ce_editor_word_byte(ptr[end]) == 0)
			break;
	}

	if (start > end)
		return (-1);

	*len = end - start;
	*word = &ptr[start];

	return (0);
}

void
ce_buffer_word_next(struct cebuf *buf)
{
	int			skip;
	const u_int8_t		*ptr;
	struct celine		*line;

	if (buf->lcnt == 0)
		return;

	line = ce_buffer_line_current(buf);
	ptr = line->data;

	if (buf->loff == line->length - 1)
		return;

	skip = ce_editor_word_separator(ptr[buf->loff]);
	buffer_next_character(buf, line);

	if (skip && ce_editor_word_byte(ptr[buf->loff]))
		goto update;

	while (buf->loff < line->length - 1 &&
	    ce_editor_word_byte(ptr[buf->loff]))
		buffer_next_character(buf, line);

	while (buf->loff < line->length - 1 && isspace(ptr[buf->loff]))
		buffer_next_character(buf, line);

update:
	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	cursor_column = buf->column;
	ce_buffer_constrain_cursor_column(buf);

	ce_term_setpos(buf->cursor_line, buf->column);
}

void
ce_buffer_word_prev(struct cebuf *buf)
{
	int			skip;
	const u_int8_t		*ptr;
	struct celine		*line;

	if (buf->lcnt == 0 || buf->loff == 0)
		return;

	line = ce_buffer_line_current(buf);
	ptr = line->data;

	buffer_prev_character(buf, line);
	skip = ce_editor_word_separator(ptr[buf->loff]);

	if (skip && !isspace(ptr[buf->loff]))
		goto update;

	while (buf->loff > 0 && isspace(ptr[buf->loff]))
		buffer_prev_character(buf, line);

	while (buf->loff > 0 && ce_editor_word_byte(ptr[buf->loff - 1]))
		buffer_prev_character(buf, line);

update:
	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	cursor_column = buf->column;

	ce_term_setpos(buf->cursor_line, buf->column);
}

void
ce_buffer_word_delete(struct cebuf *buf)
{
	u_int8_t		*ptr;
	struct celine		*line;
	size_t			start;

	if (buf->lcnt == 0)
		return;

	line = ce_buffer_line_current(buf);
	ce_buffer_mark_last(buf, ce_buffer_line_index(buf) + 1);

	start = buf->loff;
	ce_buffer_word_next(buf);

	ce_buffer_line_allocate(buf, line);
	ptr = line->data;

	if (buf->loff + 1 == line->length - 1)
		buf->loff++;

	ce_editor_pbuffer_append(&ptr[start], buf->loff - start);
	memmove(&ptr[start], &ptr[buf->loff], line->length - buf->loff);

	line->length -= buf->loff - start;
	buf->loff = start;

	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	ce_buffer_line_columns(line);
	ce_buffer_constrain_cursor_column(buf);
	cursor_column = buf->column;
	ce_term_setpos(buf->cursor_line, buf->column);

	ce_editor_dirty();
	buf->flags |= CE_BUFFER_DIRTY;
}

void
ce_buffer_word_erase(struct cebuf *buf)
{
	u_int8_t		*ptr;
	struct celine		*line;
	size_t			start, idx, orig, chars;

	if (buf->lcnt == 0 || buf->loff == 0)
		return;

	line = ce_buffer_line_current(buf);
	ce_buffer_mark_last(buf, ce_buffer_line_index(buf) + 1);

	ptr = line->data;
	orig = buf->loff;

	if (buf->loff > 0 && isspace(ptr[buf->loff - 1]))
		buffer_prev_character(buf, line);

	if (ce_editor_word_separator(ptr[buf->loff]) && buf->loff > 0)
		buffer_prev_character(buf, line);

	while ((ptr[buf->loff] == '\n' ||
	    isspace(ptr[buf->loff])) && buf->loff > 0)
		buffer_prev_character(buf, line);

	while (buf->loff > 0) {
		if (ce_editor_word_byte(ptr[buf->loff]) == 0)
			break;
		buf->loff--;
	}

	if (buf->loff != 0 ||
	    (ce_editor_word_separator(ptr[buf->loff]) && ptr[buf->loff] != ' '))
		buf->loff++;

	chars = 0;
	start = buf->loff;

	while ((isspace(ptr[buf->loff]) ||
	    ce_editor_word_byte(ptr[buf->loff])) && buf->loff < orig) {
		chars++;
		buffer_next_character(buf, line);
	}

	ce_editor_pbuffer_reset();
	ce_buffer_line_allocate(buf, line);

	buf->loff = start;

	ptr = line->data;
	for (idx = 0; idx < chars; idx++)
		buffer_line_erase_character(buf, line, 1);

	ptr = line->data;
	if (buf->loff == 0 && ptr[0] == ' ')
		buffer_line_erase_character(buf, line, 1);

	ce_editor_pbuffer_sync();

	buf->loff = start;
	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	cursor_column = buf->column;

	ce_term_setpos(buf->cursor_line, buf->column);
}

int
ce_buffer_search(struct cebuf *buf, const char *needle, int which)
{
	const u_int8_t		*p;
	int			dir;
	struct celine		*line;
	size_t			start[2], end[2], index, len, half;

	p = NULL;
	len = strlen(needle);
	dir = BUFFER_SEARCH_FORWARD;

	if (buf->lcnt == 0)
		return (0);

	index = ce_buffer_line_index(buf);

	switch (which) {
	case CE_BUFFER_SEARCH_NEXT:
		/* search 1, from next line until end. */
		start[0] = index + 1;
		end[0] = buf->lcnt;

		/* search 2, from start until next line. */
		start[1] = 1;
		end[1] = index + 1;
		dir = BUFFER_SEARCH_FORWARD;
		break;
	case CE_BUFFER_SEARCH_NORMAL:
		/* search 1, from current line until end. */
		start[0] = index;
		end[0] = buf->lcnt;

		/* search 2, from start until current line, */
		start[1] = 1;
		end[1] = index;
		dir = BUFFER_SEARCH_FORWARD;
		break;
	case CE_BUFFER_SEARCH_PREVIOUS:
		/* search 1, from previous line until start. */
		if (index > 0) {
			start[0] = index - 1;
		} else {
			start[0] = 0;
		}

		end[0] = 0;

		/* search 2, from end until current line. */
		if (buf->lcnt > 0)
			start[1] = buf->lcnt - 1;
		else
			start[1] = index;

		end[1] = index;
		dir = BUFFER_SEARCH_REVERSE;
		break;
	default:
		fatal("%s: unknown which %d", __func__, which);
	}

	p = buffer_search(buf, dir, needle, len, start[0], end[0], &index);
	if (p == NULL) {
		p = buffer_search(buf, dir, needle, len,
		    start[1], end[1], &index);
	}

	if (p == NULL)
		return (0);

	line = &buf->lines[index];
	half = buf->height / 2;

	if (index <= half) {
		buf->top = 0;
		buf->line = index + 1;
	} else {
		buf->top = index - half;
		buf->line = half + 1;
	}

	buf->loff = p - (const u_int8_t *)line->data;
	buffer_update_cursor_line(buf);

	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	cursor_column = buf->column;

	ce_term_setpos(buf->cursor_line, buf->column);

	return (1);
}

void
ce_buffer_list(struct cebuf *output)
{
	size_t		idx;
	struct cebuf	*buf;
	const char	*name;

	idx = 1;
	output->flags |= CE_BUFFER_RO;

	ce_buffer_reset(output);

	/* Add scratch as the first buffer. */
	ce_buffer_appendf(output, "[0] [scratch] (%zu lines)\n", scratch->lcnt);
	if (scratch == active) {
		output->line = idx;
		output->cursor_line = idx;
	}

	idx++;

	TAILQ_FOREACH_REVERSE(buf, &buffers, cebuflist, list) {
		if (buf == active) {
			output->line = idx;
			output->cursor_line = idx;
		}

		if (buf->internal || buf->buftype != CE_BUF_TYPE_DEFAULT)
			name = buf->name;
		else
			name = ce_editor_shortpath(buf->path);

		ce_buffer_appendf(output, "[%zd] [%s%s] (%zu lines)\n", idx - 1,
		    name, (buf->flags & CE_BUFFER_DIRTY) ? "*" : "",
		    buf->lcnt);
		idx++;
	}

	ce_buffer_populate_lines(output);
}

void
ce_buffer_input(struct cebuf *buf, u_int8_t byte)
{
	int			i;
	struct celine		*line;

	if (buf->cb != NULL) {
		buf->cb(buf, byte);
		return;
	}

	if (buf->lcnt == 0)
		ce_buffer_line_alloc_empty(buf);

	line = ce_buffer_line_current(buf);
	ce_buffer_line_allocate(buf, line);

	switch (byte) {
	case '\b':
	case 0x7f:
		if (buf->loff > 0)
			buffer_line_erase_character(buf, line, 0);
		break;
	case '\n':
		buffer_line_insert_byte(buf, line, byte);
		ce_buffer_insert_line(buf);
		break;
	case '\t':
		if (config.tab_expand) {
			for (i = 0; i < config.tab_width; i++)
				buffer_line_insert_byte(buf, line, ' ');
		} else {
			buffer_line_insert_byte(buf, line, byte);
		}
		break;
	default:
		buffer_line_insert_byte(buf, line, byte);
		break;
	}
}

void
ce_buffer_insert_line(struct cebuf *buf)
{
	struct celine	*line;
	u_int8_t	*data, *ptr;
	size_t		index, lcnt, length;

	index = ce_buffer_line_index(buf);
	line = &buf->lines[index];

	length = line->length - buf->loff;

	data = line->data;
	data += buf->loff;

	if ((ptr = calloc(1, length)) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, length, errno_s);

	memcpy(ptr, data, length);
	line->length = buf->loff;
	line->columns = buffer_line_data_to_columns(line->data, line->length);

	lcnt = buf->lcnt;
	buffer_resize_lines(buf, buf->lcnt + 1);
	memmove(&buf->lines[index + 1], &buf->lines[index],
	    (lcnt - index) * sizeof(struct celine));

	index++;
	line = &buf->lines[index];

	line->data = ptr;
	line->maxsz = length;
	line->length = length;
	line->flags = CE_LINE_ALLOCATED;
	line->columns = buffer_line_data_to_columns(line->data, line->length);

	cursor_column = TERM_CURSOR_MIN;
	ce_buffer_move_down();

	buf->flags |= CE_BUFFER_DIRTY;
}

void
ce_buffer_delete_line(struct cebuf *buf, int isjoin)
{
	size_t			index;

	if (buf->lcnt == 0)
		return;

	if (!isjoin)
		ce_editor_pbuffer_reset();

	index = ce_buffer_line_index(buf);
	ce_buffer_delete_lines(buf, index, index, 0, isjoin);

	if (!isjoin)
		ce_editor_pbuffer_sync();
}

void
ce_buffer_delete_lines(struct cebuf *buf, size_t start, size_t end, int rev,
    int isjoin)
{
	struct celine		*line;
	size_t			index, range;

	if (start > end)
		fatal("%s: start(%zu) > end(%zu)", __func__, start, end);

	if (buf->lcnt == 0 || end >= buf->lcnt)
		return;

	range = (end - start) + 1;
	for (index = start; index <= end; index++) {
		line = &buf->lines[index];
		if (!isjoin)
			ce_editor_pbuffer_append(line->data, line->length);

		if (line->flags & CE_LINE_ALLOCATED) {
			free(line->data);
			line->data = NULL;
		}
	}

	if (end < buf->lcnt - 1) {
		memmove(&buf->lines[start], &buf->lines[end + 1],
		    (buf->lcnt - end - 1) * sizeof(struct celine));
	}

	buf->lcnt -= range;

	if (rev == 0) {
		if (buf->top > range) {
			buf->top -= range - 1;
		} else {
			buf->top = 0;

			if (buf->line > range)
				buf->line -= range - 1;
			else
				buf->line = TERM_CURSOR_MIN;
		}
	}

	if (end >= buf->lcnt)
		buf->line--;

	if (buf->line == 0) {
		if (buf->top > 0)
			buf->top--;
		buf->line = TERM_CURSOR_MIN;
	}

	if (buf->lcnt == 0) {
		cursor_column = TERM_CURSOR_MIN;
		buf->column = TERM_CURSOR_MIN;
		buf->loff = 0;
		ce_buffer_line_alloc_empty(buf);
	} else {
		line = ce_buffer_line_current(buf);
		ce_buffer_line_allocate(buf, line);
	}

	buf->flags |= CE_BUFFER_DIRTY;
	buffer_update_cursor(buf);
	ce_term_setpos(buf->cursor_line, buf->column);

	ce_editor_dirty();

	ce_editor_message("deleted %zu line(s)", range);
}

void
ce_buffer_delete_inside_string(struct cebuf *buf, u_int8_t byte)
{
	ssize_t			idx;
	u_int8_t		*ptr;
	struct celine		*line;
	size_t			start, end;

	if (buf->lcnt == 0)
		return;

	line = ce_buffer_line_current(buf);
	ptr = line->data;

	for (idx = buf->loff; idx >= 0; idx--) {
		if (ptr[idx] == byte)
			break;
	}

	if (idx < 0)
		return;

	start = (size_t)idx + 1;

	for (idx = buf->loff; (size_t)idx < line->length; idx++) {
		if (ptr[idx] == byte)
			break;
	}

	if (idx < 0)
		return;

	end = (size_t)idx;

	if (start == end || start > end || ((end - 1) == start))
		return;

	ce_buffer_line_allocate(buf, line);
	ptr = line->data;
	memmove(&ptr[start], &ptr[end], line->length - end);

	line->length -= end - start;

	buf->loff = start;
	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	cursor_column = buf->column;

	ce_buffer_constrain_cursor_column(buf);
	buffer_update_cursor(buf);

	ce_editor_dirty();
}

void
ce_buffer_jump_line(struct cebuf *buf, long linenr, size_t column)
{
	size_t		line;

	if (linenr < 0)
		fatal("%s: linenr %ld < 0", __func__, linenr);

	if (buf->lcnt == 0)
		return;

	line = linenr;

	if (line > buf->lcnt)
		line = buf->lcnt;

	if (line == 0)
		line = TERM_CURSOR_MIN;

	if (line > buf->top && line < (buf->top + buf->height)) {
		buf->line = line - buf->top;
	} else {
		if (line < buf->height) {
			buf->top = 0;
			buf->line = line;
		} else {
			buf->top = line - (buf->height / 2);
			buf->line = buf->height / 2;
		}
	}

	buf->column = column;
	cursor_column = column;
	ce_buffer_constrain_cursor_column(buf);

	buffer_update_cursor(buf);

	/* XXX for now. */
	ce_editor_dirty();
}

void
ce_buffer_join_line(void)
{
	const u_int8_t		*p;
	u_int8_t		*ptr;
	struct celine		*line, *next;
	size_t			index, len, tojoin, off;

	if (active->lcnt == 0)
		return;

	index = ce_buffer_line_index(active);
	if (index + 1 > active->lcnt - 1)
		return;

	line = ce_buffer_line_current(active);
	next = &active->lines[index + 1];

	p = next->data;
	tojoin = next->length;
	while (isspace(*p) && tojoin > 0) {
		tojoin--;
		p++;
	}

	if (*p == '\n' || tojoin == 0)
		return;

	ce_buffer_line_allocate(active, line);
	len = line->length + (tojoin - 1) + 1;

	if ((line->data = realloc(line->data, len)) == NULL)
		fatal("%s: realloc %zu: %s", __func__, len, errno_s);

	off = line->length;
	ptr = line->data;
	ptr[line->length - 1] = ' ';
	memcpy(&ptr[line->length], p, tojoin);

	line->maxsz = len;
	line->length = len;
	line->columns = buffer_line_data_to_columns(line->data, line->length);

	ce_buffer_move_down();
	ce_buffer_delete_line(active, 1);
	ce_buffer_move_up();

	active->loff = off;
	active->column = buffer_line_data_to_columns(line->data, active->loff);
	ce_buffer_constrain_cursor_column(active);

	cursor_column = active->column;
	ce_term_setpos(active->cursor_line, active->column);

	ce_editor_dirty();
}

void
ce_buffer_delete_character(void)
{
	size_t			max;
	const u_int8_t		*ptr;
	struct celine		*line;

	if (active->lcnt == 0)
		return;

	line = ce_buffer_line_current(active);
	ptr = line->data;

	if (line->length == 1 && ptr[0] == '\n')
		return;

	active->flags |= CE_BUFFER_DIRTY;

	ce_buffer_line_allocate(active, line);
	buffer_line_erase_character(active, line, 1);

	if (line->length > 0) {
		max = line->length - 1;
		if (ptr[max] == '\n' && max != 0)
			max--;

		if (active->loff > max) {
			active->loff = max;
			buffer_update_cursor_column(active);
		}
	}
}

void
ce_buffer_center_line(struct cebuf *buf, size_t line)
{
	size_t		center, index, span, sp;

	span = 0;
	index = line;
	center = buf->height / 2;

	buf->top = index;

	while (span < center && index > 0) {
		sp = buffer_line_span(buf, &buf->lines[index--]);
		if (span + sp > center)
			break;
		span += sp;
	}

	buf->line = (buf->top - index) + 1;
	buf->top = index;

	buffer_update_cursor_line(buf);

	if (buf == active) {
		ce_term_setpos(buf->cursor_line, buf->column);
		ce_editor_dirty();
	}
}

void
ce_buffer_center(void)
{
	ce_buffer_center_line(active, ce_buffer_line_index(active));
}

void
ce_buffer_top(void)
{
	size_t		index;

	index = ce_buffer_line_index(active);

	active->top = index;
	active->line = active->orig_line;

	buffer_update_cursor_line(active);
	ce_term_setpos(active->cursor_line, active->column);

	ce_editor_dirty();
}

void
ce_buffer_move_up(void)
{
	size_t		index;
	struct celine	*line;
	int		scroll, span;

	if (active->cursor_line < TERM_CURSOR_MIN)
		fatal("%s: line (%zu) < min", __func__, active->cursor_line);

	if (active->cursor_line == TERM_CURSOR_MIN && active->top == 0)
		return;

	scroll = 0;
	index = ce_buffer_line_index(active);
	line = ce_buffer_line_current(active);
	span = buffer_line_span(active, line);

	if (active->cursor_line == TERM_CURSOR_MIN) {
		if (active->top >= active->height / 2) {
			scroll = 1;
			active->top -= active->height / 2;
			active->line = active->height / 2;
			active->cursor_line = active->height / 2;
		} else if (active->top > 0) {
			scroll = 1;
			active->line = active->top;
			active->cursor_line = active->top;
			active->top = 0;
		} else if (active->line > TERM_CURSOR_MIN) {
			active->line--;
		}
	} else if (active->line > TERM_CURSOR_MIN) {
		active->line--;
		buffer_update_cursor(active);
	}

	ce_term_setpos(active->cursor_line, active->column);

	if (scroll)
		ce_editor_dirty();
}

void
ce_buffer_page_up(void)
{
	if (active->lcnt < active->height)
		return;

	if (active->top > active->height)
		active->top -= active->height - 4;
	else
		active->top = 0;

	active->line = active->height / 2;

	buffer_update_cursor(active);

	ce_editor_dirty();
}

void
ce_buffer_move_down(void)
{
	struct celine	*line;
	int		scroll;
	size_t		index, next, span, start;

	if (active->lcnt == 0)
		return;

	if (active->cursor_line > active->height) {
		fatal("%s: line (%zu) > %zu",
		    __func__, active->cursor_line, active->height);
	}

	scroll = 0;
	index = ce_buffer_line_index(active);
	line = ce_buffer_line_current(active);

	if (index == active->lcnt - 1)
		return;

	span = buffer_line_span(active, line);
	if ((active->cursor_line + span) > active->height)
		scroll = 1;

	if (scroll) {
		span = 0;
		start = index;
		while (span < (active->height / 2) + 1 &&
		    index < active->lcnt) {
			span += buffer_line_span(active, &active->lines[index]);
			index++;
		}

		if (index == active->lcnt) {
			buffer_update_cursor(active);
			ce_buffer_center();
		} else {
			active->top += index - start;
			active->line = index - start;

			index = active->top +
			    (active->line - active->orig_line);
			if (index >= active->lcnt)
				active->line = active->lcnt - active->top;
		}

	} else {
		next = active->line + 1;
		if (next <= active->lcnt)
			active->line = next;
	}

	buffer_update_cursor(active);

	if (!scroll) {
		ce_term_setpos(active->cursor_line, active->column);
	} else {
		ce_editor_dirty();
	}
}

void
ce_buffer_page_down(void)
{
	size_t			next, index;

	if (active->lcnt < active->height)
		return;

	next = active->top + active->height;

	index = next + (active->line - 1);
	if (index >= active->lcnt) {
		ce_buffer_jump_down();
	} else {
		active->top += active->height - 2;

		if (index + (active->height / 2) < active->lcnt - 1)
			active->line = active->height / 2;
		else
			ce_buffer_jump_down();
	}

	buffer_update_cursor(active);

	ce_editor_dirty();
}

void
ce_buffer_move_left(void)
{
	struct celine		*line;

	if (active->column < TERM_CURSOR_MIN)
		fatal("%s: col (%zu) < min", __func__, active->column);

	if (active->column == TERM_CURSOR_MIN)
		return;

	if (active->loff == 0)
		return;

	line = ce_buffer_line_current(active);
	buffer_prev_character(active, line);

	active->column = buffer_line_data_to_columns(line->data, active->loff);
	cursor_column = active->column;

	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_jump_left(void)
{
	cursor_column = TERM_CURSOR_MIN;

	active->loff = 0;
	active->column = TERM_CURSOR_MIN;

	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_move_right(void)
{
	struct celine		*line;

	if (active->lcnt == 0)
		return;

	line = ce_buffer_line_current(active);

	if (active->loff == line->length)
		return;

	if (active->loff < line->length - 1)
		buffer_next_character(active, line);

	active->column = buffer_line_data_to_columns(line->data, active->loff);
	ce_buffer_constrain_cursor_column(active);

	cursor_column = active->column;

	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_jump_right(void)
{
	struct celine	*line;

	if (active->lcnt == 0)
		return;

	line = ce_buffer_line_current(active);

	if (line->length > 1)
		active->loff = line->length - 1;
	else
		active->loff = 0;

	active->column = buffer_line_data_to_columns(line->data, active->loff);

	ce_buffer_constrain_cursor_column(active);
	cursor_column = active->column;

	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_jump_down(void)
{
	ce_buffer_jump_line(active, active->lcnt, 0);
}

void
ce_buffer_append(struct cebuf *buf, const void *data, size_t len)
{
	u_int8_t	*p;

	buffer_grow(buf, len);

	p = buf->data;
	memcpy(p + buf->length, data, len);
	buf->length += len;
}

void
ce_buffer_appendf(struct cebuf *buf, const char *fmt, ...)
{
	int		len;
	va_list		args;
	char		str[2048];

	va_start(args, fmt);
	len = vsnprintf(str, sizeof(str), fmt, args);
	va_end(args);

	if (len == -1 || (size_t)len >= sizeof(str)) {
		fatal("%s: failed to format buffer (%d, %zu)",
		    __func__, len, sizeof(str));
	}

	ce_buffer_append(buf, str, len);
}

void
ce_buffer_appendl(struct cebuf *buf, const void *data, size_t len)
{
	size_t			elm;
	int			grow;
	u_int8_t		*ptr;
	struct celine		*line;

	grow = 0;

	if (buf->lcnt != 0) {
		line = &buf->lines[buf->lcnt - 1];
		ptr = line->data;

		if (line->length > 0) {
			if (ptr[line->length - 1] == '\n')
				grow = 1;
		} else {
			grow = 1;
		}
	} else {
		grow = 1;
	}

	if (grow) {
		elm = buf->lcnt;
		buffer_resize_lines(buf, buf->lcnt + 1);

		buf->lines[elm].length = len;
		buf->lines[elm].flags = CE_LINE_ALLOCATED;
		buf->lines[elm].maxsz = buf->lines[elm].length;

		if ((buf->lines[elm].data = calloc(1, len)) == NULL)
			fatal("%s: calloc: %s", __func__, errno_s);

		memcpy(buf->lines[elm].data, data, len);
	} else {
		elm = buf->lcnt - 1;
		line = &buf->lines[elm];
		ce_buffer_line_allocate(buf, line);

		if ((ptr = realloc(line->data, line->length + len)) == NULL)
			fatal("%s: realloc: %s", __func__, errno_s);

		memcpy(&ptr[line->length], data, len);
		line->length += len;
		line->data = ptr;
	}

	ce_buffer_line_columns(&buf->lines[elm]);
}

void
ce_buffer_line_columns(struct celine *line)
{
	line->columns = buffer_line_data_to_columns(line->data, line->length);
}

void
ce_buffer_line_alloc_empty(struct cebuf *buf)
{
	free(buf->data);

	buf->maxsz = 0;
	buf->length = 0;
	buf->data = NULL;

	buf->lcnt = 1;
	free(buf->lines);

	if ((buf->lines = calloc(1, sizeof(struct celine))) == NULL) {
		fatal("%s: calloc(%zu): %s", __func__,
		    sizeof(struct celine), errno_s);
	}

	buf->lines[0].flags = 0;
	buf->lines[0].length = 1;
	buf->lines[0].maxsz = buf->lines[0].length;

	ce_buffer_append(buf, "\n", 1);

	buf->lines[0].data = buf->data;

	ce_buffer_line_columns(&buf->lines[0]);
}

void
ce_buffer_constrain_cursor_column(struct cebuf *buf)
{
	const u_int8_t		*ptr;
	struct celine		*line;

	if (ce_editor_mode() == CE_EDITOR_MODE_INSERT)
		return;

	line = ce_buffer_line_current(buf);
	ptr = line->data;

	if (buf->loff == line->length - 1 && ptr[buf->loff] == '\n') {
		if (buf->loff > TERM_CURSOR_MIN)
			buffer_prev_character(buf, line);
		if (buf->column > TERM_CURSOR_MIN)
			buf->column--;
	}
}

int
ce_buffer_save_active(int force, const char *dstpath)
{
	struct stat		st;
	struct iovec		*iov;
	int			fd, ret;
	size_t			elms, off, cnt, line, maxsz, next;

	fd = -1;
	ret = -1;
	iov = NULL;

	if (dstpath == NULL) {
		if (active->path == NULL) {
			buffer_seterr("buffer has no active path");
			goto cleanup;
		}

		dstpath = active->path;
	} else {
		force = 1;
	}

	if ((active->flags & CE_BUFFER_RO) && force == 0) {
		buffer_seterr("buffer is read-only");
		goto cleanup;
	}

	if (!(active->flags & CE_BUFFER_DIRTY) && force == 0)
		return (0);

	if (stat(dstpath, &st) == -1) {
		if (errno != ENOENT) {
			buffer_seterr("stat failed: %s", errno_s);
			goto cleanup;
		}

		/* Force save, file was probably new. */
		force = 1;
	}

	if (st.st_mtime != active->mtime && force == 0) {
		buffer_seterr("underlying file has changed, use force");
		goto cleanup;
	}

	if ((fd = open(dstpath,
	    O_CREAT | O_TRUNC | O_WRONLY, active->mode)) == -1) {
		buffer_seterr("open(%s): %s", dstpath, errno_s);
		goto cleanup;
	}

	if (ftruncate(fd, 0) == -1) {
		buffer_seterr("ftruncate(%s): %s", dstpath, errno_s);
		goto cleanup;
	}

	maxsz = 32;
	if ((iov = calloc(maxsz, sizeof(struct iovec))) == NULL) {
		fatal("%s: calloc(%zu): %s", __func__,
		    maxsz * sizeof(struct iovec), errno_s);
	}

	off = 1;
	elms = 0;

	/*
	 * Collapse all lines into as little elements as possible by
	 * making use of the fact that if a line was un-edited it will
	 * automatically flow into the next line so we can expand
	 * the iov_data by accounting for line+1 its length into iov_len.
	 */
	for (line = 0; line < active->lcnt; line++) {
		iov[elms].iov_base = active->lines[line].data;
		iov[elms].iov_len = active->lines[line].length;

		if (!(active->lines[line].flags & CE_LINE_ALLOCATED)) {
			for (next = line + 1; next < active->lcnt; next++) {
				if (active->lines[next].flags &
				    CE_LINE_ALLOCATED) {
					break;
				}
			}

			line = next - 1;
		}

		elms++;

		if (elms > maxsz - 1) {
			maxsz += 32;
			iov = realloc(iov, maxsz * sizeof(struct iovec));
			if (iov == NULL) {
				fatal("%s: realloc(%zu): %s", __func__,
				    maxsz * sizeof(struct iovec), errno_s);
			}
		}
	}

	off = 0;
	while (elms > 0) {
		if (elms > BUFFER_MAX_IOVEC)
			cnt = BUFFER_MAX_IOVEC;
		else
			cnt = elms;

		for (;;) {
			if (writev(fd, iov + off, cnt) == -1) {
				if (errno == EINTR)
					continue;
				buffer_seterr("writev(%s): %s",
				    dstpath, errno_s);
				goto cleanup;
			}

			break;
		}

		elms -= cnt;
		off += cnt;
	}

	if (fstat(fd, &st) == -1)
		buffer_seterr("mtime update failed: %s", errno_s);

	if (close(fd) == -1) {
		buffer_seterr("close(%s): %s", dstpath, errno_s);
		goto cleanup;
	}

	ret = 0;
	active->mtime = st.st_mtime;
	active->flags &= ~CE_BUFFER_DIRTY;
	ce_buffer_setname(active, active->path);
	ce_editor_message("%s, wrote %zu lines", active->path, active->lcnt);

cleanup:
	free(iov);

	if (fd != -1)
		(void)close(fd);

	return (ret);
}

struct celine *
ce_buffer_line_current(struct cebuf *buf)
{
	size_t		index;

	index = ce_buffer_line_index(buf);

	return (&buf->lines[index]);
}

size_t
ce_buffer_line_index(struct cebuf *buf)
{
	size_t		index;

	if (buf->line == 0)
		fatal("%s: line == 0", __func__);

	index = buf->top + (buf->line - buf->orig_line);
	if (index >= buf->lcnt)
		fatal("%s: index %zu >= lcnt %zu", __func__, index, buf->lcnt);

	return (index);
}

void
ce_buffer_mark_set(struct cebuf *buf, char mark)
{
	int		idx;

	if (mark < CE_MARK_MIN || mark > CE_MARK_MAX)
		fatal("%s: invalid marker '0x%02x'", __func__, mark);

	idx = mark - CE_MARK_OFFSET;

	buf->markers[idx].set = 1;
	buf->markers[idx].line = ce_buffer_line_index(buf) + 1;

	ce_editor_message("mark %c set", mark);
}

void
ce_buffer_mark_last(struct cebuf *buf, size_t line)
{
	buf->prevmark.set = 1;
	buf->prevmark.line = line;
}

void
ce_buffer_mark_jump(struct cebuf *buf, char mark)
{
	int		idx;
	struct cemark	*mk;
	size_t		lastline;

	if (mark != CE_MARK_PREVIOUS && mark != CE_MARK_SELEXEC &&
	    (mark < CE_MARK_MIN || mark > CE_MARK_MAX))
		fatal("%s: invalid marker '0x%02x'", __func__, mark);

	switch (mark) {
	case CE_MARK_PREVIOUS:
		mk = &buf->prevmark;
		break;
	case CE_MARK_SELEXEC:
		mk = &buf->selexec;
		break;
	default:
		idx = mark - CE_MARK_OFFSET;
		mk = &buf->markers[idx];
		break;
	}

	if (mk->set) {
		lastline = ce_buffer_line_index(buf) + 1;
		ce_buffer_jump_line(buf, mk->line, 0);
		ce_buffer_mark_last(buf, lastline);
	}
}

void
ce_buffer_populate_lines(struct cebuf *buf)
{
	size_t		idx, elm, len;
	char		*start, *data;

	free(buf->lines);

	buf->lcnt = 0;
	buf->lines = NULL;

	data = buf->data;
	start = data;

	len = 0;

	for (idx = 0; idx < buf->length; idx++) {
		if (data[idx] != '\n') {
			len++;
			continue;
		}

		elm = buf->lcnt;
		buffer_resize_lines(buf, buf->lcnt + 1);

		buf->lines[elm].flags = 0;
		buf->lines[elm].data = start;
		buf->lines[elm].length = (&data[idx] - start) + 1;
		buf->lines[elm].maxsz = buf->lines[elm].length;

		ce_buffer_line_columns(&buf->lines[elm]);

		len = 0;
		start = &data[idx + 1];
	}

	if (len > 0) {
		elm = buf->lcnt;
		buffer_resize_lines(buf, buf->lcnt + 1);

		buf->lines[elm].flags = 0;
		buf->lines[elm].data = start;
		buf->lines[elm].length = len;
		buf->lines[elm].maxsz = buf->lines[elm].length;

		ce_buffer_line_allocate(buf, &buf->lines[elm]);

		start = buf->lines[elm].data;
		len = buf->lines[elm].length;

		start[len] = '\n';
		buf->lines[elm].length++;
		ce_buffer_line_columns(&buf->lines[elm]);
	}
}

void
ce_buffer_line_allocate(struct cebuf *buf, struct celine *line)
{
	u_int8_t	*ptr;

	if (!(line->flags & CE_LINE_ALLOCATED)) {
		line->maxsz = line->length + 32;
		if ((ptr = calloc(1, line->maxsz)) == NULL) {
			fatal("%s: calloc(%zu): %s", __func__, line->length,
			    errno_s);
		}

		memcpy(ptr, line->data, line->length);

		/* We don't leak data as it points to inside buf->data. */
		line->data = ptr;
		line->flags |= CE_LINE_ALLOCATED;
	}
}

struct cebuf *
ce_buffer_alloc(int internal)
{
	struct cebuf		*buf;

	if ((buf = calloc(1, sizeof(*buf))) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, sizeof(*buf), errno_s);

	if (internal == 0)
		buf->prev = active;

	buf->top = 0;
	buf->mode = 0644;
	buf->internal = internal;
	buf->type = CE_BUF_TYPE_DEFAULT;
	buf->orig_line = TERM_CURSOR_MIN;
	buf->orig_column = TERM_CURSOR_MIN;

	buf->width = ce_term_width();
	buf->height = ce_term_height() - 3;

	buf->line = buf->orig_line;
	buf->column = buf->orig_column;
	buf->cursor_line = buf->orig_line;

	if (internal == 0)
		TAILQ_INSERT_HEAD(&buffers, buf, list);
	else
		TAILQ_INSERT_HEAD(&internals, buf, list);

	return (buf);
}

static size_t
buffer_line_span(struct cebuf *buf, struct celine *line)
{
	size_t			col;
	const u_int8_t		*ptr;

	ptr = line->data;

	if (line->length > 0 && ptr[line->length - 1] == '\n')
		col = line->columns - 1;
	else
		col = line->columns;

	if (col == buf->width)
		return (1);

	return ((col / buf->width) + 1);
}

static size_t
buffer_line_data_to_columns(const void *data, size_t length)
{
	u_int16_t		cols;
	const u_int8_t		*ptr;
	size_t			idx, seqlen, tw;

	ptr = data;
	tw = config.tab_width;
	cols = TERM_CURSOR_MIN;

	for (idx = 0; idx < length; idx++) {
		if (idx == length - 1 && ptr[idx] == '\n')
			break;

		if (ptr[idx] == '\t') {
			if ((cols % tw) == 0)
				cols += 1;
			else
				cols += tw - (cols % tw) + 1;
		} else {
			cols++;
			if (ce_utf8_sequence(data, length, idx, &seqlen))
				idx += seqlen - 1;
		}
	}

	return (cols);
}

static void
buffer_line_column_to_data(struct cebuf *buf)
{
	u_int16_t		col;
	const u_int8_t		*ptr;
	struct celine		*line;
	size_t			idx, tw, seqlen;

	line = ce_buffer_line_current(buf);

	ptr = line->data;
	tw = config.tab_width;
	col = TERM_CURSOR_MIN;

	for (idx = 0; idx < line->length; idx++) {
		if (col >= buf->column)
			break;

		if (ptr[idx] == '\t') {
			if ((col % tw) == 0)
				col += 1;
			else
				col += tw - (col % tw) + 1;
		} else {
			col++;
		}

		if (col >= buf->column)
			break;

		if (ce_utf8_sequence(line->data, line->length, idx, &seqlen))
			idx += seqlen - 1;
	}

	buf->column = col;
	buf->loff = idx;

	if (buf->loff > line->length - 1)
		buf->loff = line->length - 1;

	if (buf->column > line->columns)
		buf->column = line->columns;
}

static void
buffer_line_insert_byte(struct cebuf *buf, struct celine *line, u_int8_t byte)
{
	u_int8_t	*ptr;

	if (line->length + 1 >= line->maxsz) {
		line->maxsz = line->length + 32;
		if ((line->data = realloc(line->data, line->maxsz)) == NULL) {
			fatal("%s: realloc(%zu): %s", __func__,
			    line->maxsz, errno_s);
		}
	}

	buf->flags |= CE_BUFFER_DIRTY;

	ptr = line->data;
	memmove(&ptr[buf->loff + 1], &ptr[buf->loff],
	    line->length - (buf->loff - 1));
	ptr[buf->loff] = byte;

	line->length++;
	ce_buffer_line_columns(line);

	if (byte == '\n') {
		ce_buffer_move_right();
		ce_editor_dirty();
		return;
	}

	/* Erase the current line and rewrite it completely. */
	if (ce_editor_pasting() == 0) {
		ce_term_setpos(buf->cursor_line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		ce_syntax_init();
		ce_syntax_write(buf, line, 0, line->length);
		ce_syntax_finalize();
	}

	/*
	 * Mimic ce_buffer_move_right().
	 */
	buffer_next_character(buf, line);
	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	ce_buffer_constrain_cursor_column(buf);

	cursor_column = buf->column;

	if (ce_editor_pasting() == 0)
		ce_term_setpos(buf->cursor_line, active->column);

	/* If we overflow terminal width, just redraw completely. */
	if (buffer_line_span(buf, line) > 1)
		ce_editor_dirty();
}

static void
buffer_line_erase_character(struct cebuf *buf, struct celine *line, int inplace)
{
	u_int8_t	*ptr;
	size_t		seqlen, cur, span, span_changed;

	if (line->length == 0)
		return;

	ptr = line->data;
	span = buffer_line_span(buf, line);

	if (ce_utf8_sequence(line->data, line->length, buf->loff, &seqlen) == 0)
		seqlen = 1;

	if (inplace) {
		if (ptr[buf->loff] == '\n')
			return;
		ce_editor_pbuffer_append(&ptr[buf->loff], 1);
		memmove(&ptr[buf->loff], &ptr[buf->loff + seqlen],
		    line->length - buf->loff - seqlen);
		if (buf->loff >= seqlen && buf->loff + 1 == line->length - 1) {
			buf->loff -= seqlen;
		}
	} else {
		cur = buf->loff;
		buffer_prev_character(buf, line);
		memmove(&ptr[buf->loff], &ptr[cur], line->length - cur);
		seqlen = cur - buf->loff;
	}

	line->length -= seqlen;
	span_changed = span != buffer_line_span(buf, line);

	if (span == 1 && span_changed == 0) {
		ce_term_setpos(buf->cursor_line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);

		ce_syntax_init();
		ce_syntax_write(buf, line, 0, line->length);
		ce_syntax_finalize();
	} else {
		ce_editor_dirty();
	}

	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	cursor_column = buf->column;
	ce_buffer_line_columns(line);
	ce_term_setpos(buf->cursor_line, buf->column);

	buf->flags |= CE_BUFFER_DIRTY;
}

static void
buffer_update_cursor(struct cebuf *buf)
{
	buffer_update_cursor_line(buf);
	buffer_update_cursor_column(buf);
}

static void
buffer_update_cursor_line(struct cebuf *buf)
{
	size_t		index, current, span;

	current = ce_buffer_line_index(buf);
	buf->cursor_line = TERM_CURSOR_MIN;

	for (index = buf->top; index < current; index++) {
		span = buffer_line_span(buf, &buf->lines[index]);
		buf->cursor_line += span;
		if (buf->cursor_line > buf->height) {
			buf->cursor_line -= span;
			buf->top++;
			buf->line--;
			buffer_update_cursor_line(buf);
			break;
		}
	}
}

static void
buffer_update_cursor_column(struct cebuf *buf)
{
	struct celine		*line;

	line = ce_buffer_line_current(buf);

	buf->column = cursor_column;

	if (buf->column > line->columns - 1) {
		buf->column = line->columns - 1;
		if (buf->column == 0)
			buf->column = TERM_CURSOR_MIN;
	}

	if (line->length == 0) {
		buf->loff = 0;
	} else {
		buffer_line_column_to_data(buf);
		if (buf->column != TERM_CURSOR_MIN &&
		    buf->loff < line->length - 1)
			buffer_next_character(buf, line);
	}
}

static void
buffer_next_character(struct cebuf *buf, struct celine *line)
{
	size_t		seqlen;

	if (ce_utf8_sequence(line->data, line->length, buf->loff, &seqlen)) {
		buf->loff += seqlen;
	} else {
		buf->loff++;
	}
}

static void
buffer_prev_character(struct cebuf *buf, struct celine *line)
{
	const u_int8_t		*ptr;
	size_t			seqlen, bytes, off, valid;

	valid = 0;
	bytes = 0;
	seqlen = 0;
	ptr = line->data;

	buf->loff--;
	off = buf->loff;

	while (ce_utf8_continuation_byte(ptr[buf->loff]) && buf->loff > 0) {
		buf->loff--;
		bytes++;
	}

	if (bytes > 0) {
		if (ce_utf8_sequence(line->data,
		    line->length, buf->loff, &seqlen)) {
			if ((seqlen - 1) == bytes)
				valid = 1;
		}
	}

	if (valid == 0)
		buf->loff = off;
}

static void
buffer_grow(struct cebuf *buf, size_t len)
{
	void		*r;
	size_t		nlen;

	if ((buf->length + len) < buf->length) {
		fatal("%s: overflow %zu+%zu < %zu",
		    __func__, buf->length, len, buf->length);
	}

	nlen = buf->length + len;
	if (nlen > buf->maxsz) {
		nlen = nlen + 1024;
		if ((r = realloc(buf->data, nlen)) == NULL) {
			fatal("%s: realloc %zu -> %zu: %s", __func__,
			    buf->length, nlen, errno_s);
		}

		buf->data = r;
		buf->maxsz = nlen;
	}
}

static void
buffer_resize_lines(struct cebuf *buf, size_t elm)
{
	buf->lines = realloc(buf->lines, elm * sizeof(struct celine));
	if (buf->lines == NULL) {
		fatal("%s: realloc(%zu): %s", __func__,
		    elm * sizeof(struct celine), errno_s);
	}

	buf->lcnt = elm;
}

static void
buffer_seterr(const char *fmt, ...)
{
	va_list		args;

	free(errstr);

	va_start(args, fmt);
	vasprintf(&errstr, fmt, args);
	va_end(args);
}

static void *
buffer_search(struct cebuf *buf, int dir, const void *needle, size_t len,
    size_t start, size_t end, size_t *index)
{
	void			*p;
	size_t			idx;
	struct celine		*line;

	p = NULL;

	if (dir == BUFFER_SEARCH_FORWARD) {
		for (idx = start; idx < end; idx++) {
			line = &buf->lines[idx];
			if ((p = buffer_find(line, needle, len)) != NULL)
				break;
		}
	} else {
		for (idx = start; idx > end; idx--) {
			line = &buf->lines[idx];
			if ((p = buffer_find(line, needle, len)) != NULL)
				break;
		}
	}

	if (idx == end)
		return (NULL);

	*index = idx;

	return (p);
}

static void *
buffer_find(struct celine *line, const void *needle, size_t len)
{
	const u_int8_t		*n;
	size_t			pos;
	u_int8_t		*src;

	n = needle;
	src = line->data;

	for (pos = 0; pos < line->length; pos++) {
		if (src[pos] != n[0])
			continue;

		if ((line->length - pos) < len)
			return (NULL);

		if (!memcmp(&src[pos], needle, len))
			return (src + pos);
	}

	return (NULL);
}
