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

static struct {
	const char		*ext;
	u_int32_t		type;
} file_types[] = {
	{ ".c",		CE_FILE_TYPE_C },
	{ ".cpp",	CE_FILE_TYPE_C },
	{ ".h",		CE_FILE_TYPE_C },
	{ ".py",	CE_FILE_TYPE_PYTHON },
	{ ".diff",	CE_FILE_TYPE_DIFF },
	{ ".patch",	CE_FILE_TYPE_DIFF },
	{ ".js",	CE_FILE_TYPE_JS },
	{ ".sh",	CE_FILE_TYPE_SHELL },
	{ NULL,		0 },
};

static FILE	*fp = NULL;

/* joris' config. */
struct ceconf config = {
	.tab_width = CE_TAB_WIDTH_DEFAULT,
	.tab_expand = CE_TAB_EXPAND_DEFAULT,
};

int
main(int argc, char *argv[])
{
	int		ch, debug;

	debug = 0;

	while ((ch = getopt(argc, argv, "elv")) != -1) {
		switch (ch) {
		case 'e':
			break;
		case 'd':
			debug = 1;
			break;
		case 'l':
			/* lame mode. */
			config.tab_width = 4;
			config.tab_expand = 1;
			break;
		case 'v':
			printf("coma editor 0.2\n");
			exit(0);
		case 't':
			break;
		}
	}

	if (debug) {
		if ((fp = fopen("ce.log", "a")) == NULL)
			fatal("failed to open debug log");
	}

	argc -= optind;
	argv += optind;

	ce_debug("%d args, argv[0] = %s", argc, argv[0]);

	ce_term_setup();

	ce_editor_init();
	ce_buffer_init(argc, argv);

	ce_editor_loop();
	ce_buffer_cleanup();
	ce_term_restore();

	if (debug)
		(void)fclose(fp);

	return (0);
}

void
ce_file_type_detect(struct cebuf *buf)
{
	int			idx;
	const char		*ext;

	buf->type = CE_FILE_TYPE_PLAIN;

	if ((ext = strrchr(buf->path, '.')) == NULL)
		return;

	for (idx = 0; file_types[idx].ext != NULL; idx++) {
		if (strcmp(ext, file_types[idx].ext))
			continue;

		buf->type = file_types[idx].type;
		break;
	}

	if (buf->type == CE_FILE_TYPE_PYTHON) {
		config.tab_width = 4;
		config.tab_expand = 1;
	}

	ce_debug("'%s' is type '%d'", buf->path, buf->type);
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
