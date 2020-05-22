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
#include <sys/mman.h>
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

void
ce_buffer_init(void)
{
	TAILQ_INIT(&buffers);
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
	struct cebuf		*buf, *ret;

	ret = NULL;

	if ((buf = calloc(1, sizeof(*buf))) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, sizeof(*buf), errno_s);

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

	buf->length = (size_t)st.st_size;

	buf->base = mmap(NULL, buf->length, PROT_READ, MAP_PRIVATE, fd, 0);
	if (buf->base == MAP_FAILED) {
		buffer_seterr("mmap error on %s: %s", path, errno_s);
		goto cleanup;
	}

	ret = buf;
	buf = NULL;

cleanup:
	if (buf != NULL)
		ce_buffer_free(buf);

	return (ret);
}

void
ce_buffer_free(struct cebuf *buf)
{
	TAILQ_REMOVE(&buffers, buf, list);

	if (buf->base != NULL)
		(void)munmap(buf->base, buf->length);

	free(buf->path);
	free(buf);
}

void
ce_buffer_reset(struct cebuf *buf)
{
	buf->length = 0;
}

void
ce_buffer_map(struct cebuf *buf)
{
	ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);
	ce_term_setpos(buf->line, buf->column);

	if (buf->base != NULL)
		ce_term_write(buf->base, buf->length);

	ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);
}

void
ce_buffer_command(struct cebuf *buf, char cmd)
{
	if (buf->cb != NULL) {
		buf->cb(buf, cmd);
		return;
	}
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
		if ((r = realloc(buf->base, nlen)) == NULL) {
			fatal("%s: realloc %zu -> %zu: %s", __func__,
			    buf->length, nlen, errno_s);
		}

		buf->base = r;
		buf->maxsz = nlen;
	}

	p = buf->base;
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
