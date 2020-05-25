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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

static void		buffer_grow(struct cebuf *, size_t);
static void		buffer_seterr(const char *, ...)
			    __attribute__((format (printf, 1, 2)));

static u_int16_t	buffer_line_index(struct cebuf *);
static struct celine	*buffer_line_current(struct cebuf *);
static void		buffer_line_column_to_data(struct cebuf *);
static void		buffer_update_cursor_column(struct cebuf *);
static u_int16_t	buffer_line_data_to_columns(const void *, size_t);

static struct cebuflist		buffers;
static char			*errstr = NULL;
static struct cebuf		*active = NULL;
static struct cebuf		*scratch = NULL;
static u_int16_t		cursor_column = TERM_CURSOR_MIN;

void
ce_buffer_init(int argc, char **argv)
{
	int		i;

	TAILQ_INIT(&buffers);

	scratch = ce_buffer_alloc(NULL);
	active = scratch;

	for (i = 0; i < argc; i++) {
		if (ce_buffer_alloc(argv[i]) == NULL)
			fatal("%s", errstr);
	}

	if ((active = TAILQ_FIRST(&buffers)) == NULL)
		active = scratch;
}

void
ce_buffer_cleanup(void)
{
	struct cebuf	*buf;

	while ((buf = TAILQ_FIRST(&buffers)) != NULL)
		ce_buffer_free(buf);
}

const char *
ce_buffer_strerror(void)
{
	return (errstr);
}

struct cebuf *
ce_buffer_alloc(const char *path)
{
	int			fd;
	struct stat		st;
	ssize_t			bytes;
	struct cebuf		*buf, *ret;

	fd = -1;
	ret = NULL;

	if ((buf = calloc(1, sizeof(*buf))) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, sizeof(*buf), errno_s);

	buf->prev = active;

	buf->top = 0;
	buf->orig_line = TERM_CURSOR_MIN;
	buf->orig_column = TERM_CURSOR_MIN;

	buf->line = buf->orig_line;
	buf->column = buf->orig_column;

	TAILQ_INSERT_HEAD(&buffers, buf, list);

	if (path == NULL) {
		if ((buf->data = calloc(1, 1024)) == NULL) {
			fatal("%s: calloc(%zu): %s",
			    __func__, buf->maxsz, errno_s);
		}
		ce_buffer_find_lines(buf);
		return (buf);
	}

	if ((buf->path = strdup(path)) == NULL)
		fatal("%s: strdup: %s", __func__, errno_s);

	if ((fd = open(path, O_RDONLY)) == -1) {
		buffer_seterr("cannot open %s: %s", path, errno_s);
		goto cleanup;
	}

	if (fstat(fd, &st) == -1) {
		buffer_seterr("cannot fstat %s: %s", path, errno_s);
		goto cleanup;
	}

	if ((uintmax_t)st.st_size > SIZE_MAX) {
		buffer_seterr("cannot open %s: too large", path);
		goto cleanup;
	}

	buf->maxsz = (size_t)st.st_size;
	buf->length = buf->maxsz;

	if ((buf->data = calloc(1, buf->maxsz)) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, buf->maxsz, errno_s);

	for (;;) {
		bytes = read(fd, buf->data, buf->maxsz);
		if (bytes == -1) {
			if (errno == EINTR)
				continue;
			buffer_seterr("failed to read from %s: %s",
			    path, errno_s);
			goto cleanup;
		}

		if ((size_t)bytes != buf->maxsz) {
			buffer_seterr("read error on %s (%zd/%zu)",
			    path, bytes, buf->maxsz);
			goto cleanup;
		}

		break;
	}

	ce_buffer_find_lines(buf);

	ret = buf;
	active = buf;

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

void
ce_buffer_free(struct cebuf *buf)
{
	TAILQ_REMOVE(&buffers, buf, list);

	free(buf->data);
	free(buf->path);
	free(buf);
}

void
ce_buffer_reset(struct cebuf *buf)
{
	buf->length = 0;
	buf->line = buf->orig_line;
	buf->column = buf->orig_column;
}

void
ce_buffer_restore(void)
{
	if (active->prev == NULL)
		return;

	active = active->prev;
}

void
ce_buffer_activate(struct cebuf *buf)
{
	buf->prev = active;
	active = buf;
}

const char *
ce_buffer_as_string(struct cebuf *buf)
{
	const char	null = '\0';

	ce_buffer_append(buf, &null, sizeof(null));

	return (buf->data);
}

void
ce_buffer_map(void)
{
	size_t		idx;
	int		line;

	if (active->data == NULL)
		return;

	line = active->orig_line;
	ce_term_setpos(active->orig_line, active->orig_column);

	for (idx = active->top; idx < active->lcnt; idx++) {
		ce_term_setpos(line, TERM_CURSOR_MIN);
		ce_term_write(active->lines[idx].data,
		    active->lines[idx].length);

		line++;

		if (line > ce_term_height() - 2)
			break;
	}

	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_input(struct cebuf *buf, char byte)
{
	if (buf->cb != NULL) {
		buf->cb(buf, byte);
		return;
	}
}

void
ce_buffer_move_up(void)
{
	if (active->line < TERM_CURSOR_MIN)
		fatal("%s: line (%u) < min", __func__, active->line);

	if (active->line == TERM_CURSOR_MIN)
		return;

	if (active->line < TERM_SCROLL_OFFSET) {
		if (active->top > 0) {
			active->top--;
		} else {
			active->line--;
		}
	} else {
		active->line--;
	}

	buffer_update_cursor_column(active);
	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_move_down(void)
{
	u_int16_t	next, index;

	if (active->line > ce_term_height() - 2) {
		fatal("%s: line (%u) > %u",
		    __func__, active->line, ce_term_height() - 2);
	}

	if (active->line == ce_term_height() - 2)
		return;

	index = buffer_line_index(active);

	if (active->line >= (ce_term_height() - 2 - TERM_SCROLL_OFFSET)) {
		if (index < active->lcnt - 1)
			active->top++;
	} else {
		next = active->line + 1;
		if (next <= active->lcnt)
			active->line = next;
	}

	buffer_update_cursor_column(active);
	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_move_left(void)
{
	struct celine		*line;

	if (active->column < TERM_CURSOR_MIN)
		fatal("%s: col (%u) < min", __func__, active->column);

	line = buffer_line_current(active);

	if (active->column == TERM_CURSOR_MIN)
		return;

	if (active->loff == 0)
		return;

	active->loff--;
	active->column = buffer_line_data_to_columns(line->data, active->loff);
	cursor_column = active->column;

	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_jump_left(void)
{
	active->loff = 0;
	active->column = TERM_CURSOR_MIN;
	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_move_right(void)
{
	struct celine	*line;

	if (active->column > ce_term_width()) {
		fatal("%s: col (%u) > %u", __func__,
		    active->column, ce_term_width());
	}

	line = buffer_line_current(active);

	if (active->column == ce_term_width())
		return;

	if (active->loff == line->length)
		return;

	if (active->loff < line->length - 1)
		active->loff++;

	active->column = buffer_line_data_to_columns(line->data, active->loff);
	cursor_column = active->column;

	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_jump_right(void)
{
	struct celine	*line;

	line = buffer_line_current(active);

	active->loff = line->length;
	active->column = buffer_line_data_to_columns(line->data,
	    active->loff - 1);

	ce_term_setpos(active->line, active->column);
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
ce_buffer_find_lines(struct cebuf *buf)
{
	size_t		idx, elm;
	char		*start, *data;

	free(buf->lines);

	buf->lcnt = 0;
	buf->lines = NULL;

	data = buf->data;
	start = data;

	for (idx = 0; idx < buf->length; idx++) {
		if (data[idx] != '\n')
			continue;

		elm = buf->lcnt + 1;
		buf->lines = realloc(buf->lines, elm * sizeof(struct celine));
		if (buf->lines == NULL) {
			fatal("%s: realloc(%zu): %s", __func__,
			    elm * sizeof(struct celine), errno_s);
		}

		buf->lines[buf->lcnt].data = start;
		buf->lines[buf->lcnt].length = &data[idx] - start;

		ce_buffer_line_columns(&buf->lines[buf->lcnt]);

		start = &data[idx + 1];
		buf->lcnt++;
	}

	if (buf->lcnt == 0) {
		buf->lcnt = 1;

		if ((buf->lines = calloc(1, sizeof(struct celine))) == NULL) {
			fatal("%s: calloc(%zu): %s", __func__,
			    sizeof(struct celine), errno_s);
		}

		buf->lines[0].data = buf->data;
		buf->lines[0].length = buf->length;

		ce_buffer_line_columns(&buf->lines[0]);
	}
}

void
ce_buffer_line_columns(struct celine *line)
{
	line->columns = buffer_line_data_to_columns(line->data, line->length);
}

static u_int16_t
buffer_line_data_to_columns(const void *data, size_t length)
{
	size_t			idx;
	u_int16_t		cols;
	const u_int8_t		*ptr;

	ptr = data;
	cols = TERM_CURSOR_MIN;

	for (idx = 0; idx < length; idx++) {
		if (ptr[idx] == '\t') {
			if ((cols % 8) == 0)
				cols += 1;
			else
				cols += 8 - (cols % 8) + 1;
		} else {
			cols++;
		}
	}

	return (cols);
}

static void
buffer_line_column_to_data(struct cebuf *buf)
{
	size_t			idx;
	u_int16_t		col;
	const u_int8_t		*ptr;
	struct celine		*line;

	line = buffer_line_current(buf);

	ptr = line->data;
	col = TERM_CURSOR_MIN;

	for (idx = 0; idx < line->length; idx++) {
		if (col >= buf->column)
			break;

		if (ptr[idx] == '\t') {
			if ((col % 8) == 0)
				col += 1;
			else
				col += 8 - (col % 8) + 1;
		} else {
			col++;
		}

		if (col >= buf->column)
			break;
	}

	buf->column = col;
	buf->loff = idx;

	if (buf->loff > line->length - 1) {
		fatal("%s: loff %zu > length %zu", __func__,
		    buf->loff, line->length - 1);
	}

	if (buf->column > line->columns) {
		fatal("%s: colum %u > columns %u", __func__,
		    buf->column, line->columns);
	}
}

static struct celine *
buffer_line_current(struct cebuf *buf)
{
	u_int16_t	index;

	index = buffer_line_index(buf);

	return (&buf->lines[index]);
}

static u_int16_t
buffer_line_index(struct cebuf *buf)
{
	u_int16_t	index;

	if (buf->line == 0)
		fatal("%s: line == 0", __func__);

	index = buf->top + (buf->line - 1);
	if (index >= buf->lcnt)
		fatal("%s: index %u > lcnt %zu", __func__, index, buf->lcnt);

	return (index);
}

static void
buffer_update_cursor_column(struct cebuf *buf)
{
	struct celine		*line;

	line = buffer_line_current(buf);

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
		if (buf->loff < line->length - 1)
			buf->loff++;
	}
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
buffer_seterr(const char *fmt, ...)
{
	va_list		args;

	free(errstr);

	va_start(args, fmt);
	vasprintf(&errstr, fmt, args);
	va_end(args);
}
