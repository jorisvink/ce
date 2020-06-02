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

#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

#define EDITOR_MODE_NORMAL	0
#define EDITOR_MODE_INSERT	1
#define EDITOR_MODE_COMMAND	2
#define EDITOR_MODE_BUFLIST	3
#define EDITOR_MODE_MAX		4

#define KEY_MAP_LEN(x)		((sizeof(x) / sizeof(x[0])))

struct keymap {
	char		key;
	void		(*command)(void);
};

static void	editor_signal(int);
static void	editor_event(void);
static void	editor_resume(void);
static void	editor_signal_setup(void);
static int	editor_allowed_command_key(char);
static void	editor_read(int, void *, size_t);

static void	editor_draw_status(void);

static void	editor_cmd_quit(void);
static void	editor_cmd_reset(void);
static void	editor_cmd_suspend(void);
static void	editor_cmd_buffer_list(void);

static void	editor_cmd_open_file(const char *);

static void	editor_cmd_command_mode(void);
static void	editor_cmd_normal_mode(void);

static void	editor_cmd_insert_mode(void);
static void	editor_cmd_insert_mode_append(void);
static void	editor_cmd_insert_mode_prepend(void);

static void	editor_cmd_input(struct cebuf *, char);
static void	editor_buflist_input(struct cebuf *, char);

static struct keymap normal_map[] = {
	{ 'k',		ce_buffer_move_up },
	{ 'j',		ce_buffer_move_down },
	{ 'l',		ce_buffer_move_right },
	{ '$',		ce_buffer_jump_right },
	{ 'h',		ce_buffer_move_left },
	{ '0',		ce_buffer_jump_left },
	{ 0x06,		ce_buffer_page_down },
	{ 0x02,		ce_buffer_page_up },
	{ 'i',		editor_cmd_insert_mode },
	{ 'o',		editor_cmd_insert_mode_append },
	{ 'O',		editor_cmd_insert_mode_prepend },
	{ ':',		editor_cmd_command_mode },
	{ 0x12,		editor_cmd_buffer_list },
	{ '\x1a',	editor_cmd_suspend },
};

static struct keymap insert_map[] = {
	{ '\x1b',	editor_cmd_normal_mode },
};

static struct keymap command_map[] = {
	{ '\x1b',	editor_cmd_normal_mode },
};

static struct keymap buflist_map[] = {
	{ 'k',		ce_buffer_move_up },
	{ 'j',		ce_buffer_move_down },
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
	{ EDITOR_MODE_BUFLIST,	buflist_map,	KEY_MAP_LEN(buflist_map) },
};

static int			quit = 0;
static int			dirty = 1;
static volatile sig_atomic_t	sig_recv = -1;
static struct cebuf		*cmdbuf = NULL;
static struct cebuf		*buflist = NULL;
static const char		colon_char = ':';
static int			mode = EDITOR_MODE_NORMAL;

void
ce_editor_loop(void)
{
	struct pollfd		pfd;
	int			ret;

	editor_signal_setup();

	memset(&pfd, 0, sizeof(pfd));

	pfd.events = POLLIN;
	pfd.fd = STDIN_FILENO;

	cmdbuf = ce_buffer_internal("<cmd>");
	ce_buffer_line_alloc_empty(cmdbuf);

	cmdbuf->cb = editor_cmd_input;
	cmdbuf->line = ce_term_height();
	cmdbuf->orig_line = ce_term_height();

	buflist = ce_buffer_internal("<buffers>");
	buflist->cb = editor_buflist_input;

	while (!quit) {
		if (sig_recv != -1) {
			switch (sig_recv) {
			case SIGQUIT:
			case SIGTERM:
				quit = 1;
				continue;
			case SIGCONT:
				dirty = 1;
				editor_resume();
				break;
			}
			sig_recv = -1;
		}

		if (ce_buffer_active() == cmdbuf)
			ce_buffer_map();

		if (dirty) {
			if (ce_buffer_active() != cmdbuf)
				ce_term_writestr(TERM_SEQUENCE_CLEAR_ONLY);
			ce_buffer_map();
			dirty = 0;
		}

		editor_draw_status();
		ce_term_flush();

		ret = poll(&pfd, 1, -1);
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

void
ce_editor_dirty(void)
{
	dirty = 1;
}

static void
editor_signal(int sig)
{
	sig_recv = sig;
}

static void
editor_signal_setup(void)
{
	struct sigaction	sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = editor_signal;

	if (sigfillset(&sa.sa_mask) == -1)
		fatal("sigfillset: %s", errno_s);

	if (sigaction(SIGCONT, &sa, NULL) == -1)
		fatal("sigfillset: %s", errno_s);
	if (sigaction(SIGQUIT, &sa, NULL) == -1)
		fatal("sigfillset: %s", errno_s);
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		fatal("sigfillset: %s", errno_s);

	(void)signal(SIGINT, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGPIPE, SIG_IGN);
}

static void
editor_event(void)
{
	u_int8_t		key;
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
	case EDITOR_MODE_BUFLIST:
		modestr = "";
		break;
	case EDITOR_MODE_INSERT:
		modestr = "- INSERT -";
		break;
	default:
		fatal("%s: unknown mode %d", __func__, mode);
	}

	ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);

	ce_term_color(TERM_COLOR_WHITE + TERM_COLOR_BG);
	ce_term_color(TERM_COLOR_BLACK + TERM_COLOR_FG);

	ce_term_setpos(ce_term_height() - 1, TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
	ce_term_writef("[%s] [ %zu,%zu-%zu ] [%zu lines] %s",
	    curbuf->name, curbuf->top + curbuf->line, curbuf->loff,
	    curbuf->column, curbuf->lcnt, modestr);

	ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);
	ce_term_reset();
}

static void
editor_cmd_input(struct cebuf *buf, char key)
{
	const char	*cmd;

	switch (key) {
	case '\n':
		ce_buffer_restore();
		cmd = ce_buffer_as_string(buf);
		switch (cmd[1]) {
		case 'q':
			editor_cmd_quit();
			break;
		case 'w':
			if (ce_buffer_save_active() == -1) {
				/* XXX error handling */
				ce_debug("err: %s", ce_buffer_strerror());
				break;
			}
			break;
		case 'd':
			ce_buffer_delete_line(ce_buffer_active());
			break;
		case 'e':
			if (strlen(cmd) > 3)
				editor_cmd_open_file(&cmd[3]);
			break;
		case 'b':
			switch (cmd[2]) {
			case 'c':
				ce_buffer_free(ce_buffer_active());
				ce_editor_dirty();
				break;
			}
			break;
		}
		ce_buffer_activate(buf);
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
		if (editor_allowed_command_key(key)) {
			ce_buffer_append(buf, &key, sizeof(key));
			buf->column++;
		}
		break;
	}

	cmdbuf->lines[0].length = buf->length;
	ce_buffer_line_columns(&cmdbuf->lines[0]);
}

static void
editor_buflist_input(struct cebuf *buf, char key)
{
	size_t		index;

	switch (key) {
	case '\n':
		index = buf->top + (buf->line - 1);
		ce_buffer_activate_index(index);
		mode = EDITOR_MODE_NORMAL;
		break;
	default:
		break;
	}
}

static void
editor_cmd_quit(void)
{
	quit = 1;
}

static void
editor_resume(void)
{
	ce_term_discard();
	ce_term_setup();
	ce_term_flush();
}

static void
editor_cmd_suspend(void)
{
	ce_term_discard();
	ce_term_writestr(TERM_SEQUENCE_ALTERNATE_OFF);
	ce_term_flush();

	kill(0, SIGTSTP);
}

static void
editor_cmd_buffer_list(void)
{
	ce_buffer_list(buflist);
	ce_buffer_activate(buflist);
	ce_buffer_jump_left();

	mode = EDITOR_MODE_BUFLIST;
}

static void
editor_cmd_insert_mode(void)
{
	mode = EDITOR_MODE_INSERT;
}

static void
editor_cmd_insert_mode_append(void)
{
	ce_buffer_jump_left();
	ce_buffer_move_down();
	ce_buffer_input(ce_buffer_active(), '\n');
	ce_buffer_move_up();

	mode = EDITOR_MODE_INSERT;
}

static void
editor_cmd_insert_mode_prepend(void)
{
	ce_buffer_jump_left();
	ce_buffer_input(ce_buffer_active(), '\n');
	ce_buffer_move_up();

	mode = EDITOR_MODE_INSERT;
}

static void
editor_cmd_command_mode(void)
{
	editor_cmd_reset();

	ce_buffer_append(cmdbuf, &colon_char, sizeof(colon_char));

	cmdbuf->column++;
	cmdbuf->lines[0].length = cmdbuf->length;
	ce_buffer_line_columns(&cmdbuf->lines[0]);

	ce_buffer_activate(cmdbuf);

	mode = EDITOR_MODE_COMMAND;
}

static void
editor_cmd_normal_mode(void)
{
	if (mode == EDITOR_MODE_COMMAND || mode == EDITOR_MODE_BUFLIST)
		ce_buffer_restore();

	if (mode == EDITOR_MODE_COMMAND)
		editor_cmd_reset();

	mode = EDITOR_MODE_NORMAL;
}

static void
editor_cmd_reset(void)
{
	ce_buffer_reset(cmdbuf);
	ce_term_setpos(ce_term_height(), TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
}

static int
editor_allowed_command_key(char key)
{
	if (isalnum((unsigned char)key))
		return (1);

	switch (key) {
	case ' ':
	case '.':
	case '/':
		return (1);
	}

	return (0);
}

static void
editor_cmd_open_file(const char *path)
{
	if (ce_buffer_file(path) == NULL) {
		/* XXX error handling */
		ce_debug("cannot open '%s': %s", path, ce_buffer_strerror());
		return;
	}

	ce_editor_dirty();
}
