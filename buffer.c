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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ce.h"

/* Conservative. */
#define BUFFER_MAX_IOVEC		64

static void		buffer_grow(struct cebuf *, size_t);
static void		buffer_seterr(const char *, ...)
			    __attribute__((format (printf, 1, 2)));

static size_t		buffer_line_index(struct cebuf *);
static struct celine	*buffer_line_current(struct cebuf *);
static void		buffer_populate_lines(struct cebuf *);
static void		buffer_line_column_to_data(struct cebuf *);
static void		buffer_update_cursor_column(struct cebuf *);
static u_int16_t	buffer_line_data_to_columns(const void *, size_t);
static void		buffer_line_allocate(struct cebuf *, struct celine *);
static void		buffer_line_erase_byte(struct cebuf *, struct celine *);
static void		buffer_line_insert_byte(struct cebuf *,
			    struct celine *, u_int8_t);

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
	buf->cursor_line = buf->orig_line;

	TAILQ_INSERT_HEAD(&buffers, buf, list);

	if (path == NULL) {
		if ((buf->data = calloc(1, 1024)) == NULL) {
			fatal("%s: calloc(%zu): %s",
			    __func__, buf->maxsz, errno_s);
		}
		buf->maxsz = 1024;
		buffer_populate_lines(buf);
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

	buffer_populate_lines(buf);

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
	buf->cursor_line = buf->orig_line;
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
	u_int16_t	steps;

	if (active->data == NULL)
		return;

	line = active->orig_line;
	ce_term_setpos(active->orig_line, active->orig_column);

	for (idx = active->top; idx < active->lcnt; idx++) {
		ce_term_setpos(line, TERM_CURSOR_MIN);
		ce_term_write(active->lines[idx].data,
		    active->lines[idx].length);

		steps = (active->lines[idx].columns / ce_term_width()) + 1;
		line += steps;

		if (line > ce_term_height() - 2)
			break;
	}

	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_input(struct cebuf *buf, u_int8_t byte)
{
	struct celine		*line;

	if (buf->cb != NULL) {
		buf->cb(buf, byte);
		return;
	}

	line = buffer_line_current(buf);
	buffer_line_allocate(buf, line);

	switch (byte) {
	case '\b':
	case 0x7f:
		if (buf->loff > 0)
			buffer_line_erase_byte(buf, line);
		break;
	case '\n':
		break;
	default:
		buffer_line_insert_byte(buf, line, byte);
		break;
	}
}

void
ce_buffer_delete_line(void)
{
	size_t			index;
	struct celine		*line;

	index = buffer_line_index(active);
	line = buffer_line_current(active);
	ce_debug("removing line %zu", index);

	if (line->flags & CE_LINE_ALLOCATED)
		free(line->data);

	ce_debug("moving %zu lines", active->lcnt - index);

	memmove(&active->lines[index], &active->lines[index + 1],
	    (active->lcnt - index - 1) * sizeof(struct celine));

	active->lcnt--;
	ce_debug("index = %zu, lcnt == %zu", index, active->lcnt);

	if (index == active->lcnt) {
		ce_buffer_move_up();
	} else {
		line = buffer_line_current(active);
		buffer_line_allocate(active, line);
		buffer_update_cursor_column(active);
		ce_term_setpos(active->cursor_line, active->column);
	}
}

void
ce_buffer_move_up(void)
{
	struct celine		*line;
	u_int16_t		lines, last;

	if (active->cursor_line < TERM_CURSOR_MIN)
		fatal("%s: line (%u) < min", __func__, active->cursor_line);

	if (active->cursor_line == TERM_CURSOR_MIN && active->top == 0)
		return;

	if (active->cursor_line < TERM_SCROLL_OFFSET) {
		if (active->top > 0) {
			active->top--;
			line = &active->lines[active->top];
			lines = (line->columns / ce_term_width()) + 1;
			active->cursor_line += lines;
		} else if (active->line > TERM_CURSOR_MIN) {
			active->line--;
		}
	} else if (active->line > TERM_CURSOR_MIN) {
		active->line--;
	}

	line = buffer_line_current(active);
	last = (line->columns / ce_term_width()) + 1;

	if (active->line == TERM_CURSOR_MIN)
		active->cursor_line = TERM_CURSOR_MIN;
	else
		active->cursor_line -= last;

	buffer_update_cursor_column(active);
	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_page_up(void)
{
	size_t			index;
	struct celine		*line;
	u_int16_t		curline, lines, height;

	if (active->top > ce_term_height() - 2)
		active->top -= ce_term_height() - 2;
	else
		active->top = 0;

	curline = 1;
	height = (ce_term_height() - 2) / 2;

	for (index = active->top; curline < height; index++) {
		if (index == active->lcnt - 1)
			break;

		line = &active->lines[index];
		lines = (line->columns / ce_term_width()) + 1;

		curline += lines;
	}

	active->line = index - (active->top - 1);
	active->cursor_line = curline;

	buffer_update_cursor_column(active);
	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_move_down(void)
{
	struct celine	*line;
	size_t		index;
	int		scroll;
	u_int16_t	next, lines, last;

	if (active->cursor_line > ce_term_height() - 2) {
		fatal("%s: line (%u) > %u",
		    __func__, active->cursor_line, ce_term_height() - 2);
	}

	scroll = 0;
	line = buffer_line_current(active);
	last = (line->columns / ce_term_width()) + 1;

	index = buffer_line_index(active);
	if (index == active->lcnt - 1)
		return;

	if (active->cursor_line == ce_term_height() - 2)
		scroll = 1;

	if (active->cursor_line >=
	    (ce_term_height() - 2 - TERM_SCROLL_OFFSET)) {
		scroll = 1;
	} else {
		next = active->line + 1;
		if (next <= active->lcnt)
			active->line = next;
	}

	if (scroll) {
		if (index < active->lcnt - 1) {
			line = &active->lines[active->top++];
			lines = (line->columns / ce_term_width()) + 1;
			active->cursor_line -= lines;
		}
	}

	active->cursor_line += last;

	buffer_update_cursor_column(active);
	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_page_down(void)
{
	struct celine		*line;
	size_t			next, index;
	u_int16_t		lines, curline, height;

	next = active->top + (ce_term_height() - 2);

	index = next + (active->line - 1);
	if (index >= active->lcnt) {
		active->top = (active->lcnt - 1) - 15;
		active->line = TERM_CURSOR_MIN;
		active->cursor_line = TERM_CURSOR_MIN;
	} else {
		active->top += ce_term_height() - 2;
	}

	curline = 1;
	height = (ce_term_height() - 2) / 2;

	for (index = active->top; curline < height; index++) {
		if (index == active->lcnt - 1)
			break;

		line = &active->lines[index];
		lines = (line->columns / ce_term_width()) + 1;

		curline += lines;
	}

	active->line = index - (active->top - 1);
	active->cursor_line = curline;

	buffer_update_cursor_column(active);
	ce_term_setpos(active->cursor_line, active->column);
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

	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_jump_left(void)
{
	active->loff = 0;
	active->column = TERM_CURSOR_MIN;
	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_move_right(void)
{
	struct celine	*line;

	line = buffer_line_current(active);

	if (active->loff == line->length)
		return;

	if (active->loff < line->length - 1)
		active->loff++;

	active->column = buffer_line_data_to_columns(line->data, active->loff);
	cursor_column = active->column;

	ce_term_setpos(active->cursor_line, active->column);
}

void
ce_buffer_jump_right(void)
{
	struct celine	*line;

	line = buffer_line_current(active);

	if (line->length > 0)
		active->loff = line->length - 1;
	else
		active->loff = 0;

	active->column = buffer_line_data_to_columns(line->data,
	    active->loff);

	ce_term_setpos(active->cursor_line, active->column);
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
ce_buffer_line_columns(struct celine *line)
{
	line->columns = buffer_line_data_to_columns(line->data, line->length);
}

int
ce_buffer_save_active(void)
{
	struct iovec		*iov;
	int			fd, len, ret;
	char			path[PATH_MAX];
	size_t			elms, off, cnt, line, maxsz, next;

	fd = -1;
	ret = -1;
	iov = NULL;

	if (active->path == NULL) {
		buffer_seterr("buffer has no active path");
		return (-1);
	}

	len = snprintf(path, sizeof(path), ".%s.ces", active->path);
	if (len == -1 || (size_t)len >= sizeof(path)) {
		buffer_seterr("failed to create path for saving file");
		return (-1);
	}

	if ((fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600)) == -1) {
		buffer_seterr("open(%s): %s", path, errno_s);
		goto cleanup;
	}

	maxsz = 32;
	if ((iov = calloc(maxsz, sizeof(struct iovec))) == NULL) {
		fatal("%s: calloc(%zu): %s", __func__,
		    maxsz * sizeof(struct iovec), strerror(errno));
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
				    CE_LINE_ALLOCATED)
					break;

				iov[elms].iov_len += active->lines[next].length;
			}

			line = next - 1;
		}

		elms++;

		if (elms > maxsz - 1) {
			maxsz += 32;
			iov = realloc(iov, maxsz * sizeof(struct iovec));
			if (iov == NULL) {
				fatal("%s: realloc(%zu): %s", __func__,
				    maxsz * sizeof(struct iovec),
				    strerror(errno));
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
				    path, strerror(errno));
				goto cleanup;
			}

			break;
		}

		elms -= cnt;
		off += cnt;
	}

	if (close(fd) == -1) {
		buffer_seterr("close(%s): %s", path, strerror(errno));
		goto cleanup;
	}

	fd = -1;

	if (rename(path, active->path) == -1) {
		buffer_seterr("rename(%s): %s", path, strerror(errno));
		goto cleanup;
	}

	ret = 0;

cleanup:
	free(iov);
	(void)unlink(path);

	if (fd != -1)
		(void)close(fd);

	return (ret);
}

static void
buffer_populate_lines(struct cebuf *buf)
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

		TAILQ_INIT(&buf->lines[buf->lcnt].ops);

		buf->lines[buf->lcnt].data = start;
		buf->lines[buf->lcnt].length = (&data[idx] - start) + 1;

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

		TAILQ_INIT(&buf->lines[buf->lcnt].ops);

		buf->lines[0].data = buf->data;
		buf->lines[0].length = buf->length;

		ce_buffer_line_columns(&buf->lines[0]);
	}
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
	size_t		index;

	index = buffer_line_index(buf);

	return (&buf->lines[index]);
}

static size_t
buffer_line_index(struct cebuf *buf)
{
	size_t		index;

	if (buf->line == 0)
		fatal("%s: line == 0", __func__);

	index = buf->top + (buf->line - 1);
	if (index >= buf->lcnt)
		fatal("%s: index %zu > lcnt %zu", __func__, index, buf->lcnt);

	return (index);
}

static void
buffer_line_allocate(struct cebuf *buf, struct celine *line)
{
	u_int8_t	*ptr;

	if (!(line->flags & CE_LINE_ALLOCATED)) {
		line->maxsz = line->length + 32;
		if ((ptr = calloc(1, line->maxsz)) == NULL) {
			fatal("%s: calloc(%zu): %s", __func__, line->length,
			    strerror(errno));
		}

		memcpy(ptr, line->data, line->length);

		/* We don't leak data as it points to inside buf->data. */
		line->data = ptr;
		line->flags |= CE_LINE_ALLOCATED;

		ce_debug("line %zu has been allocated", buffer_line_index(buf));
	}
}

static void
buffer_line_insert_byte(struct cebuf *buf, struct celine *line, u_int8_t byte)
{
	u_int8_t	*ptr;

	if (line->length + 1 > line->maxsz) {
		line->maxsz = line->length + 32;
		if ((line->data = realloc(line->data, line->maxsz)) == NULL) {
			fatal("%s: realloc(%zu): %s", __func__,
			    line->maxsz, strerror(errno));
		}
	}

	ptr = line->data;
	memmove(&ptr[buf->loff + 1], &ptr[buf->loff], line->length - buf->loff);

	ptr[buf->loff] = byte;

	line->length++;
	ce_buffer_line_columns(line);

	ce_buffer_move_right();
}

static void
buffer_line_erase_byte(struct cebuf *buf, struct celine *line)
{
	u_int8_t	*ptr;
	size_t		index;

	if (line->length == 0) {
		ce_debug("line needs removing");

		if (buf->lcnt == 1) {
			ce_debug("last line, not removing");
			return;
		}

		index = buffer_line_index(buf);
		memmove(&buf->lines[index], &buf->lines[index + 1],
		    (buf->lcnt - index) * sizeof(struct celine));

		buf->lcnt--;

		ce_buffer_move_up();
		ce_buffer_jump_right();

		return;
	}

	ptr = line->data;
	memmove(&ptr[buf->loff - 1], &ptr[buf->loff], line->length - buf->loff);

	line->length--;

	ce_buffer_line_columns(line);
	ce_buffer_move_left();
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
