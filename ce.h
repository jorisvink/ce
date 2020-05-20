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

#define errno_s				strerror(errno)

#define TERM_CURSOR_MIN			1
#define TERM_ESCAPE			"\033["

#define TERM_SEQUENCE_CLEAR		TERM_ESCAPE "2J" TERM_ESCAPE "1;1H"
#define TERM_SEQUENCE_CURSOR_UP		TERM_ESCAPE "0A"
#define TERM_SEQUENCE_CURSOR_DOWN	TERM_ESCAPE "0B"
#define TERM_SEQUENCE_CURSOR_RIGHT	TERM_ESCAPE "0C"
#define TERM_SEQUENCE_CURSOR_LEFT	TERM_ESCAPE "0D"
#define TERM_SEQUENCE_CURSOR_SAVE	TERM_ESCAPE "s"
#define TERM_SEQUENCE_CURSOR_RESTORE	TERM_ESCAPE "u"
#define TERM_SEQUENCE_LINE_ERASE	TERM_ESCAPE "K"
#define TERM_SEQUENCE_FMT_SET_CURSOR	TERM_ESCAPE "%d;%dH"

void	ce_log(const char *, ...);

int	ce_term_width(void);
int	ce_term_height(void);
void	ce_term_setup(void);
void	ce_term_restore(void);
void	ce_term_setpos(int, int);
void	ce_term_write(const char *);
void	ce_term_writef(const char *, ...);

void	ce_editor_loop(void);

void	fatal(const char *, ...);

#endif
