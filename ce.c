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

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ce.h"

static FILE	*logfp = NULL;

int
main(int argc, char *argv[])
{
	if ((logfp = fopen("ce.log", "a")) == NULL)
		fatal("%s: fopen: %s", __func__, errno_s);

	ce_log("ce starting...");

	ce_term_setup();
	ce_editor_loop();
	ce_term_restore();

	ce_log("ce closing...");
	fclose(logfp);

	return (0);
}

void
ce_log(const char *fmt, ...)
{
	va_list		args;

	va_start(args, fmt);
	vfprintf(logfp, fmt, args);
	va_end(args);

	fprintf(logfp, "\n");
	fflush(logfp);
}

void
fatal(const char *fmt, ...)
{
	va_list		args;

	ce_term_restore();

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");

	exit(1);
}
