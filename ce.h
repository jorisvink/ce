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

#ifndef __H_CE_H

#include <sys/queue.h>

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#define errno_s				strerror(errno)

#define CE_BUFFER_SEARCH_NORMAL		0
#define CE_BUFFER_SEARCH_PREVIOUS	1
#define CE_BUFFER_SEARCH_NEXT		2

#define CE_EDITOR_MODE_NORMAL		0
#define CE_EDITOR_MODE_INSERT		1
#define CE_EDITOR_MODE_COMMAND		2
#define CE_EDITOR_MODE_BUFLIST		3
#define CE_EDITOR_MODE_SEARCH		4
#define CE_EDITOR_MODE_NORMAL_CMD	5
#define CE_EDITOR_MODE_MAX		6

#define TERM_SCROLL_OFFSET		10

#define TERM_COLOR_BLACK		0
#define TERM_COLOR_RED			1
#define TERM_COLOR_GREEN		2
#define TERM_COLOR_YELLOW		3
#define TERM_COLOR_BLUE			4
#define TERM_COLOR_MAGENTA		5
#define TERM_COLOR_CYAN			6
#define TERM_COLOR_WHITE		7

#define TERM_COLOR_BRIGHT		40
#define TERM_COLOR_FG			30
#define TERM_COLOR_BG			40

#define TERM_CURSOR_MIN			1
#define TERM_ESCAPE			"\033["

#define TERM_SEQUENCE_CLEAR_ONLY	TERM_ESCAPE "2J"
#define TERM_SEQUENCE_CLEAR		TERM_ESCAPE "2J" TERM_ESCAPE "1;1H"
#define TERM_SEQUENCE_CURSOR_UP		TERM_ESCAPE "0A"
#define TERM_SEQUENCE_CURSOR_DOWN	TERM_ESCAPE "0B"
#define TERM_SEQUENCE_CURSOR_RIGHT	TERM_ESCAPE "0C"
#define TERM_SEQUENCE_CURSOR_LEFT	TERM_ESCAPE "0D"
#define TERM_SEQUENCE_CURSOR_SAVE	TERM_ESCAPE "s"
#define TERM_SEQUENCE_CURSOR_RESTORE	TERM_ESCAPE "u"
#define TERM_SEQUENCE_LINE_ERASE	TERM_ESCAPE "K"

#define TERM_SEQUENCE_FMT_SET_COLOR	TERM_ESCAPE "%dm"
#define TERM_SEQUENCE_FMT_SET_CURSOR	TERM_ESCAPE "%zu;%zuH"

#define TERM_SEQUENCE_RESET		TERM_ESCAPE "m"

#define TERM_SEQUENCE_ALTERNATE_ON	TERM_ESCAPE "?1049h"
#define TERM_SEQUENCE_ALTERNATE_OFF	TERM_ESCAPE "?1049l"

/*
 * An operation that happened on a line.
 */
struct ceop {
	/* The operation command. */
	u_int16_t		cmd;

	/* The length of the associated data. */
	size_t			length;

	/* Additional data for the operation. */
	u_int8_t		data[4];

	TAILQ_ENTRY(ceop)	list;
};

/*
 * Represents a single line in a file.
 */
#define CE_LINE_ALLOCATED	0x0001
struct celine {
	/* Flags. */
	u_int32_t		flags;

	/* Line data. */
	void			*data;

	/* Size of allocation in case line is allocated. */
	size_t			maxsz;

	/* Length of the line in bytes. */
	size_t			length;

	/* Length of the line in columns. */
	size_t			columns;

	/* List of operations on this line. */
	TAILQ_HEAD(, ceop)	ops;
};

/*
 * A buffer from either a file or internal.
 */
#define CE_BUFFER_DIRTY		0x0001
#define CE_BUFFER_MMAP		0x0002
#define CE_BUFFER_RO		0x0004
struct cebuf {
	/* Internal buffer? */
	int			internal;

	/* Buffer flags. */
	u_int32_t		flags;

	/* The data inside the buffer, its max size and current length. */
	void			*data;
	size_t			maxsz;
	size_t			length;

	/* Pointer to previous buffer. */
	struct cebuf		*prev;

	/* If backed by a file, the path. */
	char			*path;

	/* Name of the buffer. */
	char			*name;

	/* Current cursor line. */
	size_t			cursor_line;

	/* Current line / column offsets. */
	size_t			line;
	size_t			column;

	/* Origin line and column. */
	size_t			orig_line;
	size_t			orig_column;

	/* The "top" of where to begin to render the lines. */
	size_t			top;

	/* The byte offset in the current line we're at. */
	size_t			loff;

	/* Number of lines in this buffer. */
	size_t			lcnt;
	struct celine		*lines;

	/* Callback for special buffers (like cmdbuf). */
	void			(*cb)(struct cebuf *, char);

	TAILQ_ENTRY(cebuf)	list;
};

TAILQ_HEAD(cebuflist, cebuf);

void		ce_buffer_map(void);
void		ce_buffer_cleanup(void);
void		ce_buffer_restore(void);
int		ce_buffer_save_active(int);
void		ce_buffer_init(int, char **);
void		ce_buffer_free(struct cebuf *);
void		ce_buffer_list(struct cebuf *);
void		ce_buffer_reset(struct cebuf *);
void		ce_buffer_activate_index(size_t);
void		ce_buffer_activate(struct cebuf *);
size_t		ce_buffer_line_index(struct cebuf *);
void		ce_buffer_delete_line(struct cebuf *);
void		ce_buffer_insert_line(struct cebuf *);
void		ce_buffer_line_columns(struct celine *);
void		ce_buffer_free_internal(struct cebuf *);
void		ce_buffer_input(struct cebuf *, u_int8_t);
void		ce_buffer_line_alloc_empty(struct cebuf *);
void		ce_buffer_constrain_cursor_column(struct cebuf *);
int		ce_buffer_search(struct cebuf *, const char *, int);
void		ce_buffer_append(struct cebuf *, const void *, size_t);
void		ce_buffer_appendf(struct cebuf *, const char *, ...)
		    __attribute__((format (printf, 2, 3)));

void		ce_buffer_page_up(void);
void		ce_buffer_move_up(void);
void		ce_buffer_page_down(void);
void		ce_buffer_move_down(void);
void		ce_buffer_move_left(void);
void		ce_buffer_jump_left(void);
void		ce_buffer_move_right(void);
void		ce_buffer_jump_right(void);
void		ce_buffer_delete_byte(void);

const char	*ce_buffer_strerror(void);
const char	*ce_buffer_as_string(struct cebuf *);

struct cebuf	*ce_buffer_active(void);
struct cebuf	*ce_buffer_file(const char *);
struct cebuf	*ce_buffer_internal(const char *);
struct celine	*ce_buffer_line_current(struct cebuf *);

size_t		ce_term_width(void);
size_t		ce_term_height(void);

void		ce_term_color(int);
void		ce_term_setup(void);
void		ce_term_reset(void);
void		ce_term_flush(void);
void		ce_term_discard(void);
void		ce_term_restore(void);
void		ce_term_setpos(size_t, size_t);
void		ce_term_writestr(const char *);
void		ce_term_write(const void *, size_t);
void		ce_term_writef(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

void		ce_editor_init(void);
void		ce_editor_loop(void);
int		ce_editor_mode(void);
void		ce_editor_dirty(void);
void		ce_editor_message(const char *, ...);
void		ce_editor_pbuffer_reset(void);
void		ce_editor_pbuffer_append(const void *, size_t);
void		ce_editor_cmdline_append(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

struct cebuf	*ce_editor_cmdbuf(void);
void		ce_editor_cmdbuf_reset(void);

void		ce_syntax_init(void);
void		ce_syntax_write(struct cebuf *, struct celine *, size_t);

void		ce_debug(const char *, ...)
		    __attribute__((format (printf, 1, 2)));
void		fatal(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

#endif
