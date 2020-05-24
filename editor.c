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

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

#define EDITOR_MODE_NORMAL	0
#define EDITOR_MODE_INSERT	1
#define EDITOR_MODE_COMMAND	2
#define EDITOR_MODE_MAX		3

#define KEY_MAP_LEN(x)		((sizeof(x) / sizeof(x[0])))

struct keymap {
	char		key;
	void		(*command)(void);
};

static void	editor_event(void);
static void	editor_read(int, void *, size_t);

static void	editor_draw_status(void);

static void	editor_cmd_quit(void);
static void	editor_cmd_reset(void);

static void	editor_cmd_insert_mode(void);
static void	editor_cmd_normal_mode(void);
static void	editor_cmd_command_mode(void);

static void	editor_cmd_input(struct cebuf *, char);

static struct keymap normal_map[] = {
	{ 'k',		ce_buffer_move_up },
	{ 'j',		ce_buffer_move_down },
	{ 'l',		ce_buffer_move_right },
	{ '$',		ce_buffer_jump_right },
	{ 'h',		ce_buffer_move_left },
	{ '0',		ce_buffer_jump_left },
	{ 'i',		editor_cmd_insert_mode },
	{ ':',		editor_cmd_command_mode },
};

static struct keymap insert_map[] = {
	{ '\x1b',	editor_cmd_normal_mode },
};

static struct keymap command_map[] = {
	{ '\x1b',	editor_cmd_normal_mode },
};

static struct {
	int			mode;
	const struct keymap	*map;
	size_t			maplen;
} keymaps[] = {
	{ EDITOR_MODE_NORMAL,	normal_map,	KEY_MAP_LEN(normal_map) },
	{ EDITOR_MODE_INSERT,	insert_map,	KEY_MAP_LEN(insert_map) },
	{ EDITOR_MODE_COMMAND,	command_map,	KEY_MAP_LEN(command_map) },
};

static int		quit = 0;
static const char	colon_char = ':';
static int		mode = EDITOR_MODE_NORMAL;

static struct cebuf	*cmdbuf = NULL;

void
ce_editor_loop(void)
{
	int			ret;
	struct pollfd		pfd;

	memset(&pfd, 0, sizeof(pfd));

	pfd.events = POLLIN;
	pfd.fd = STDIN_FILENO;

	if ((cmdbuf = ce_buffer_alloc(NULL)) == NULL)
		fatal("%s: failed to allocate cmdbuf", __func__);

	cmdbuf->cb = editor_cmd_input;
	cmdbuf->line = ce_term_height();
	cmdbuf->orig_line = ce_term_height();

	if ((cmdbuf->path = strdup("<cmd>")) == NULL)
		fatal("%s: failed to set path for cmdbuf", __func__);

	while (!quit) {
		editor_draw_status();

		ce_buffer_map();
		ce_term_flush();

		ret = poll(&pfd, 1, 1000);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: poll: %s", __func__, errno_s);
		}

		if (ret == 0)
			continue;

		if (pfd.revents & (POLLHUP | POLLERR))
			fatal("%s: poll error", __func__);

		if (pfd.revents & POLLIN)
			editor_event();
	}
}

static void
editor_event(void)
{
	char			key;
	size_t			idx;
	struct cebuf		*curbuf = ce_buffer_active();

	if (mode >= EDITOR_MODE_MAX)
		fatal("%s: mode %d invalid", __func__, mode);

	editor_read(STDIN_FILENO, &key, sizeof(key));

	for (idx = 0; idx < keymaps[mode].maplen; idx++) {
		if (key == keymaps[mode].map[idx].key) {
			keymaps[mode].map[idx].command();
			return;
		}
	}

	if (curbuf == NULL)
		return;

	if (mode == EDITOR_MODE_NORMAL)
		return;

	ce_buffer_input(curbuf, key);
}

static void
editor_read(int fd, void *data, size_t len)
{
	ssize_t		sz;
	size_t		off;
	u_int8_t	*ptr;

	off = 0;
	ptr = data;

	while (len > 0) {
		sz = read(fd, ptr + off, len - off);
		if (sz == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: read: %s", __func__, errno_s);
		}

		off += sz;
		len -= sz;
	}
}

static void
editor_draw_status(void)
{
	const char		*modestr = NULL;
	struct cebuf		*curbuf = ce_buffer_active();

	switch (mode) {
	case EDITOR_MODE_NORMAL:
	case EDITOR_MODE_COMMAND:
		modestr = "";
		break;
	case EDITOR_MODE_INSERT:
		modestr = "- INSERT -";
		break;
	default:
		fatal("%s: unknown mode %d", __func__, mode);
	}

	ce_term_writestr(TERM_SEQUENCE_BACKGROUND_WHITE);
	ce_term_writestr(TERM_SEQUENCE_FOREGROUND_BLACK);

	ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);

	ce_term_setpos(ce_term_height() - 1, TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
	ce_term_writef("[%s] [ %zu,%zu-%u ] [%zu lines] %s",
	    curbuf->path, curbuf->top + curbuf->line, curbuf->loff,
	    curbuf->column, curbuf->lcnt, modestr);

	ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);

	ce_term_writestr(TERM_SEQUENCE_BACKGROUND_BLACK);
	ce_term_writestr(TERM_SEQUENCE_FOREGROUND_WHITE);
}

static void
editor_cmd_input(struct cebuf *buf, char key)
{
	const char	*cmd;

	switch (key) {
	case '\n':
		cmd = ce_buffer_as_string(buf);
		if (!strcmp(cmd, ":q")) {
			editor_cmd_quit();
			return;
		}
		editor_cmd_normal_mode();
		break;
	case '\b':
		if (buf->length > 1) {
			buf->length--;
			buf->column--;
		}
		ce_term_setpos(buf->orig_line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		break;
	case '\t':
		break;
	default:
		ce_buffer_append(buf, &key, sizeof(key));
		buf->column++;
		break;
	}

	ce_buffer_find_lines(buf);
}

static void
editor_cmd_quit(void)
{
	quit = 1;
}

static void
editor_cmd_insert_mode(void)
{
	mode = EDITOR_MODE_INSERT;
}

static void
editor_cmd_command_mode(void)
{
	editor_cmd_reset();

	cmdbuf->column++;
	ce_buffer_append(cmdbuf, &colon_char, sizeof(colon_char));
	ce_buffer_find_lines(cmdbuf);

	ce_buffer_activate(cmdbuf);

	mode = EDITOR_MODE_COMMAND;
}

static void
editor_cmd_normal_mode(void)
{
	if (mode == EDITOR_MODE_COMMAND) {
		ce_buffer_restore();
		editor_cmd_reset();
	}

	mode = EDITOR_MODE_NORMAL;
}

static void
editor_cmd_reset(void)
{
	ce_buffer_reset(cmdbuf);
	ce_term_setpos(ce_term_height(), TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
}
