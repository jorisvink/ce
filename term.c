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
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "ce.h"

#define TERM_MIN_ROWS		25
#define TERM_MIN_COLS		50

static struct termios	cur;
static struct termios	old;
static struct winsize	winsz;

static int 		can_restore = 0;
static struct cebuf	*termbuf = NULL;

void
ce_term_setup(void)
{
	memset(&old, 0, sizeof(old));
	memset(&cur, 0, sizeof(cur));

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsz) == -1)
		fatal("%s: ioctl(): %s", __func__, errno_s);

	if (winsz.ws_row < TERM_MIN_ROWS)
		fatal("terminal too small (minimum %d rows)", TERM_MIN_ROWS);
	if (winsz.ws_col < TERM_MIN_COLS)
		fatal("terminal too small (minimum %d columns)", TERM_MIN_COLS);

	if (tcgetattr(STDIN_FILENO, &old) == -1)
		fatal("%s: tcgetattr: %s", __func__, errno_s);

	cur = old;

	cur.c_cc[VMIN] = 1;
	cur.c_cc[VTIME] = 0;
	cur.c_oflag &= ~ONLCR;
	cur.c_iflag &= ~ONLCR;
	cur.c_lflag &= ~(ICANON | ECHO | ISIG | ECHOE);

	if (tcsetattr(STDIN_FILENO, TCSANOW, &cur) == -1)
		fatal("%s: tcsetattr: %s", __func__, errno_s);

	if ((termbuf = calloc(1, sizeof(*termbuf))) == NULL) {
		fatal("%s: calloc(%zu): %s", __func__,
		    sizeof(*termbuf), errno_s);
	}

	can_restore = 1;
	ce_term_writestr(TERM_SEQUENCE_CLEAR);
}

void
ce_term_restore(void)
{
	if (can_restore)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &old);

	can_restore = 0;
}

u_int16_t
ce_term_height(void)
{
	return (winsz.ws_row);
}

u_int16_t
ce_term_width(void)
{
	return (winsz.ws_col);
}

void
ce_term_setpos(u_int16_t line, u_int16_t col)
{
	u_int16_t	adj, orig;

	if (col < TERM_CURSOR_MIN) {
		fatal("%s: invalid column %u (%u)",
		    __func__, col, ce_term_width());
	}

	if (line < TERM_CURSOR_MIN || line > ce_term_height()) {
		fatal("%s: invalid line %u (%u)",
		    __func__, line, ce_term_height());
	}

	orig = col;
	adj = col / (ce_term_width() + 1);
	if ((col = col % ce_term_width()) == 0)
		col = ce_term_width();

	ce_term_writef(TERM_SEQUENCE_FMT_SET_CURSOR, line + adj, col);
}

void
ce_term_writestr(const char *data)
{
	ce_term_write(data, strlen(data));
}

void
ce_term_writef(const char *fmt, ...)
{
	int		len;
	va_list		args;
	char		buf[2048];

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	if (len == -1)
		fatal("%s: failed to format buffer", __func__);

	if ((size_t)len >= sizeof(buf))
		fatal("%s: format too large (%d) bytes", __func__, len);

	va_end(args);

	ce_term_write(buf, len);
}

void
ce_term_write(const void *data, size_t len)
{
	ce_buffer_append(termbuf, data, len);
}

void
ce_term_flush(void)
{
	ssize_t		sz;

	if (termbuf->data == NULL || termbuf->length == 0)
		return;

	for (;;) {
		sz = write(STDOUT_FILENO, termbuf->data, termbuf->length);
		if (sz == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: write: %s", __func__, errno_s);
		}

		break;
	}

	ce_buffer_reset(termbuf);
}
