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

static void		buffer_seterr(const char *, ...)
			    __attribute__((format (printf, 1, 2)));
static u_int16_t	buffer_line_index(struct cebuf *);

static struct cebuflist		buffers;
static char			*errstr = NULL;
static struct cebuf		*active = NULL;
static struct cebuf		*scratch = NULL;

void
ce_buffer_init(int argc, char **argv)
{
	int		i;

	TAILQ_INIT(&buffers);

	if ((scratch = calloc(1, sizeof(*scratch))) == NULL) {
		fatal("%s: calloc(%zu): %s", __func__,
		    sizeof(*scratch), errno_s);
	}

	scratch->top = 0;
	scratch->orig_line = TERM_CURSOR_MIN;
	scratch->orig_column = TERM_CURSOR_MIN;

	scratch->line = scratch->orig_line;
	scratch->column = scratch->orig_column;

	active = scratch;

	for (i = 1; i < argc; i++) {
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

	free(scratch->data);
	free(scratch);

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

	if (path == NULL)
		return (buf);

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

	if (active->lcnt == 0) {
		ce_term_setpos(line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		ce_term_write(active->data, active->length);
	} else {
		for (idx = active->top; idx < active->lcnt; idx++) {
			ce_term_setpos(line, TERM_CURSOR_MIN);
			ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
			ce_term_write(active->lines[idx].data,
			    active->lines[idx].length);

			line++;

			if (line > ce_term_height() - 2)
				break;
		}
	}

	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_command(struct cebuf *buf, char cmd)
{
	if (buf->cb != NULL) {
		active->cb(buf, cmd);
		return;
	}

#if 0
	ce_buffer_append(buf, &cmd, sizeof(cmd));
	active->column++;
#endif
}

void
ce_buffer_move_up(void)
{
	u_int16_t	line;

	if (active->line < TERM_CURSOR_MIN)
		fatal("%s: line (%u) < min", __func__, active->line);

	if (active->line == TERM_CURSOR_MIN)
		return;

	active->line--;

	line = buffer_line_index(active);
	if (active->column > active->lines[line].length) {
		active->column = active->lines[line].length;
		ce_term_setpos(active->line, active->column);
	} else {
		ce_term_writestr(TERM_SEQUENCE_CURSOR_UP);
	}
}

void
ce_buffer_move_down(void)
{
	u_int16_t	line, next;

	if (active->line > ce_term_height() - 2) {
		fatal("%s: line (%u) > %u",
		    __func__, active->line, ce_term_height() - 2);
	}

	if (active->line == ce_term_height() - 2)
		return;

	next = active->line + 1;

	if (next <= active->lcnt) {
		active->line = next;

		line = buffer_line_index(active);
		if (active->column > active->lines[line].length) {
			active->column = active->lines[line].length;
			ce_term_setpos(active->line, active->column);
		} else {
			ce_term_writestr(TERM_SEQUENCE_CURSOR_DOWN);
		}
	}
}

void
ce_buffer_move_left(void)
{
	if (active->column < TERM_CURSOR_MIN)
		fatal("%s: col (%u) < min", __func__, active->column);

	if (active->column == TERM_CURSOR_MIN)
		return;

	active->column--;
	ce_term_writestr(TERM_SEQUENCE_CURSOR_LEFT);
}

void
ce_buffer_jump_left(void)
{
	active->column = TERM_CURSOR_MIN;
	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_move_right(void)
{
	u_int16_t	next, line;

	if (active->column > ce_term_width()) {
		fatal("%s: col (%u) > %u", __func__,
		    active->column, ce_term_width());
	}

	if (active->column == ce_term_width())
		return;

	next = active->column + 1;
	line = buffer_line_index(active);
	if (next <= active->lines[line].length) {
		active->column++;
		ce_term_writestr(TERM_SEQUENCE_CURSOR_RIGHT);
	}
}

void
ce_buffer_jump_right(void)
{
	u_int16_t	line;

	line = buffer_line_index(active);
	active->column = active->lines[line].length;
	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_append(struct cebuf *buf, const void *data, size_t len)
{
	u_int8_t	*p;
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
			fatal("%s: calloc(%zu): %s", __func__,
			    elm * sizeof(char *), errno_s);
		}

		data[idx] = '\0';

		buf->lines[buf->lcnt].data = start;
		buf->lines[buf->lcnt].offset = idx;
		buf->lines[buf->lcnt].length = strlen(start);

		if (buf->lines[buf->lcnt].length == 0)
			buf->lines[buf->lcnt].length = 1;

		data[idx] = '\n';

		start = &data[idx + 1];
		buf->lcnt++;
	}
}

static u_int16_t
buffer_line_index(struct cebuf *buf)
{
	u_int16_t	line;

	if (active->line == 0)
		fatal("%s: line == 0", __func__);

	line = active->line - 1;
	if (line >= buf->lcnt)
		fatal("%s: line %u > lcnt %zu", __func__, line, active->lcnt);

	return (line);
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
