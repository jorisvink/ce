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

static FILE	*fp = NULL;

int
main(int argc, char *argv[])
{
	int		ch, debug;

	debug = 0;

	while ((ch = getopt(argc, argv, "dv")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'v':
			printf("coma editor 0.1\n");
			exit(0);
		}
	}

	if (debug) {
		if ((fp = fopen("ce.log", "a")) == NULL)
			fatal("failed to open debug log");
	}

	argc -= optind;
	argv += optind;

	ce_debug("%d args, argv[0] = %s", argc, argv[0]);

	ce_syntax_init();
	ce_editor_init();
	ce_buffer_init(argc, argv);

	ce_term_setup();
	ce_editor_loop();
	ce_buffer_cleanup();
	ce_term_restore();

	if (debug)
		(void)fclose(fp);

	return (0);
}

void
ce_debug(const char *fmt, ...)
{
	va_list		args;

	if (fp == NULL)
		return;

	va_start(args, fmt);
	vfprintf(fp, fmt, args);
	va_end(args);

	fprintf(fp, "\n");
	fflush(fp);
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
