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

#define TERM_SCROLL_OFFSET		10

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
#define TERM_SEQUENCE_FMT_SET_CURSOR	TERM_ESCAPE "%d;%dH"

#define TERM_SEQUENCE_BACKGROUND_BLACK	TERM_ESCAPE "40m"
#define TERM_SEQUENCE_BACKGROUND_WHITE	TERM_ESCAPE "47m"

#define TERM_SEQUENCE_FOREGROUND_BLACK	TERM_ESCAPE "30m"
#define TERM_SEQUENCE_FOREGROUND_WHITE	TERM_ESCAPE "37m"

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
	u_int16_t		columns;

	/* List of operations on this line. */
	TAILQ_HEAD(, ceop)	ops;
};

/*
 * A buffer from either a file or internal.
 */
struct cebuf {
	/* The data inside the buffer, its max size and current length. */
	void			*data;
	size_t			maxsz;
	size_t			length;

	/* Pointer to previous buffer. */
	struct cebuf		*prev;

	/* If backed by a file, the path. */
	char			*path;

	/* Current cursor line. */
	u_int16_t		cursor_line;

	/* Current line / column offsets. */
	u_int16_t		line;
	u_int16_t		column;

	/* Origin line and column. */
	u_int16_t		orig_line;
	u_int16_t		orig_column;

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
void		ce_buffer_init(int, char **);
void		ce_buffer_free(struct cebuf *);
void		ce_buffer_reset(struct cebuf *);
void		ce_buffer_activate(struct cebuf *);
void		ce_buffer_line_columns(struct celine *);
void		ce_buffer_input(struct cebuf *, u_int8_t);
void		ce_buffer_append(struct cebuf *, const void *, size_t);

void		ce_buffer_page_up(void);
void		ce_buffer_move_up(void);
void		ce_buffer_page_down(void);
void		ce_buffer_move_down(void);
void		ce_buffer_move_left(void);
void		ce_buffer_jump_left(void);
void		ce_buffer_move_right(void);
void		ce_buffer_jump_right(void);

const char	*ce_buffer_strerror(void);
const char	*ce_buffer_as_string(struct cebuf *);

struct cebuf	*ce_buffer_active(void);
struct cebuf	*ce_buffer_alloc(const char *);

u_int16_t	ce_term_width(void);
u_int16_t	ce_term_height(void);
void		ce_term_setup(void);
void		ce_term_flush(void);
void		ce_term_restore(void);
void		ce_term_writestr(const char *);
void		ce_term_write(const void *, size_t);
void		ce_term_setpos(u_int16_t, u_int16_t);
void		ce_term_writef(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

void		ce_editor_loop(void);
void		ce_editor_cmdline_append(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

void		ce_debug(const char *, ...)
		    __attribute__((format (printf, 1, 2)));
void		fatal(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

#endif
