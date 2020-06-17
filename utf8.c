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
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

int
ce_utf8_continuation_byte(u_int8_t byte)
{
	if (byte & (1 << 7)) {
		if ((byte & (1 << 6)) == 0)
			return (1);
	}

	return (0);
}

int
ce_utf8_sequence(const void *data, size_t len, size_t off, size_t *seqlen)
{
	const u_int8_t		*p;
	size_t			slen, valid, idx;

	if (off > len)
		fatal("%s: off %zu > len %zu", __func__, off, len);

	slen = 0;
	valid = 0;

	p = data;

	/* 2-byte sequence (1100 0000). */
	if (p[off] & ((1 << 7) | (1 << 6))) {
		slen = 2;

		/* 3-byte sequence (1110 0000). */
		if (p[off] & (1 << 5)) {
			slen = 3;

			/* 4-byte sequence (1111 0000). */
			if (p[off] & (1 << 4)) {
				/* check next bit is clear. */
				if ((p[off] & (1 << 3)) == 0)
					slen = 4;
			}
		}
	}

	/* If there are not enough bytes left for the sequence, its bad. */
	if (off + slen > len || slen == 0)
		return (0);

	/* Check that all following bytes look like continuation bytes. */
	for (idx = 1; idx < slen; idx++) {
		if (ce_utf8_continuation_byte(p[off + idx]))
			valid++;
	}

	*seqlen = slen;

	return (valid == (slen - 1));
}
