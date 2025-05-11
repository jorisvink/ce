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
	{ ".swift",	CE_FILE_TYPE_SWIFT },
	{ ".yml",	CE_FILE_TYPE_YAML },
	{ ".yaml",	CE_FILE_TYPE_YAML },
	{ ".json",	CE_FILE_TYPE_JSON },
	{ ".html",	CE_FILE_TYPE_HTML },
	{ ".css",	CE_FILE_TYPE_CSS },
	{ ".go",	CE_FILE_TYPE_GO },
	{ ".tex",	CE_FILE_TYPE_LATEX },
	{ ".latex",	CE_FILE_TYPE_LATEX },
	{ ".lua",	CE_FILE_TYPE_LUA },
	{ ".zig",	CE_FILE_TYPE_ZIG },
	{ NULL,		0 },
};

static FILE	*fp = NULL;
static int	lame_mode = 0;

/* joris' config. */
struct ceconf config = {
	.tab_show = 1,
	.tab_width = CE_TAB_WIDTH_DEFAULT,
	.tab_expand = CE_TAB_EXPAND_DEFAULT,
};

int
main(int argc, char *argv[])
{
	int		ch, debug;

	debug = 0;

	while ((ch = getopt(argc, argv, "delv")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'l':
			lame_mode = 1;
			break;
		case 'v':
			printf("coma editor 0.3\n");
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

	ce_term_setup();

	ce_editor_init();
	ce_game_init();
	ce_hist_init();

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

	ce_debug("'%s' is type '%d'", buf->path, buf->type);
}

int
ce_lame_mode(void)
{
	return (lame_mode);
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

char *
ce_strdup(const char *p)
{
	char	*np;

	if ((np = strdup(p)) == NULL)
		fatal("%s: strdup(%s): %s", __func__, p, errno_s);

	return (np);
}

void
fatal(const char *fmt, ...)
{
	va_list		args;
	static int	called = 0;

	if (called)
		exit(1);

	called = 1;
	ce_term_restore();

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");

	exit(1);
}
