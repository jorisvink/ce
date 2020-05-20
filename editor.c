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
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ce.h"

#define EDITOR_MODE_NORMAL	0
#define EDITOR_MODE_INSERT	1
#define EDITOR_MODE_MAX		2

#define KEY_MAP_LEN(x)		((sizeof(x) / sizeof(x[0])))

struct keymap {
	char		key;
	void		(*command)(void);
};

static void	editor_event(void);
static void	editor_read(int, void *, size_t);

static void	editor_draw_status(void);

static void	editor_cmd_quit(void);
static void	editor_cmd_move_up(void);
static void	editor_cmd_move_down(void);
static void	editor_cmd_move_left(void);
static void	editor_cmd_jump_left(void);
static void	editor_cmd_move_right(void);
static void	editor_cmd_jump_right(void);

static void	editor_cmd_insert_mode(void);
static void	editor_cmd_normal_mode(void);

static struct keymap normal_map[] = {
	{ 'q',		editor_cmd_quit },
	{ 'k',		editor_cmd_move_up },
	{ 'j',		editor_cmd_move_down },
	{ 'l',		editor_cmd_move_right },
	{ '$',		editor_cmd_jump_right },
	{ 'h',		editor_cmd_move_left },
	{ '0',		editor_cmd_jump_left },
	{ 'i',		editor_cmd_insert_mode },
};

static struct keymap insert_map[] = {
	{ '\x1b',	editor_cmd_normal_mode },
};

#define normal_map_len	(sizeof(normal_map) / sizeof(normal_map[0]))

static struct {
	int			mode;
	const struct keymap	*map;
	size_t			maplen;
} keymaps[] = {
	{ EDITOR_MODE_NORMAL,	normal_map, KEY_MAP_LEN(normal_map) },
	{ EDITOR_MODE_INSERT,	insert_map, KEY_MAP_LEN(insert_map) },
};

static int		quit = 0;

static int		col = TERM_CURSOR_MIN;
static int		line = TERM_CURSOR_MIN;
static int		mode = EDITOR_MODE_NORMAL;

void
ce_editor_loop(void)
{
	int			ret;
	struct pollfd		pfd;

	pfd.events = POLLIN;
	pfd.fd = STDIN_FILENO;

	while (!quit) {
		editor_draw_status();

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

	if (mode >= EDITOR_MODE_MAX)
		fatal("%s: mode %d invalid", __func__, mode);

	editor_read(STDIN_FILENO, &key, sizeof(key));

	for (idx = 0; idx < keymaps[mode].maplen; idx++) {
		if (key == keymaps[mode].map[idx].key) {
			keymaps[mode].map[idx].command();
			break;
		}
	}
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
	ce_term_write(TERM_SEQUENCE_CURSOR_SAVE);
	ce_term_setpos(ce_term_height(), TERM_CURSOR_MIN);

	ce_term_write(TERM_SEQUENCE_LINE_ERASE);
	ce_term_writef("[ %dx%d ]", line, col);

	ce_term_write(TERM_SEQUENCE_CURSOR_RESTORE);
}

static void
editor_cmd_quit(void)
{
	quit = 1;
}

static void
editor_cmd_move_up(void)
{
	if (line < TERM_CURSOR_MIN)
		fatal("%s: line (%d) < 0", __func__, line);

	if (line == TERM_CURSOR_MIN)
		return;

	line--;
	ce_term_write(TERM_SEQUENCE_CURSOR_UP);
}

static void
editor_cmd_move_down(void)
{
	if (line > ce_term_height() - 1) {
		fatal("%s: line (%d) > %d",
		    __func__, line, ce_term_height() - 1);
	}

	if (line == ce_term_height() - 1)
		return;

	line++;
	ce_term_write(TERM_SEQUENCE_CURSOR_DOWN);
}

static void
editor_cmd_move_left(void)
{
	if (col < TERM_CURSOR_MIN)
		fatal("%s: col (%d) < 0", __func__, col);

	if (col == TERM_CURSOR_MIN)
		return;

	col--;
	ce_term_write(TERM_SEQUENCE_CURSOR_LEFT);
}

static void
editor_cmd_jump_left(void)
{
	col = TERM_CURSOR_MIN;
	ce_term_setpos(line, col);
}

static void
editor_cmd_move_right(void)
{
	if (col > ce_term_width())
		fatal("%s: col (%d) > %d", __func__, col, ce_term_width() - 1);

	if (col == ce_term_width())
		return;

	col++;
	ce_term_write(TERM_SEQUENCE_CURSOR_RIGHT);
}

static void
editor_cmd_jump_right(void)
{
	col = ce_term_width();
	ce_term_setpos(line, col);
}

static void
editor_cmd_insert_mode(void)
{
	mode = EDITOR_MODE_INSERT;
}

static void
editor_cmd_normal_mode(void)
{
	mode = EDITOR_MODE_NORMAL;
}
