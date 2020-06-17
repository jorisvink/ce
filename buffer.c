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
#include <sys/mman.h>
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

static struct cebuf	*buffer_alloc(int);

static void		buffer_grow(struct cebuf *, size_t);
static void		buffer_resize_lines(struct cebuf *, size_t);
static void		buffer_next_character(struct cebuf *, struct celine *);
static void		buffer_prev_character(struct cebuf *, struct celine *);

static void		buffer_seterr(const char *, ...)
			    __attribute__((format (printf, 1, 2)));

static void		*buffer_find(struct celine *, const void *, size_t);
static void		*buffer_search(struct cebuf *, int, const void *,
			    size_t, size_t, size_t, size_t *);

static size_t		buffer_line_span(struct celine *);
static void		buffer_update_cursor(struct cebuf *);
static void		buffer_populate_lines(struct cebuf *);
static void		buffer_line_column_to_data(struct cebuf *);
static void		buffer_update_cursor_line(struct cebuf *);
static void		buffer_update_cursor_column(struct cebuf *);
static u_int16_t	buffer_line_data_to_columns(const void *, size_t);
static void		buffer_line_allocate(struct cebuf *, struct celine *);
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
	active = scratch;

	for (i = 0; i < argc; i++) {
		if (*argv[i] == '+' && last != NULL) {
			errno = 0;
			linenr = strtol(argv[i], &ep, 10);
			if (errno != 0 || *ep != '\0')
				fatal("%s is a bad line number", argv[i]);

			ce_buffer_jump_line(last, linenr);
			continue;
		}

		if ((last = ce_buffer_file(argv[i])) == NULL)
			ce_editor_message("%s", ce_buffer_strerror());
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

	while ((buf = TAILQ_FIRST(&internals)) != NULL)
		ce_buffer_free_internal(buf);
}

const char *
ce_buffer_strerror(void)
{
	return (errstr);
}

struct cebuf *
ce_buffer_internal(const char *name)
{
	struct cebuf	*buf;

	buf = buffer_alloc(1);

	if ((buf->name = strdup(name)) == NULL)
		fatal("%s: strdup: %s", __func__, errno_s);

	if ((buf->data = calloc(1, 1024)) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, buf->maxsz, errno_s);

	buf->maxsz = 1024;

	ce_buffer_line_alloc_empty(buf);

	return (buf);
}

struct cebuf *
ce_buffer_file(const char *path)
{
	int			fd;
	struct stat		st;
	struct cebuf		*buf, *ret;

	fd = -1;
	ret = NULL;

	TAILQ_FOREACH(buf, &buffers, list) {
		if (!strcmp(buf->name, path)) {
			active = buf;
			return (buf);
		}
	}

	buf = buffer_alloc(0);

	if ((buf->name = strdup(path)) == NULL)
		fatal("%s: strdup: %s", __func__, errno_s);

	if ((buf->path = realpath(path, NULL)) == NULL) {
		if (errno != ENOENT) {
			buffer_seterr("%s: %s", path, errno_s);
			goto cleanup;
		}

		if ((buf->path = strdup(path)) == NULL)
			fatal("%s: strdup: %s", __func__, errno_s);

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

	if ((uintmax_t)st.st_size > SIZE_MAX) {
		buffer_seterr("%s: too large", path);
		goto cleanup;
	}

	buf->maxsz = (size_t)st.st_size;
	buf->length = buf->maxsz;
	buf->mode = st.st_mode;

	if (buf->maxsz > 0) {
		buf->data = mmap(NULL, buf->maxsz,
		    PROT_READ, MAP_PRIVATE, fd, 0);

		if (buf->data == MAP_FAILED) {
			fatal("%s: mmap(%zu): %s",
			    __func__, buf->maxsz, errno_s);
		}

		buf->flags |= CE_BUFFER_MMAP;
	}

finalize:
	ce_file_type_detect(buf);

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

	TAILQ_REMOVE(&buffers, buf, list);

	if (active == buf)
		active = buf->prev;

	TAILQ_FOREACH(bp, &buffers, list) {
		if (bp->prev == buf) {
			ce_debug("adjusted %s", bp->name);
			bp->prev = active;
		}
	}

	if (buf->flags & CE_BUFFER_MMAP) {
		if (munmap(buf->data, buf->length) == -1)
			ce_editor_message("munmap: %s", errno_s);
	} else {
		free(buf->data);
	}

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

	if (active == buf)
		active = buf->prev;

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

	ce_editor_dirty();
}

void
ce_buffer_activate(struct cebuf *buf)
{
	buf->prev = active;
	active = buf;

	ce_editor_dirty();
}

void
ce_buffer_activate_index(size_t index)
{
	size_t			idx;
	struct cebuf		*buf;

	if (index == 0) {
		active = scratch;
		ce_editor_dirty();
		return;
	}

	idx = 1;

	TAILQ_FOREACH_REVERSE(buf, &buffers, cebuflist, list) {
		if (idx++ == index) {
			active = buf;
			ce_editor_dirty();
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
ce_buffer_map(void)
{
	size_t		idx, line, towrite;

	if (active->data == NULL)
		return;

	ce_syntax_init();

	line = active->orig_line;
	ce_term_setpos(active->orig_line, active->orig_column);

	for (idx = active->top; idx < active->lcnt; idx++) {
		ce_term_setpos(line, TERM_CURSOR_MIN);

		towrite = (ce_term_height() - 2 - (line - 1)) * ce_term_width();
		if (towrite > active->lines[idx].length)
			towrite = active->lines[idx].length;

		if (active->internal) {
			ce_term_write(active->lines[idx].data, towrite);
		} else {
			ce_syntax_write(active, &active->lines[idx], towrite);
		}

		line += buffer_line_span(&active->lines[idx]);

		if (line > ce_term_height() - 2)
			break;
	}

	ce_syntax_finalize();

	ce_term_setpos(active->cursor_line, active->column);
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

	ce_debug("word: '%.*s'", (int)*len, *word);
	ce_debug("found start at %zu (%zu)", start, buf->loff);
	ce_debug("end of word %zu (%zu)", end, line->length);

	return (0);
}

void
ce_buffer_word_next(struct cebuf *buf)
{
	const u_int8_t		*ptr;
	struct celine		*line;
	size_t			idx, start;

	if (buf->lcnt == 0)
		return;

	line = ce_buffer_line_current(buf);

	ptr = line->data;

	for (idx = buf->loff; idx < line->length; idx++) {
		if (ce_editor_word_byte(ptr[idx]) == 0)
			break;
	}

	start = idx;
	while (idx < line->length) {
		if (ce_editor_word_byte(ptr[idx]) == 1)
			break;
		idx++;
	}

	if (idx == line->length && start != buf->loff)
		idx = start;

	buf->loff = idx;
	buf->column = buffer_line_data_to_columns(line->data, buf->loff);
	cursor_column = buf->column;

	ce_term_setpos(buf->cursor_line, buf->column);
}

void
ce_buffer_word_prev(struct cebuf *buf)
{
	size_t			idx;
	const u_int8_t		*ptr;
	struct celine		*line;

	if (buf->lcnt == 0 || buf->loff == 0)
		return;

	line = ce_buffer_line_current(buf);
	ptr = line->data;

	for (idx = buf->loff - 1; idx > 0; idx--) {
		if (ce_editor_word_byte(ptr[idx]) == 1)
			break;
	}

	while (idx > 0) {
		if (ce_editor_word_byte(ptr[idx]) == 0)
			break;
		idx--;
	}

	if (idx != 0)
		idx += 1;

	buf->loff = idx;
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
	half = (ce_term_height() - 2) / 2;

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

	idx = 1;
	output->flags |= CE_BUFFER_RO;

	ce_buffer_reset(output);

	/* Add scratch as the first buffer. */
	ce_buffer_appendf(output, "[scratch] (%zu lines)\n", scratch->lcnt);
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
		ce_buffer_appendf(output, "[%s%s] (%zu lines)\n",
		    buf->name, (buf->flags & CE_BUFFER_DIRTY) ? "*" : "",
		    buf->lcnt);
		idx++;
	}

	buffer_populate_lines(output);
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
	buffer_line_allocate(buf, line);

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
		if (buf->type == CE_FILE_TYPE_PYTHON) {
			for (i = 0; i < 4; i++)
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

	TAILQ_INIT(&line->ops);

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
ce_buffer_delete_line(struct cebuf *buf)
{
	size_t			index;
	struct celine		*line;

	if (buf->lcnt == 0)
		return;

	index = ce_buffer_line_index(buf);
	line = ce_buffer_line_current(buf);

	ce_editor_pbuffer_append(line->data, line->length);

	if (line->flags & CE_LINE_ALLOCATED)
		free(line->data);

	memmove(&buf->lines[index], &buf->lines[index + 1],
	    (buf->lcnt - index - 1) * sizeof(struct celine));

	buf->lcnt--;

	if (index == buf->lcnt) {
		ce_buffer_move_up();
	} else {
		line = ce_buffer_line_current(buf);
		buffer_line_allocate(buf, line);
		buffer_update_cursor(buf);
	}

	if (buf->lcnt == 0) {
		cursor_column = TERM_CURSOR_MIN;
		buf->column = TERM_CURSOR_MIN;
		buf->loff = 0;
	}

	buf->flags |= CE_BUFFER_DIRTY;

	/* XXX for now. */
	ce_editor_dirty();
}

void
ce_buffer_jump_line(struct cebuf *buf, long linenr)
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

	if (line < ce_term_height() - 2) {
		buf->top = 0;
		buf->line = line;
	} else {
		buf->top = line - ((ce_term_height() - 2) / 2);
		buf->line = (ce_term_height() - 2) / 2;
	}

	buffer_update_cursor(buf);

	/* XXX for now. */
	ce_editor_dirty();
}

void
ce_buffer_join_line(void)
{
	u_int8_t		*ptr;
	size_t			index, len;
	struct celine		*line, *next;

	if (active->lcnt == 0)
		return;

	index = ce_buffer_line_index(active);
	if (index + 1 > active->lcnt - 1)
		return;

	line = ce_buffer_line_current(active);
	next = &active->lines[index + 1];

	buffer_line_allocate(active, line);
	len = line->length + (next->length - 1);

	if ((line->data = realloc(line->data, len)) == NULL)
		fatal("%s: realloc %zu: %s", __func__, len, errno_s);

	ptr = line->data;
	memcpy(&ptr[line->length - 1], next->data, next->length);

	line->length = len;
	line->columns = buffer_line_data_to_columns(line->data, line->length);

	ce_buffer_move_down();
	ce_buffer_delete_line(active);
	ce_buffer_move_up();

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

	buffer_line_allocate(active, line);
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
ce_buffer_move_up(void)
{
	int			scroll;

	if (active->cursor_line < TERM_CURSOR_MIN)
		fatal("%s: line (%zu) < min", __func__, active->cursor_line);

	if (active->cursor_line == TERM_CURSOR_MIN && active->top == 0)
		return;

	scroll = 0;

	if (active->cursor_line < TERM_SCROLL_OFFSET) {
		if (active->top > 0) {
			scroll = 1;
			active->top--;
		} else if (active->line > TERM_CURSOR_MIN) {
			active->line--;
		}
	} else if (active->line > TERM_CURSOR_MIN) {
		active->line--;
	}

	buffer_update_cursor(active);

	if (!scroll)
		ce_term_setpos(active->cursor_line, active->column);
	else
		ce_editor_dirty();
}

void
ce_buffer_page_up(void)
{
	size_t			index;
	struct celine		*line;
	u_int16_t		curline, lines, height;

	if (active->lcnt < ce_term_height() - 2)
		return;

	if (active->top > ce_term_height() - 2)
		active->top -= (ce_term_height() - 2) - 4;
	else
		active->top = 0;

	curline = 1;
	height = (ce_term_height() - 2) / 2;

	for (index = active->top; curline < height; index++) {
		if (index == active->lcnt - 1)
			break;

		line = &active->lines[index];
		lines = buffer_line_span(line);

		curline += lines;
	}

	active->line = index - (active->top - 1);

	buffer_update_cursor(active);

	ce_editor_dirty();
}

void
ce_buffer_move_down(void)
{
	struct celine	*line;
	int		scroll;
	size_t		index, next, upper, current, lower, lines, diff;

	if (active->lcnt == 0)
		return;

	if (active->cursor_line > ce_term_height() - 2) {
		fatal("%s: line (%zu) > %zu",
		    __func__, active->cursor_line, ce_term_height() - 2);
	}

	diff = 0;
	lines = 0;
	lower = 0;
	upper = 0;
	scroll = 0;

	line = ce_buffer_line_current(active);
	index = ce_buffer_line_index(active);

	if (index == active->lcnt - 1)
		return;

	current = buffer_line_span(line);

	if (current > ce_term_height() - 2)
		scroll = 1;

	if (active->cursor_line == ce_term_height() - 2)
		scroll = 1;

	if (active->cursor_line >= (ce_term_height() - 2 - TERM_SCROLL_OFFSET))
		scroll = 1;

	if (scroll) {
		if (index < active->lcnt - 1) {
			line = &active->lines[active->top++];
			upper = buffer_line_span(line);
		}
	} else {
		next = active->line + 1;
		if (next <= active->lcnt)
			active->line = next;
	}

	if (current < ce_term_height() - 2) {
		lines = 0;
		index = active->top;

		while (lines < ce_term_height() - 2 &&
		    index < active->lcnt - 1) {
			line = &active->lines[index++];
			lines += buffer_line_span(line);
		}

		if (index < active->lcnt - 1) {
			line = &active->lines[index - 1];
			lower = buffer_line_span(line);
		}

		if (upper < lower)
			diff = lower - upper;
		else
			diff = 0;

		if (diff > 0 && scroll) {
			active->top += diff;
			active->line -= diff;
		}
	}

	buffer_update_cursor(active);

	line = ce_buffer_line_current(active);

	if (!scroll)
		ce_term_setpos(active->cursor_line, active->column);
	else
		ce_editor_dirty();
}

void
ce_buffer_page_down(void)
{
	struct celine		*line;
	size_t			next, index;
	u_int16_t		lines, curline, height;

	if (active->lcnt < ce_term_height() - 2)
		return;

	next = active->top + (ce_term_height() - 2);

	index = next + (active->line - 1);
	if (index >= active->lcnt) {
		active->top = (active->lcnt - 1) - 15;
		active->line = TERM_CURSOR_MIN;
	} else {
		active->top += (ce_term_height() - 2) - 2;
	}

	curline = 1;
	height = (ce_term_height() - 2) / 2;

	for (index = active->top; curline < height; index++) {
		if (index == active->lcnt - 1)
			break;

		line = &active->lines[index];
		lines = buffer_line_span(line);

		curline += lines;
	}

	active->line = index - (active->top - 1);

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

	if (active->loff < line->length - 1) 		buffer_next_character(active, line);

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
	ce_buffer_jump_line(active, active->lcnt);
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
ce_buffer_line_columns(struct celine *line)
{
	line->columns = buffer_line_data_to_columns(line->data, line->length);
}

void
ce_buffer_line_alloc_empty(struct cebuf *buf)
{
	if (buf->flags & CE_BUFFER_MMAP) {
		if (munmap(buf->data, buf->maxsz) == -1)
			buffer_seterr("%s: munmap: %s", buf->name, errno_s);

		buf->maxsz = 0;
		buf->length = 0;
		buf->data = NULL;

		buf->flags &= ~CE_BUFFER_MMAP;
	}

	buf->lcnt = 1;

	if ((buf->lines = calloc(1, sizeof(struct celine))) == NULL) {
		fatal("%s: calloc(%zu): %s", __func__,
		    sizeof(struct celine), errno_s);
	}

	TAILQ_INIT(&buf->lines[0].ops);

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
ce_buffer_save_active(int force)
{
	struct iovec		*iov;
	const char		*file, *dir;
	int			fd, len, ret;
	char			path[PATH_MAX], *copy;
	size_t			elms, off, cnt, line, maxsz, next;

	fd = -1;
	ret = -1;
	iov = NULL;
	copy = NULL;
	path[0] = '\0';

	if (active->path == NULL) {
		buffer_seterr("buffer has no active path");
		goto cleanup;
	}

	if ((active->flags & CE_BUFFER_RO) && force == 0) {
		buffer_seterr("buffer is read-only");
		goto cleanup;
	}

	if (!(active->flags & CE_BUFFER_DIRTY) && force == 0)
		return (0);

	if ((copy = strdup(active->path)) == NULL)
		fatal("%s: strdup failed %s", __func__, errno_s);

	if ((file = basename(copy)) == NULL) {
		buffer_seterr("basename failed: %s", errno_s);
		goto cleanup;
	}

	if ((dir = dirname(copy)) == NULL) {
		buffer_seterr("dirname failed: %s", errno_s);
		goto cleanup;
	}

	len = snprintf(path, sizeof(path), "%s/.#%s.tmp", dir, file);
	if (len == -1 || (size_t)len >= sizeof(path)) {
		buffer_seterr("failed to create path for saving file");
		goto cleanup;
	}

	if ((fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600)) == -1) {
		buffer_seterr("open(%s): %s", path, errno_s);
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
				buffer_seterr("writev(%s): %s", path, errno_s);
				goto cleanup;
			}

			break;
		}

		elms -= cnt;
		off += cnt;
	}

	if (fchmod(fd, active->mode) == -1) {
		buffer_seterr("fchmod(%s): %s", path, errno_s);
		goto cleanup;
	}

	if (close(fd) == -1) {
		buffer_seterr("close(%s): %s", path, errno_s);
		goto cleanup;
	}

	fd = -1;

	if (rename(path, active->path) == -1) {
		buffer_seterr("rename(%s): %s", path, errno_s);
		goto cleanup;
	}

	ret = 0;
	active->flags &= ~CE_BUFFER_DIRTY;
	ce_editor_message("wrote %zu lines to %s", active->lcnt, active->name);

cleanup:
	free(iov);
	free(copy);

	if (path[0] != '\0')
		(void)unlink(path);

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

	index = buf->top + (buf->line - 1);
	if (index >= buf->lcnt)
		fatal("%s: index %zu > lcnt %zu", __func__, index, buf->lcnt);

	return (index);
}

static struct cebuf *
buffer_alloc(int internal)
{
	struct cebuf		*buf;

	if ((buf = calloc(1, sizeof(*buf))) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, sizeof(*buf), errno_s);

	if (internal == 0)
		buf->prev = active;

	buf->top = 0;
	buf->internal = internal;
	buf->orig_line = TERM_CURSOR_MIN;
	buf->orig_column = TERM_CURSOR_MIN;

	buf->line = buf->orig_line;
	buf->column = buf->orig_column;
	buf->cursor_line = buf->orig_line;

	if (internal == 0)
		TAILQ_INSERT_HEAD(&buffers, buf, list);
	else
		TAILQ_INSERT_HEAD(&internals, buf, list);

	return (buf);
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

		elm = buf->lcnt;
		buffer_resize_lines(buf, buf->lcnt + 1);
		TAILQ_INIT(&buf->lines[elm].ops);

		buf->lines[elm].flags = 0;
		buf->lines[elm].data = start;
		buf->lines[elm].length = (&data[idx] - start) + 1;
		buf->lines[elm].maxsz = buf->lines[elm].length;

		ce_buffer_line_columns(&buf->lines[elm]);

		start = &data[idx + 1];
	}
}

static size_t
buffer_line_span(struct celine *line)
{
	size_t			col;
	const u_int8_t		*ptr;

	ptr = line->data;

	if (line->length > 0 && ptr[line->length - 1] == '\n')
		col = line->columns - 1;
	else
		col = line->columns;

	if (col == ce_term_width())
		return (1);

	return ((col / ce_term_width()) + 1);
}

static u_int16_t
buffer_line_data_to_columns(const void *data, size_t length)
{
	u_int16_t		cols;
	const u_int8_t		*ptr;
	size_t			idx, seqlen;

	ptr = data;
	cols = TERM_CURSOR_MIN;

	for (idx = 0; idx < length; idx++) {
		if (idx == length - 1 && ptr[idx] == '\n')
			break;

		if (ptr[idx] == '\t') {
			if ((cols % 8) == 0)
				cols += 1;
			else
				cols += 8 - (cols % 8) + 1;
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
	size_t			idx;
	u_int16_t		col;
	const u_int8_t		*ptr;
	struct celine		*line;

	line = ce_buffer_line_current(buf);

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

	if (buf->loff > line->length - 1)
		buf->loff = line->length - 1;

	if (buf->column > line->columns)
		buf->column = line->columns;
}

static void
buffer_line_allocate(struct cebuf *buf, struct celine *line)
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

static void
buffer_line_insert_byte(struct cebuf *buf, struct celine *line, u_int8_t byte)
{
	u_int8_t	*ptr;

	if (line->length + 1 > line->maxsz) {
		line->maxsz = line->length + 32;
		if ((line->data = realloc(line->data, line->maxsz)) == NULL) {
			fatal("%s: realloc(%zu): %s", __func__,
			    line->maxsz, errno_s);
		}
	}

	ptr = line->data;
	memmove(&ptr[buf->loff + 1], &ptr[buf->loff], line->length - buf->loff);

	ptr[buf->loff] = byte;

	line->length++;
	ce_buffer_line_columns(line);

	ce_buffer_move_right();

	/* XXX for now. */
	buf->flags |= CE_BUFFER_DIRTY;
	ce_editor_dirty();
}

static void
buffer_line_erase_character(struct cebuf *buf, struct celine *line, int inplace)
{
	u_int8_t	*ptr;
	size_t		seqlen, cur;

	if (line->length == 0) 		return;

	ptr = line->data;

	if (ce_utf8_sequence(line->data, line->length, buf->loff, &seqlen) == 0)
		seqlen = 1;

	if (inplace) {
		memmove(&ptr[buf->loff], &ptr[buf->loff + seqlen],
		    line->length - buf->loff - seqlen);
	} else {
		cur = buf->loff;
		buffer_prev_character(buf, line);
		memmove(&ptr[buf->loff], &ptr[cur], line->length - cur);
		seqlen = cur - buf->loff;
	}

	line->length -= seqlen;

	if (inplace == 0) {
		buf->column =
		    buffer_line_data_to_columns(line->data, buf->loff);
		cursor_column = buf->column;
		ce_buffer_line_columns(line);
		ce_term_setpos(buf->cursor_line, buf->column);
	}

	/* XXX for now. */
	buf->flags |= CE_BUFFER_DIRTY;
	ce_editor_dirty();
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
	size_t		index, current;

	current = ce_buffer_line_index(buf);
	buf->cursor_line = TERM_CURSOR_MIN;

	for (index = buf->top; index < current; index++)
		buf->cursor_line += buffer_line_span(&buf->lines[index]);
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
