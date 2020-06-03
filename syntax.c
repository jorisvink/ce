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

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ce.h"

void
ce_syntax_init(void)
{
}

void
ce_syntax_write(struct cebuf *buf, struct celine *line, size_t towrite)
{
	const u_int8_t		*p;
	size_t			off, col, spaces, i;

	off = 0;
	col = 1;
	p = line->data;

	while (off != line->length) {
		if (p[off] == '\t') {
			ce_term_color(TERM_COLOR_BLUE + TERM_COLOR_FG);

			if ((col % 8) == 0)
				spaces = 1;
			else
				spaces = 8 - (col % 8) + 1;

			col += spaces;

			ce_term_write(">", 1);
			for (i = 1; i < spaces; i++)
				ce_term_write(".", 1);

			ce_term_reset();
		} else {
			ce_term_write(&p[off], 1);
			col++;
		}

		off++;
	}
}
