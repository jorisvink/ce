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

#include <stdio.h>
#include <termios.h>
#include <unistd.h>

#include "ce.h"

static struct termios	cur;
static struct termios	old;

static int 		can_restore = 0;

void
ce_term_setup(void)
{
	memset(&old, 0, sizeof(old));
	memset(&cur, 0, sizeof(cur));

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

int
ce_term_height(void)
{
	/* XXX */
	return (25);
}

int
ce_term_width(void)
{
	/* XXX */
	return (80);
}

void
ce_term_setpos(int line, int col)
{
	if (col < TERM_CURSOR_MIN || col > ce_term_width())
		fatal("%s: invalid column %d", __func__, col);

	if (line < TERM_CURSOR_MIN || line > ce_term_height())
		fatal("%s: invalid line %d", __func__, line);

	ce_term_writef(TERM_SEQUENCE_FMT_SET_CURSOR, line, col);
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
	ssize_t		sz;

	for (;;) {
		if ((sz = write(STDOUT_FILENO, data, len)) == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: write: %s", __func__, errno_s);
		}

		break;
	}
}
