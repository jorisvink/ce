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
#include <time.h>
#include <unistd.h>

#include "ce.h"

/* Show messages for 5 seconds. */
#define EDITOR_MESSAGE_DELAY	5

#define EDITOR_KEY_ESC		0x1b
#define EDITOR_KEY_UP		0xfa
#define EDITOR_KEY_DOWN		0xfb
#define EDITOR_KEY_LEFT		0xfc
#define EDITOR_KEY_RIGHT	0xfd

#define EDITOR_COMMAND_DELETE	1
#define EDITOR_COMMAND_YANK	2

#define KEY_MAP_LEN(x)		((sizeof(x) / sizeof(x[0])))

struct keymap {
	u_int8_t	key;
	void		(*command)(void);
};

static void	editor_signal(int);
static void	editor_resume(void);
static void	editor_signal_setup(void);
static void	editor_process_input(void);
static u_int8_t	editor_process_escape(void);
static int	editor_allowed_command_key(char);
static ssize_t	editor_read(int, void *, size_t, int);

static void	editor_draw_status(void);

static void	editor_cmd_quit(int);
static void	editor_cmd_reset(void);
static void	editor_cmd_suspend(void);
static void	editor_cmd_paste(void);
static void	editor_cmd_search_next(void);
static void	editor_cmd_search_prev(void);
static void	editor_cmd_buffer_list(void);

static void	editor_cmd_open_file(const char *);

static void	editor_cmd_command_mode(void);
static void	editor_cmd_search_mode(void);
static void	editor_cmd_normal_mode(void);

static void	editor_cmd_yank_lines(struct cebuf *, long);
static void	editor_cmd_delete_lines(struct cebuf *, long);

static void	editor_cmd_insert_mode(void);
static void	editor_cmd_insert_mode_append(void);
static void	editor_cmd_insert_mode_prepend(void);

static void	editor_normal_mode_command(char key);

static void	editor_cmdbuf_input(struct cebuf *, char);
static void	editor_cmdbuf_search(struct cebuf *, char);
static void	editor_buflist_input(struct cebuf *, char);

static struct keymap normal_map[] = {
	{ 'k',			ce_buffer_move_up },
	{ 'j',			ce_buffer_move_down },
	{ 'l',			ce_buffer_move_right },
	{ 'h',			ce_buffer_move_left },

	{ '$',			ce_buffer_jump_right },
	{ '0',			ce_buffer_jump_left },
	{ 'G',			ce_buffer_jump_down },
	{ 'J',			ce_buffer_join_line },

	{ 0x06,			ce_buffer_page_down },
	{ 0x02,			ce_buffer_page_up },

	{ 'x',			ce_buffer_delete_byte },
	{ 'p',			editor_cmd_paste },
	{ 'n',			editor_cmd_search_next },
	{ 'N',			editor_cmd_search_prev },

	{ 'i',			editor_cmd_insert_mode },
	{ 'o',			editor_cmd_insert_mode_append },
	{ 'O',			editor_cmd_insert_mode_prepend },

	{ ':',			editor_cmd_command_mode },
	{ '/',			editor_cmd_search_mode },
	{ 0x12,			editor_cmd_buffer_list },

	{ 0x1a,			editor_cmd_suspend },
};

static struct keymap insert_map[] = {
	{ EDITOR_KEY_UP,	ce_buffer_move_up },
	{ EDITOR_KEY_DOWN,	ce_buffer_move_down },
	{ EDITOR_KEY_RIGHT,	ce_buffer_move_right },
	{ EDITOR_KEY_LEFT,	ce_buffer_move_left },

	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },

};

static struct keymap command_map[] = {
	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },
};

static struct keymap buflist_map[] = {
	{ 'k',			ce_buffer_move_up },
	{ 'j',			ce_buffer_move_down },
	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },
};

static struct {
	int			mode;
	const struct keymap	*map;
	size_t			maplen;
} keymaps[] = {
	{ CE_EDITOR_MODE_NORMAL, normal_map, KEY_MAP_LEN(normal_map) },
	{ CE_EDITOR_MODE_INSERT, insert_map, KEY_MAP_LEN(insert_map) },
	{ CE_EDITOR_MODE_COMMAND, command_map, KEY_MAP_LEN(command_map) },
	{ CE_EDITOR_MODE_BUFLIST, buflist_map, KEY_MAP_LEN(buflist_map) },
	{ CE_EDITOR_MODE_SEARCH, command_map, KEY_MAP_LEN(command_map) },
};

static struct {
	char			*message;
	time_t			when;
} msg;

static int			quit = 0;
static int			dirty = 1;
static volatile sig_atomic_t	sig_recv = -1;
static int			normalcmd = -1;
static char			*search = NULL;
static struct cebuf		*cmdbuf = NULL;
static struct cebuf		*buflist = NULL;
static struct cebuf		*pbuffer = NULL;
static int			mode = CE_EDITOR_MODE_NORMAL;

void
ce_editor_init(void)
{
	msg.message = NULL;
	msg.when = 0;

	editor_signal_setup();
}

void
ce_editor_loop(void)
{
	struct timespec		ts;

	pbuffer = ce_buffer_internal("<pb>");
	ce_buffer_reset(pbuffer);

	cmdbuf = ce_buffer_internal("<cmd>");
	ce_buffer_reset(cmdbuf);

	cmdbuf->cb = editor_cmdbuf_input;
	cmdbuf->line = ce_term_height();
	cmdbuf->orig_line = ce_term_height();

	buflist = ce_buffer_internal("<buffers>");
	buflist->cb = editor_buflist_input;

	while (!quit) {
		(void)clock_gettime(CLOCK_MONOTONIC, &ts);

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
			case SIGWINCH:
				dirty = 1;
				ce_term_restore();
				ce_term_setup();

				cmdbuf->line = ce_term_height();
				cmdbuf->orig_line = ce_term_height();
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

		if (msg.message) {
			if ((ts.tv_sec - msg.when) >= EDITOR_MESSAGE_DELAY) {
				free(msg.message);
				msg.message = NULL;
			}

			ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);
			ce_term_setpos(ce_term_height(), TERM_CURSOR_MIN);
			ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
			if (msg.message)
				ce_term_writestr(msg.message);
			ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);
		}

		ce_term_flush();

		editor_process_input();
	}

	free(search);
	free(msg.message);
}

void
ce_editor_dirty(void)
{
	dirty = 1;
}

int
ce_editor_mode(void)
{
	return (mode);
}

void
ce_editor_message(const char *fmt, ...)
{
	struct timespec		ts;
	va_list			args;

	free(msg.message);

	va_start(args, fmt);
	if (vasprintf(&msg.message, fmt, args) == -1)
		fatal("%s: vasprintf: %s", __func__, errno_s);
	va_end(args);

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	msg.when = ts.tv_sec;
}

void
ce_editor_pbuffer_reset(void)
{
	ce_buffer_reset(pbuffer);
}

void
ce_editor_pbuffer_append(const void *data, size_t len)
{
	ce_debug("paste buffer got %zu bytes", len);
	ce_buffer_append(pbuffer, data, len);
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
		fatal("sigaction: %s", errno_s);
	if (sigaction(SIGQUIT, &sa, NULL) == -1)
		fatal("sigaction: %s", errno_s);
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		fatal("sigaction: %s", errno_s);
	if (sigaction(SIGWINCH, &sa, NULL) == -1)
		fatal("sigaction: %s", errno_s);

	(void)signal(SIGINT, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGPIPE, SIG_IGN);
}

static void
editor_process_input(void)
{
	size_t			idx;
	u_int8_t		key;
	struct cebuf		*curbuf = ce_buffer_active();

	if (mode >= CE_EDITOR_MODE_MAX)
		fatal("%s: mode %d invalid", __func__, mode);

	if (editor_read(STDIN_FILENO, &key, sizeof(key), -1) == 0)
		return;

	if (key == EDITOR_KEY_ESC)
		key = editor_process_escape();

	if (mode == CE_EDITOR_MODE_NORMAL_CMD) {
		editor_normal_mode_command(key);
		return;
	}

	for (idx = 0; idx < keymaps[mode].maplen; idx++) {
		if (key == keymaps[mode].map[idx].key) {
			keymaps[mode].map[idx].command();
			return;
		}
	}

	if (mode == CE_EDITOR_MODE_NORMAL) {
		editor_normal_mode_command(key);
		return;
	}

	ce_buffer_input(curbuf, key);
}

static u_int8_t
editor_process_escape(void)
{
	u_int8_t	key, ret;

	ret = EDITOR_KEY_ESC;

	if (editor_read(STDIN_FILENO, &key, sizeof(key), 5) == 0)
		goto cleanup;

	if (key != 0x5b)
		goto cleanup;

	if (editor_read(STDIN_FILENO, &key, sizeof(key), 50) == 0)
		goto cleanup;

	switch (key) {
	case 0x41:
		ret = EDITOR_KEY_UP;
		break;
	case 0x42:
		ret = EDITOR_KEY_DOWN;
		break;
	case 0x43:
		ret = EDITOR_KEY_RIGHT;
		break;
	case 0x44:
		ret = EDITOR_KEY_LEFT;
		break;
	}

cleanup:
	return (ret);
}

static ssize_t
editor_read(int fd, void *data, size_t len, int ms)
{
	size_t			off;
	int			nfd;
	struct pollfd		pfd;
	u_int8_t		*ptr;
	ssize_t			sz, total;

	off = 0;
	total = 0;
	ptr = data;

	pfd.events = POLLIN;
	pfd.fd = STDIN_FILENO;

	if ((nfd = poll(&pfd, 1, ms)) == -1) {
		if (errno == EINTR)
			return (0);
		fatal("%s: poll %s", __func__, errno_s);
	}

	if (nfd == 0)
		return (0);

	if (pfd.revents & (POLLHUP | POLLERR))
		fatal("%s: poll error", __func__);

	while (len > 0) {
		sz = read(fd, ptr + off, len - off);
		if (sz == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: read: %s", __func__, errno_s);
		}

		off += sz;
		len -= sz;
		total += sz;
	}

	return (total);
}

static void
editor_draw_status(void)
{
	const char		*isdirty = "";
	const char		*filemode = "";
	const char		*modestr = NULL;
	struct cebuf		*curbuf = ce_buffer_active();

	switch (mode) {
	case CE_EDITOR_MODE_NORMAL:
	case CE_EDITOR_MODE_COMMAND:
	case CE_EDITOR_MODE_BUFLIST:
	case CE_EDITOR_MODE_NORMAL_CMD:
		modestr = "";
		break;
	case CE_EDITOR_MODE_SEARCH:
		modestr = "- SEARCH -";
		break;
	case CE_EDITOR_MODE_INSERT:
		modestr = "- INSERT -";
		break;
	default:
		fatal("%s: unknown mode %d", __func__, mode);
	}

	if (curbuf->flags & CE_BUFFER_DIRTY)
		isdirty = "*";

	if (curbuf->flags & CE_BUFFER_RO)
		filemode = "r-";
	else
		filemode = "rw";

	ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);

	ce_term_color(TERM_COLOR_WHITE + TERM_COLOR_BG);
	ce_term_color(TERM_COLOR_BLACK + TERM_COLOR_FG);

	ce_term_setpos(ce_term_height() - 1, TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
	ce_term_writef("[%s%s] [%s] [ %zu,%zu-%zu ] [%zu lines] %s",
	    curbuf->name, isdirty, filemode, curbuf->top + curbuf->line,
	    curbuf->loff, curbuf->column, curbuf->lcnt, modestr);

	ce_term_reset();
	ce_term_setpos(ce_term_height(), ce_term_width() * 0.75f);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);

	if (mode == CE_EDITOR_MODE_NORMAL_CMD)
		ce_term_write(cmdbuf->data, cmdbuf->length);

	ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);
}

static void
editor_cmdbuf_input(struct cebuf *buf, char key)
{
	char			*ep;
	const char		*cmd;
	int			force;
	long			linenr;

	switch (key) {
	case '\n':
		ce_buffer_restore();
		cmd = ce_buffer_as_string(buf);

		if (isdigit((unsigned char)cmd[1])) {
			errno = 0;
			linenr = strtol(&cmd[1], &ep, 10);
			if (*ep == '\0' && errno == 0) {
				ce_buffer_jump_line(ce_buffer_active(), linenr);
				ce_buffer_activate(buf);
				editor_cmd_normal_mode();
				return;
			}
		}

		switch (cmd[1]) {
		case 'q':
			force = cmd[2] == '!';
			editor_cmd_quit(force);
			break;
		case 'w':
			force = cmd[2] == '!';
			if (ce_buffer_save_active(force) == -1) {
				ce_editor_message("failed to save: %s",
				    ce_buffer_strerror());
			}
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
	case 0x7f:
		if (buf->length <= 1) {
			editor_cmd_normal_mode();
			break;
		}
		buf->length--;
		buf->column--;
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
editor_cmdbuf_search(struct cebuf *buf, char key)
{
	const char		*cmd;

	switch (key) {
	case '\n':
		ce_buffer_restore();
		cmd = ce_buffer_as_string(buf);
		if (strlen(cmd) > 1) {
			free(search);
			if ((search = strdup(cmd + 1)) == NULL)
				fatal("%s: strdup: %s", __func__, errno_s);
			ce_buffer_search(ce_buffer_active(), search,
			    CE_BUFFER_SEARCH_NORMAL);
		}
		ce_buffer_activate(buf);
		editor_cmd_normal_mode();
		break;
	case '\b':
	case 0x7f:
		if (buf->length <= 1) {
			editor_cmd_normal_mode();
			break;
		}
		buf->length--;
		buf->column--;
		ce_term_setpos(buf->orig_line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		break;
	default:
		if (isprint((unsigned char)key)) {
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
		mode = CE_EDITOR_MODE_NORMAL;
		break;
	default:
		break;
	}
}

static void
editor_normal_mode_command(char key)
{
	long			num;
	const char		*str;
	struct cebuf		*buf;
	int			reset;

	if (key >= '0' && key <= '9') {
		mode = CE_EDITOR_MODE_NORMAL_CMD;
		ce_buffer_append(cmdbuf, &key, sizeof(key));
		return;
	}

	reset = 0;
	buf = ce_buffer_active();

	if (normalcmd == -1) {
		ce_buffer_append(cmdbuf, &key, sizeof(key));

		switch (key) {
		case 'd':
			normalcmd = EDITOR_COMMAND_DELETE;
			break;
		case 'y':
			normalcmd = EDITOR_COMMAND_YANK;
			break;
		case EDITOR_KEY_ESC:
			reset = 1;
			break;
		}
	} else {
		reset = 1;
		str = ce_buffer_as_string(cmdbuf);

		if (!isdigit((unsigned char)str[0]))
			str++;

		errno = 0;
		num = strtol(str, NULL, 10);
		if (errno != 0 || num == 0)
			num = 1;

		switch (normalcmd) {
		case EDITOR_COMMAND_DELETE:
			editor_cmd_delete_lines(buf, num);
			break;
		case EDITOR_COMMAND_YANK:
			editor_cmd_yank_lines(buf, num);
			break;
		}
	}

	if (normalcmd != -1)
		mode = CE_EDITOR_MODE_NORMAL_CMD;

	if (reset) {
		normalcmd = -1;
		ce_buffer_reset(cmdbuf);
		mode = CE_EDITOR_MODE_NORMAL;
	}
}

static void
editor_cmd_delete_lines(struct cebuf *buf, long num)
{
	long		i;

	ce_editor_pbuffer_reset();

	for (i = 0; i < num; i++)
		ce_buffer_delete_line(buf);

#if defined(__APPLE__)
	ce_macos_set_pasteboard_contents(pbuffer->data, pbuffer->length);
#endif
}

static void
editor_cmd_yank_lines(struct cebuf *buf, long num)
{
	struct celine	*line;
	size_t		index, idx, end;

	ce_editor_pbuffer_reset();
	index = ce_buffer_line_index(buf);

	end = index + num;
	if (end >= buf->lcnt)
		end = buf->lcnt - 1;

	for (idx = index; idx < end; idx++) {
		line = &buf->lines[idx];
		ce_editor_pbuffer_append(line->data, line->length);
	}

#if defined(__APPLE__)
	ce_macos_set_pasteboard_contents(pbuffer->data, pbuffer->length);
#endif

	ce_editor_message("yanked %zu line(s)", end - index);
}

static void
editor_cmd_quit(int force)
{
	struct cebuf		*buf;

	if ((buf = ce_buffer_first_dirty()) != NULL && force == 0) {
		ce_editor_message("buffer %s is modified", buf->name);
		ce_buffer_activate(buf);
		return;
	}

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

	mode = CE_EDITOR_MODE_BUFLIST;
}

static void
editor_cmd_search_next(void)
{
	struct cebuf	*buf = ce_buffer_active();

	if (search == NULL)
		return;

	if (ce_buffer_search(buf, search, CE_BUFFER_SEARCH_NEXT))
		ce_editor_dirty();
}

static void
editor_cmd_search_prev(void)
{
	struct cebuf	*buf = ce_buffer_active();

	if (search == NULL)
		return;

	if (ce_buffer_search(buf, search, CE_BUFFER_SEARCH_PREVIOUS))
		ce_editor_dirty();
}

static void
editor_cmd_paste(void)
{
	size_t			idx;
	const u_int8_t		*ptr;
	struct cebuf		*buf;
#if defined(__APPLE__)
	u_int8_t		*pb;
#endif

#if defined(__APPLE__)
	ce_editor_pbuffer_reset();
	ce_macos_get_pasteboard_contents(&pb, &pbuffer->length);
	pbuffer->data = pb;
#endif

	if (pbuffer->length == 0)
		return;

	buf = ce_buffer_active();
	ptr = pbuffer->data;

	mode = CE_EDITOR_MODE_INSERT;

	ce_buffer_jump_left();
	ce_buffer_move_down();
	ce_buffer_input(buf, '\n');
	ce_buffer_move_up();

	for (idx = 0; idx < pbuffer->length - 1; idx++)
		ce_buffer_input(buf, ptr[idx]);

	ce_buffer_jump_left();
	mode = CE_EDITOR_MODE_NORMAL;
}

static void
editor_cmd_insert_mode(void)
{
	mode = CE_EDITOR_MODE_INSERT;
}

static void
editor_cmd_insert_mode_append(void)
{
	ce_buffer_jump_left();
	ce_buffer_move_down();
	ce_buffer_input(ce_buffer_active(), '\n');
	ce_buffer_move_up();

	mode = CE_EDITOR_MODE_INSERT;
}

static void
editor_cmd_insert_mode_prepend(void)
{
	ce_buffer_jump_left();
	ce_buffer_input(ce_buffer_active(), '\n');
	ce_buffer_move_up();

	mode = CE_EDITOR_MODE_INSERT;
}

static void
editor_cmd_command_mode(void)
{
	free(msg.message);
	msg.message = NULL;

	editor_cmd_reset();
	ce_buffer_append(cmdbuf, ":", 1);

	cmdbuf->column++;
	cmdbuf->cb = editor_cmdbuf_input;
	cmdbuf->lines[0].length = cmdbuf->length;
	ce_buffer_line_columns(&cmdbuf->lines[0]);

	ce_buffer_activate(cmdbuf);

	mode = CE_EDITOR_MODE_COMMAND;
}

static void
editor_cmd_search_mode(void)
{
	free(msg.message);
	msg.message = NULL;

	editor_cmd_reset();
	ce_buffer_append(cmdbuf, "/", 1);

	cmdbuf->column++;
	cmdbuf->cb = editor_cmdbuf_search;
	cmdbuf->lines[0].length = cmdbuf->length;
	ce_buffer_line_columns(&cmdbuf->lines[0]);

	ce_buffer_activate(cmdbuf);

	mode = CE_EDITOR_MODE_SEARCH;
}

static void
editor_cmd_normal_mode(void)
{
	struct cebuf	*buf = ce_buffer_active();

	if (mode == CE_EDITOR_MODE_INSERT) {
		mode = CE_EDITOR_MODE_NORMAL;
		if (buf->lcnt > 0) {
			ce_buffer_constrain_cursor_column(buf);
			ce_term_setpos(buf->cursor_line, buf->column);
		}
		return;
	}

	if (mode == CE_EDITOR_MODE_COMMAND ||
	    mode == CE_EDITOR_MODE_BUFLIST ||
	    mode == CE_EDITOR_MODE_SEARCH)
		ce_buffer_restore();

	if (mode == CE_EDITOR_MODE_COMMAND || mode == CE_EDITOR_MODE_SEARCH)
		editor_cmd_reset();

	mode = CE_EDITOR_MODE_NORMAL;
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
	case '-':
	case '_':
	case '!':
		return (1);
	}

	return (0);
}

static void
editor_cmd_open_file(const char *path)
{
	if (ce_buffer_file(path) == NULL) {
		ce_editor_message("%s", ce_buffer_strerror());
		return;
	}

	ce_editor_dirty();
}
