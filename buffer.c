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

static void	buffer_seterr(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

static struct cebuflist		buffers;
static char			*errstr = NULL;
static struct cebuf		*active = NULL;
static struct cebuf		*scratch = NULL;

void
ce_buffer_init(int argc, char **argv)
{
	int		i;

	TAILQ_INIT(&buffers);

	for (i = 1; i < argc; i++) {
		if (ce_buffer_alloc(argv[i]) == NULL)
			fatal("%s", errstr);
	}

	if ((scratch = calloc(1, sizeof(*scratch))) == NULL) {
		fatal("%s: calloc(%zu): %s", __func__,
		    sizeof(*scratch), errno_s);
	}

	scratch->orig_line = TERM_CURSOR_MIN;
	scratch->orig_column = TERM_CURSOR_MIN;

	scratch->line = scratch->orig_line;
	scratch->column = scratch->orig_column;

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
	buf->orig_line = TERM_CURSOR_MIN;
	buf->orig_column = TERM_CURSOR_MIN;
	buf->line = buf->orig_line;
	buf->column = buf->orig_column;

	TAILQ_INSERT_HEAD(&buffers, buf, list);

	if (path == NULL)
		return (buf);

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

	ret = buf;
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
	int		line;
	char		*end, *line_end, *line_start;

	if (active->data == NULL)
		return;

	line = active->orig_line;
	ce_term_setpos(active->orig_line, active->orig_column);

	line_end = NULL;
	line_start = active->data;

	end = line_start + active->length;

	while (line_end < end) {
		if ((line_end = strchr(line_start, '\n')) == NULL) {
			ce_term_write(line_start, end - line_start);
			break;
		}

		ce_term_setpos(line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		ce_term_write(line_start, line_end - line_start);

		line++;

		if (line > ce_term_height() - 2)
			break;

		line_start = line_end + 1;
	}

	ce_term_setpos(active->line, active->column);
}

void
ce_buffer_command(char cmd)
{
	if (active->cb != NULL) {
		active->cb(active, cmd);
		return;
	}

	ce_buffer_append(active, &cmd, sizeof(cmd));
	active->column++;
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

static void
buffer_seterr(const char *fmt, ...)
{
	va_list		args;

	free(errstr);

	va_start(args, fmt);
	vasprintf(&errstr, fmt, args);
	va_end(args);
}
