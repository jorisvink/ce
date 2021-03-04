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

#include <sys/param.h>
#include <sys/types.h>

#include <ctype.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "ce.h"

#define CE_SPLASH_TEXT_1	"Coma Editor - by Joris Vink"
#define CE_SPLASH_TEXT_2	"joris@coders.se"

/* Show messages for 5 seconds. */
#define EDITOR_MESSAGE_DELAY	5

#define EDITOR_WORD_ERASE	0x17
#define EDITOR_KEY_ESC		0x1b
#define EDITOR_KEY_UP		0xfa
#define EDITOR_KEY_DOWN		0xfb
#define EDITOR_KEY_LEFT		0xfc
#define EDITOR_KEY_RIGHT	0xfd

#define EDITOR_COMMAND_DELETE		1
#define EDITOR_COMMAND_YANK		2
#define EDITOR_COMMAND_WORD_NEXT	3
#define EDITOR_COMMAND_WORD_PREV	4
#define EDITOR_COMMAND_MARK_SET		5
#define EDITOR_COMMAND_MARK_JMP		6
#define EDITOR_COMMAND_ALTER		7

#define KEY_MAP_LEN(x)		((sizeof(x) / sizeof(x[0])))

struct keymap {
	u_int8_t	key;
	void		(*command)(void);
};

static void	editor_signal(int);
static void	editor_resume(void);
static void	editor_signal_setup(void);
static void	editor_process_input(int);
static u_int8_t	editor_process_escape(void);
static int	editor_allowed_command_key(char);
static ssize_t	editor_read(int, void *, size_t, int);
static void	editor_yank_lines(struct cebuf *, size_t, size_t, int);

static void	editor_draw_status(void);

static void	editor_cmd_quit(int);
static void	editor_cmd_reset(void);
static void	editor_cmd_suspend(void);
static void	editor_cmd_paste(void);
static void	editor_cmd_word_erase(void);
static void	editor_cmd_search_next(void);
static void	editor_cmd_search_prev(void);
static void	editor_cmd_search_word(void);
static void	editor_cmd_buffer_list(void);

static void	editor_cmd_directory_list(void);
static void	editor_cmd_directory_list_pwd(void);
static void	editor_directory_list(const char *);

static void	editor_cmd_open_file(const char *);

static void	editor_cmd_command_mode(void);
static void	editor_cmd_search_mode(void);
static void	editor_cmd_normal_mode(void);
static void	editor_cmd_range(struct cebuf *,
		    void (*cb)(struct cebuf *, size_t, size_t, int));

static void	editor_cmd_change_string(struct cebuf *);
static void	editor_cmd_word_prev(struct cebuf *, long);
static void	editor_cmd_word_next(struct cebuf *, long);
static void	editor_cmd_yank_lines(struct cebuf *, long);
static void	editor_cmd_delete_words(struct cebuf *, long);
static void	editor_cmd_delete_lines(struct cebuf *, long);

static void	editor_cmd_select_mode(void);

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
	{ 'C',			ce_buffer_center },

	{ '$',			ce_buffer_jump_right },
	{ '0',			ce_buffer_jump_left },
	{ 'G',			ce_buffer_jump_down },
	{ 'J',			ce_buffer_join_line },

	{ 0x06,			ce_buffer_page_down },
	{ 0x02,			ce_buffer_page_up },

	{ 'x',			ce_buffer_delete_character},

	{ 'p',			editor_cmd_paste },
	{ 'n',			editor_cmd_search_next },
	{ 'N',			editor_cmd_search_prev },
	{ 0x23,			editor_cmd_search_word },

	{ 'i',			editor_cmd_insert_mode },
	{ 'o',			editor_cmd_insert_mode_append },
	{ 'O',			editor_cmd_insert_mode_prepend },

	{ 's',			editor_cmd_select_mode },

	{ ':',			editor_cmd_command_mode },
	{ '/',			editor_cmd_search_mode },

	{ 0x04,			editor_cmd_directory_list },
	{ 0x07,			editor_cmd_directory_list_pwd },

	{ 0x12,			editor_cmd_buffer_list },
	{ 0x1a,			editor_cmd_suspend },
};

static struct keymap insert_map[] = {
	{ EDITOR_KEY_UP,	ce_buffer_move_up },
	{ EDITOR_KEY_DOWN,	ce_buffer_move_down },
	{ EDITOR_KEY_RIGHT,	ce_buffer_move_right },
	{ EDITOR_KEY_LEFT,	ce_buffer_move_left },
	{ EDITOR_WORD_ERASE,	editor_cmd_word_erase },

	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },

};

static struct keymap command_map[] = {
	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },
};

static struct keymap buflist_map[] = {
	{ 'k',			ce_buffer_move_up },
	{ 'j',			ce_buffer_move_down },
	{ 'G',			ce_buffer_jump_down },
	{ 'n',			editor_cmd_search_next },
	{ 'N',			editor_cmd_search_prev },
	{ 0x06,			ce_buffer_page_down },
	{ 0x02,			ce_buffer_page_up },
	{ '/',			editor_cmd_search_mode },
	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },
};

static struct keymap select_map[] = {
	{ 'k',			ce_buffer_move_up },
	{ 'j',			ce_buffer_move_down },
	{ 'l',			ce_buffer_move_right },
	{ 'h',			ce_buffer_move_left },
	{ 'C',			ce_buffer_center },
	{ '$',			ce_buffer_jump_right },
	{ '0',			ce_buffer_jump_left },
	{ 'G',			ce_buffer_jump_down },
	{ 'J',			ce_buffer_join_line },
	{ 0x06,			ce_buffer_page_down },
	{ 0x02,			ce_buffer_page_up },
	{ '/',			editor_cmd_search_mode },
	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },
};

static struct {
	const struct keymap	*map;
	size_t			maplen;
} keymaps[] = {
	{ normal_map, KEY_MAP_LEN(normal_map) },
	{ insert_map, KEY_MAP_LEN(insert_map) },
	{ command_map, KEY_MAP_LEN(command_map) },
	{ buflist_map, KEY_MAP_LEN(buflist_map) },
	{ command_map, KEY_MAP_LEN(command_map) },
	{ buflist_map, KEY_MAP_LEN(buflist_map) },
	{ select_map, KEY_MAP_LEN(select_map) },
};

static struct {
	char			*message;
	time_t			when;
} msg;

static struct {
	int			act;
	size_t			end;
	size_t			start;
} range;

static int			quit = 0;
static int			dirty = 1;
static int			splash = 0;
static int			pasting = 0;
static volatile sig_atomic_t	sig_recv = -1;
static int			normalcmd = -1;
static char			*search = NULL;
static struct cebuf		*cmdbuf = NULL;
static struct cebuf		*buflist = NULL;
static struct cebuf		*pbuffer = NULL;
static int			mode = CE_EDITOR_MODE_NORMAL;
static int			lastmode = CE_EDITOR_MODE_NORMAL;

void
ce_editor_init(void)
{
	memset(&msg, 0, sizeof(msg));
	memset(&range, 0, sizeof(range));

	editor_signal_setup();
}

void
ce_editor_loop(void)
{
	pbuffer = ce_buffer_internal("<pb>");
	ce_buffer_reset(pbuffer);

	cmdbuf = ce_buffer_internal("<cmd>");
	ce_buffer_reset(cmdbuf);

	cmdbuf->cb = editor_cmdbuf_input;
	cmdbuf->line = ce_term_height();
	cmdbuf->orig_line = ce_term_height();

	buflist = ce_buffer_internal("<buflist>");
	buflist->cb = editor_buflist_input;

	while (!quit)
		ce_editor_tick(-1);

	free(search);
	free(msg.message);
}

void
ce_editor_tick(int delay)
{
	struct timespec		ts;
	struct cemark		tmp;
	struct cebuf		*buf;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	if (sig_recv != -1) {
		switch (sig_recv) {
		case SIGQUIT:
		case SIGTERM:
			quit = 1;
			return;
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

	buf = ce_buffer_active();

	if (mode == CE_EDITOR_MODE_SELECT) {
		tmp.line = ce_buffer_line_index(buf);
		tmp.off = buf->loff;
		tmp.set = 1;

		if (tmp.line < buf->selmark.line) {
			buf->selend = buf->selmark;
			buf->selstart = tmp;
		} else {
			if (buf->selstart.line != buf->selmark.line &&
			    buf->selstart.off != buf->selmark.off)
				buf->selstart = buf->selmark;
			buf->selend = tmp;
		}

		ce_debug("selection from %zu.%zu -> %zu.%zu",
		    buf->selstart.line, buf->selstart.off,
		    buf->selend.line, buf->selend.off);

		/* for now XXX */
		dirty = 1;
	}

	if (dirty) {
		if (mode == CE_EDITOR_MODE_SEARCH &&
		    (lastmode == CE_EDITOR_MODE_DIRLIST ||
		    lastmode == CE_EDITOR_MODE_BUFLIST)) {
			ce_term_writestr(TERM_SEQUENCE_CLEAR_ONLY);
			ce_buffer_map(buflist);
		} else if (buf != cmdbuf) {
			ce_term_writestr(TERM_SEQUENCE_CLEAR_ONLY);
			ce_buffer_map(buf);
		}

		dirty = 0;
	}

	if (buf == cmdbuf)
		ce_buffer_map(buf);

	if (splash) {
		ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);
		ce_term_setpos(ce_term_height() * 0.45,
		    (ce_term_width() / 2) -
		    (sizeof(CE_SPLASH_TEXT_1) - 1) / 2);
		ce_term_writestr(CE_SPLASH_TEXT_1);
		ce_term_setpos((ce_term_height() * 0.45) + 2,
		    (ce_term_width() / 2) -
		    (sizeof(CE_SPLASH_TEXT_2) - 1) / 2);
		ce_term_writestr(CE_SPLASH_TEXT_2);
		ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);
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
	editor_process_input(delay);

	if (splash) {
		dirty = 1;
		splash = 0;
	}
}

void
ce_editor_dirty(void)
{
	dirty = 1;
}

void
ce_editor_show_splash(void)
{
	splash = 1;
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
	int			len;
	va_list			args;
	size_t			off, width;

	free(msg.message);

	va_start(args, fmt);
	if ((len = vasprintf(&msg.message, fmt, args)) == -1)
		fatal("%s: vasprintf: %s", __func__, errno_s);
	va_end(args);

	width = ce_term_width() - 8;

	if ((size_t)len > width) {
		off = len - width;
		memmove(&msg.message[0], &msg.message[off], (len - off) + 1);
		msg.message[0] = '>';
	}

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
	ce_buffer_append(pbuffer, data, len);
}

void
ce_editor_pbuffer_sync(void)
{
#if defined(__APPLE__)
	ce_macos_set_pasteboard_contents(pbuffer->data, pbuffer->length);
#endif
}

int
ce_editor_word_byte(u_int8_t byte)
{
	if (isspace(byte))
		return (0);

	if (ce_editor_word_separator(byte))
		return (0);

	return (1);
}

int
ce_editor_word_separator(u_int8_t byte)
{
	if (isspace(byte))
		return (1);

	switch (byte) {
	case '(':
	case ')':
	case '{':
	case '}':
	case '\t':
	case '\n':
	case '[':
	case ']':
	case ':':
	case ';':
	case ',':
	case '-':
	case '=':
	case '*':
	case '.':
	case '@':
	case '<':
	case '>':
	case '\'':
	case '"':
	case '&':
	case '/':
		return (1);
	default:
		break;
	}

	return (0);
}

int
ce_editor_pasting(void)
{
	return (pasting);
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
editor_process_input(int delay)
{
	size_t			idx;
	u_int8_t		key;
	struct cebuf		*curbuf = ce_buffer_active();

	if (mode >= CE_EDITOR_MODE_MAX)
		fatal("%s: mode %d invalid", __func__, mode);

	if (editor_read(STDIN_FILENO, &key, sizeof(key), delay) == 0)
		return;

	ce_debug("0x%02x", key);

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

	switch (mode) {
	case CE_EDITOR_MODE_NORMAL:
		editor_normal_mode_command(key);
		return;
	case CE_EDITOR_MODE_SELECT:
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
	const u_int8_t		*ptr;
	struct cebuf		*curbuf;
	int			flen, slen, llen;
	size_t			cmdoff, width, pc;
	const char		*isdirty, *filemode, *modestr;
	char			fline[1024], sline[128], lline[128];

	isdirty = "";
	filemode = "";
	modestr = NULL;
	curbuf = ce_buffer_active();

	switch (mode) {
	case CE_EDITOR_MODE_COMMAND:
	case CE_EDITOR_MODE_SEARCH:
		if (lastmode == CE_EDITOR_MODE_DIRLIST ||
		    lastmode == CE_EDITOR_MODE_BUFLIST) {
			modestr = "- SEARCH -";
			curbuf = buflist;
			break;
		}
		return;
	case CE_EDITOR_MODE_NORMAL:
	case CE_EDITOR_MODE_BUFLIST:
	case CE_EDITOR_MODE_DIRLIST:
	case CE_EDITOR_MODE_NORMAL_CMD:
		modestr = "";
		break;
	case CE_EDITOR_MODE_SELECT:
		modestr = "- SELECT -";
		break;
	case CE_EDITOR_MODE_INSERT:
		modestr = "- INSERT -";
		break;
	default:
		fatal("%s: unknown mode %d", __func__, mode);
	}

	if (curbuf->flags & CE_BUFFER_DIRTY)
		isdirty = " [+]";

	if (curbuf->flags & CE_BUFFER_RO)
		filemode = "r-";
	else
		filemode = "rw";

	if (curbuf->top == 0) {
		llen = snprintf(lline, sizeof(lline), "%zuL [Top]",
		    curbuf->lcnt);
	} else if (curbuf->lcnt - (curbuf->top + curbuf->line) <
	    ((ce_term_height() - 2) / 2)) {
		llen = snprintf(lline, sizeof(lline), "%zuL [Bot]",
		    curbuf->lcnt);
	} else {
		pc = ((curbuf->top + curbuf->line) / (float)curbuf->lcnt) * 100;
		llen = snprintf(lline, sizeof(lline), "%zuL [%zu%%]",
		    curbuf->lcnt, pc);
	}

	if (llen == -1 || (size_t)llen >= sizeof(lline))
		fatal("failed to create status percent line");

	flen = snprintf(fline, sizeof(fline), "%s%s", curbuf->name, isdirty);
	if (flen == -1)
		fatal("failed to create status file line");

	if (mode == CE_EDITOR_MODE_SELECT) {
		slen = snprintf(sline, sizeof(sline),
		    "[%s] %zu,%zu-%zu %s %zu.%zu <> %zu.%zu", filemode,
		    curbuf->top + curbuf->line, curbuf->loff,
		    curbuf->column, modestr,
		    curbuf->selstart.line, curbuf->selstart.off,
		    curbuf->selend.line, curbuf->selend.off);
	} else {
		slen = snprintf(sline, sizeof(sline),
		    "[%s] %zu,%zu-%zu %s", filemode,
		    curbuf->top + curbuf->line, curbuf->loff,
		    curbuf->column, modestr);
	}

	if (slen == -1 || (size_t)slen >= sizeof(sline))
		fatal("failed to create status line");

	width = (ce_term_width() - 1) - slen - llen;
	if ((size_t)flen > width) {
		cmdoff = flen - width;
		fline[cmdoff] = '>';
	} else {
		cmdoff = 0;
	}

	ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);
	ce_term_setpos(ce_term_height() - 1, TERM_CURSOR_MIN);

	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
	ce_term_color(TERM_COLOR_CYAN + TERM_COLOR_FG);
	ce_term_color(TERM_COLOR_BLACK + TERM_COLOR_BG);
	ce_term_writestr(TERM_SEQUENCE_ATTR_REVERSE);
	ce_term_writef("%s %s", &fline[cmdoff], sline);

	if ((size_t)(slen + flen) < (ce_term_width() - llen)) {
		width = (ce_term_width() - llen) - (slen + flen);
		while (width > 1) {
			ce_term_writestr(" ");
			width--;
		}
	}

	ce_term_setpos(ce_term_height() - 1, ce_term_width() - llen + 1);
	ce_term_writef("%s", lline);

	cmdoff = ce_term_width() * 0.75f;

	ce_term_attr_off();
	ce_term_setpos(ce_term_height(), cmdoff);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);

	if (mode == CE_EDITOR_MODE_NORMAL_CMD) {
		width = ce_term_width() - (cmdoff + 10);

		if (cmdbuf->length > width)
			cmdoff = cmdbuf->length - width - 1;
		else
			cmdoff = 0;

		ptr = cmdbuf->data;
		ce_term_write(&ptr[cmdoff], cmdbuf->length - cmdoff);
	}

	ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);
}

static void
editor_cmdbuf_input(struct cebuf *buf, char key)
{
	char			*ep;
	int			force;
	long			linenr;
	const char		*cmd, *path;

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
			if (strlen(cmd) > 3)
				path = &cmd[3];
			else
				path = NULL;
			if (ce_buffer_save_active(force, path) == -1) {
				ce_editor_message("failed to save: %s",
				    ce_buffer_strerror());
			}
			break;
		case 'e':
			if (strlen(cmd) > 3)
				editor_cmd_open_file(&cmd[3]);
			break;
		case 'l':
			if (strlen(cmd) > 3) {
				editor_directory_list(&cmd[3]);
				return;
			}
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
			if (mode == CE_EDITOR_MODE_SEARCH &&
			    lastmode == CE_EDITOR_MODE_DIRLIST)
				ce_dirlist_narrow(buflist, NULL);
			editor_cmd_normal_mode();
			break;
		}
		buf->length--;
		buf->column--;
		ce_term_setpos(buf->orig_line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);

		if (mode == CE_EDITOR_MODE_SEARCH &&
		    lastmode == CE_EDITOR_MODE_DIRLIST) {
			cmd = ce_buffer_as_string(buf);
			ce_dirlist_narrow(buflist, &cmd[1]);
			buf->length--;
		}
		break;
	default:
		if (isprint((unsigned char)key)) {
			ce_buffer_append(buf, &key, sizeof(key));
			buf->column++;
		}

		if (mode == CE_EDITOR_MODE_SEARCH &&
		    lastmode == CE_EDITOR_MODE_DIRLIST) {
			cmd = ce_buffer_as_string(buf);
			ce_dirlist_narrow(buflist, &cmd[1]);
			buf->length--;
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
	const char	*path;

	if (mode == CE_EDITOR_MODE_BUFLIST) {
		if (key >= '0' && key <= '9') {
			ce_buffer_activate_index(key - '0');
			lastmode = mode;
			mode = CE_EDITOR_MODE_NORMAL;
			return;
		}
	}

	switch (key) {
	case '\n':
		index = buf->top + (buf->line - 1);

		switch (mode) {
		case CE_EDITOR_MODE_BUFLIST:
			ce_buffer_activate_index(index);
			break;
		case CE_EDITOR_MODE_DIRLIST:
			ce_buffer_restore();
			path = ce_dirlist_select(buf, index);
			if (path != NULL)
				editor_cmd_open_file(path);
			ce_dirlist_close(buf);
			break;
		}

		lastmode = mode;
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
	int			reset, range_reset, next_mode;

	if (normalcmd == -1 ||
	    (normalcmd != EDITOR_COMMAND_MARK_SET &&
	    normalcmd != EDITOR_COMMAND_MARK_JMP)) {
		if (key >= '0' && key <= '9') {
			lastmode = mode;
			mode = CE_EDITOR_MODE_NORMAL_CMD;
			ce_buffer_append(cmdbuf, &key, sizeof(key));
			return;
		}
	}

	reset = 0;
	range_reset = 0;
	buf = ce_buffer_active();
	next_mode = CE_EDITOR_MODE_NORMAL;

	if (normalcmd == -1) {
		ce_buffer_append(cmdbuf, &key, sizeof(key));

		switch (key) {
		case 'a':
			normalcmd = EDITOR_COMMAND_ALTER;
			break;
		case 'b':
			normalcmd = EDITOR_COMMAND_WORD_PREV;
			goto direct;
		case 'd':
			normalcmd = EDITOR_COMMAND_DELETE;
			break;
		case 'm':
			normalcmd = EDITOR_COMMAND_MARK_SET;
			break;
		case '\'':
			normalcmd = EDITOR_COMMAND_MARK_JMP;
			break;
		case 'y':
		case 'c':
			normalcmd = EDITOR_COMMAND_YANK;
			break;
		case 'w':
			normalcmd = EDITOR_COMMAND_WORD_NEXT;
			goto direct;
		default:
			reset = 1;
			range_reset = 1;
			break;
		}
	} else {
direct:
		reset = 1;
		str = ce_buffer_as_string(cmdbuf);

		if (!isdigit((unsigned char)str[0]))
			str++;

		errno = 0;
		num = strtol(str, NULL, 10);
		if (errno != 0 || num == 0)
			num = 1;

		switch (normalcmd) {
		case EDITOR_COMMAND_MARK_SET:
			if (key >= CE_MARK_MIN && key <= CE_MARK_MAX)
				ce_buffer_mark_set(buf, key);
			break;
		case EDITOR_COMMAND_MARK_JMP:
			if (key == CE_MARK_PREVIOUS ||
			    (key >= CE_MARK_MIN && key <= CE_MARK_MAX))
				ce_buffer_mark_jump(buf, key);
			break;
		case EDITOR_COMMAND_ALTER:
			switch (key) {
			case 'i':
				editor_cmd_change_string(buf);
				next_mode = CE_EDITOR_MODE_INSERT;
				break;
			}
			break;
		case EDITOR_COMMAND_DELETE:
			switch (key) {
			case 'i':
				editor_cmd_change_string(buf);
				break;
			case 's':
				if (buf->lcnt == 0)
					break;
				range.act = EDITOR_COMMAND_DELETE;
				range.start = ce_buffer_line_index(buf);
				ce_editor_message("delete range start @ %zu",
				    range.start);
				break;
			case 'e':
				if (range.act != EDITOR_COMMAND_DELETE)
					break;
				range.end = ce_buffer_line_index(buf);
				editor_cmd_range(buf, ce_buffer_delete_lines);
				break;
			case 'd':
				editor_cmd_delete_lines(buf, num);
				break;
			case 'w':
				editor_cmd_delete_words(buf, num);
				break;
			}
			break;
		case EDITOR_COMMAND_YANK:
			switch (key) {
			case 's':
				if (buf->lcnt == 0)
					break;
				range.act = EDITOR_COMMAND_YANK;
				range.start = ce_buffer_line_index(buf);
				ce_editor_message("yank range start @ %zu",
				    range.start);
				break;
			case 'e':
				if (range.act != EDITOR_COMMAND_YANK)
					break;
				range.end = ce_buffer_line_index(buf);
				editor_cmd_range(buf, editor_yank_lines);
				break;
			case 'y':
				editor_cmd_yank_lines(buf, num);
				break;
			}
			break;
		case EDITOR_COMMAND_WORD_NEXT:
			editor_cmd_word_next(buf, num);
			break;
		case EDITOR_COMMAND_WORD_PREV:
			editor_cmd_word_prev(buf, num);
			break;
		}
	}

	if (normalcmd != -1) {
		lastmode = mode;
		mode = CE_EDITOR_MODE_NORMAL_CMD;
	}

	if (reset) {
		normalcmd = -1;
		ce_buffer_reset(cmdbuf);

		if (next_mode == CE_EDITOR_MODE_INSERT) {
			editor_cmd_insert_mode();
		} else {
			lastmode = mode;
			mode = next_mode;
		}
	}

	if (range_reset && range.act != 0) {
		ce_editor_message("range command aborted");
		memset(&range, 0, sizeof(range));
	}
}

static void
editor_cmd_range(struct cebuf *buf,
    void (*cb)(struct cebuf *, size_t, size_t, int))
{
	int		rev;
	size_t		start, end;

	if (range.end < range.start) {
		rev = 1;
		start = range.end;
		end = range.start;
	} else {
		rev = 0;
		start = range.start;
		end = range.end;
	}

	if (end > buf->lcnt - 1)
		end = buf->lcnt - 1;

	if (start >= end)
		return;

	ce_editor_pbuffer_reset();
	ce_buffer_mark_last(buf, start);

	cb(buf, start, end, rev);
	memset(&range, 0, sizeof(range));

	ce_editor_pbuffer_sync();
}

static void
editor_cmd_change_string(struct cebuf *buf)
{
	u_int8_t		key;

	if (editor_read(STDIN_FILENO, &key, sizeof(key), -1) == 0)
		return;

	switch (key) {
	case '\'':
	case '"':
		break;
	default:
		return;
	}

	ce_buffer_delete_inside_string(buf, key);
}

static void
editor_cmd_delete_lines(struct cebuf *buf, long end)
{
	size_t		start;

	if (buf->lcnt == 0)
		return;

	end = end - 1;
	ce_editor_pbuffer_reset();

	start = ce_buffer_line_index(buf);

	ce_buffer_mark_last(buf, start);
	ce_buffer_delete_lines(buf, start, start + end, 1);

	ce_editor_pbuffer_sync();
}

static void
editor_cmd_delete_words(struct cebuf *buf, long num)
{
	long		i;

	ce_editor_pbuffer_reset();

	for (i = 0; i < num; i++)
		ce_buffer_word_delete(buf);

	ce_editor_pbuffer_sync();
}

static void
editor_cmd_yank_lines(struct cebuf *buf, long num)
{
	size_t		index, end;

	num--;
	index = ce_buffer_line_index(buf);
	ce_buffer_mark_last(buf, ce_buffer_line_index(buf) + 1);

	end = index + num;
	if (end > buf->lcnt - 1)
		end = buf->lcnt - 1;

	ce_editor_pbuffer_reset();

	editor_yank_lines(buf, index, end, 0);

	ce_editor_pbuffer_sync();
}

static void
editor_yank_lines(struct cebuf *buf, size_t start, size_t end, int rev)
{
	size_t		idx;
	struct celine	*line;

	for (idx = start; idx <= end; idx++) {
		line = &buf->lines[idx];
		ce_editor_pbuffer_append(line->data, line->length);
	}

	ce_editor_message("yanked %zu line(s)", (end - start) + 1);
}

static void
editor_cmd_word_next(struct cebuf *buf, long num)
{
	long		i;

	for (i = 0; i < num; i++)
		ce_buffer_word_next(buf);
}

static void
editor_cmd_word_prev(struct cebuf *buf, long num)
{
	long		i;

	for (i = 0; i < num; i++)
		ce_buffer_word_prev(buf);
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
	ce_buffer_setname(buflist, "<buffers>");

	ce_buffer_list(buflist);
	ce_buffer_activate(buflist);
	ce_buffer_jump_left();

	lastmode = mode;
	mode = CE_EDITOR_MODE_BUFLIST;
}

static void
editor_cmd_directory_list_pwd(void)
{
	editor_directory_list(".");
}

static void
editor_cmd_directory_list(void)
{
	struct cebuf	*buf;
	const char	*path;
	char		*cp, *dname, pwd[PATH_MAX];

	cp = NULL;
	path = ".";
	buf = ce_buffer_active();

	if (buf->path != NULL) {
		if ((cp = strdup(buf->path)) == NULL)
			fatal("%s: strdup of '%s' failed", __func__, buf->path);

		if ((dname = dirname(cp)) == NULL)
			fatal("%s: dirname: %s", __func__, errno_s);

		path = dname;

		if (getcwd(pwd, sizeof(pwd)) == NULL)
			fatal("%s: getcwd: %s", __func__, errno_s);

		if (!strcmp(pwd, path))
			path = ".";
	}

	editor_directory_list(path);

	if (cp)
		free(cp);
}

static void
editor_directory_list(const char *path)
{
	ce_dirlist_path(buflist, path);

	ce_buffer_activate(buflist);
	ce_buffer_jump_left();

	lastmode = mode;
	mode = CE_EDITOR_MODE_DIRLIST;
}

static void
editor_cmd_word_erase(void)
{
	if (mode != CE_EDITOR_MODE_INSERT)
		fatal("ce not in insert mode");

	ce_buffer_word_erase(ce_buffer_active());

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
editor_cmd_search_word(void)
{
	struct cebuf		*buf;
	const u_int8_t		*word;
	size_t			length;

	buf = ce_buffer_active();

	if (ce_buffer_word_cursor(buf, &word, &length) == -1)
		return;

	free(search);

	if ((search = malloc(length + 1)) == NULL)
		fatal("%s: malloc: %s", __func__, errno_s);

	memcpy(search, word, length);
	search[length] = '\0';

	if (ce_buffer_search(buf, search, CE_BUFFER_SEARCH_PREVIOUS))
		ce_editor_dirty();
}

static void
editor_cmd_paste(void)
{
	const u_int8_t		*ptr;
	struct cebuf		*buf;
	size_t			idx, len;
#if defined(__APPLE__)
	size_t			old_len;
	u_int8_t		*pb, *old;
#endif

#if defined(__APPLE__)
	ce_editor_pbuffer_reset();
	old = pbuffer->data;
	old_len = pbuffer->length;
	ce_macos_get_pasteboard_contents(&pb, &pbuffer->length);
	pbuffer->data = pb;
#endif

	if (pbuffer->length == 0)
		return;

	buf = ce_buffer_active();
	ptr = pbuffer->data;

	editor_cmd_insert_mode();

	if (ptr[pbuffer->length - 1] == '\n') {
		ce_buffer_jump_left();
		ce_buffer_move_down();
		ce_buffer_input(buf, '\n');
		ce_buffer_move_up();
		len = pbuffer->length - 1;
	} else {
		len = pbuffer->length;
	}

	pasting = 1;

	for (idx = 0; idx < len; idx++)
		ce_buffer_input(buf, ptr[idx]);

	pasting = 0;

	ce_editor_dirty();

	if (ptr[len] == '\n')
		ce_buffer_jump_left();

	lastmode = mode;
	mode = CE_EDITOR_MODE_NORMAL;

#if defined(__APPLE__)
	pbuffer->data = old;
	pbuffer->length = old_len;
#endif
}

static void
editor_cmd_select_mode(void)
{
	struct cebuf		*buf = ce_buffer_active();

	lastmode = mode;
	mode = CE_EDITOR_MODE_SELECT;

	buf->selstart.set = 1;
	buf->selstart.off = buf->loff;
	buf->selstart.line = ce_buffer_line_index(buf);

	buf->selend = buf->selstart;
	buf->selmark = buf->selstart;
}

static void
editor_cmd_insert_mode(void)
{
	struct cebuf		*buf = ce_buffer_active();

	lastmode = mode;
	mode = CE_EDITOR_MODE_INSERT;

	if (buf->lcnt > 0)
		ce_buffer_mark_last(buf, ce_buffer_line_index(buf) + 1);
}

static void
editor_cmd_insert_mode_append(void)
{
	struct cebuf		*buf = ce_buffer_active();

	if (buf->lcnt == 0 || buf->line == buf->lcnt) {
		editor_cmd_insert_mode();
		ce_buffer_jump_right();
		ce_buffer_input(buf, '\n');
	} else {
		ce_buffer_jump_left();
		ce_buffer_move_down();
		ce_buffer_input(buf, '\n');
		ce_buffer_move_up();
		editor_cmd_insert_mode();
	}
}

static void
editor_cmd_insert_mode_prepend(void)
{
	ce_buffer_jump_left();
	ce_buffer_input(ce_buffer_active(), '\n');
	ce_buffer_move_up();

	editor_cmd_insert_mode();
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

	lastmode = mode;
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

	lastmode = mode;
	mode = CE_EDITOR_MODE_SEARCH;
}

static void
editor_cmd_normal_mode(void)
{
	struct cebuf	*buf = ce_buffer_active();

	if (mode == CE_EDITOR_MODE_INSERT) {
		lastmode = mode;
		mode = CE_EDITOR_MODE_NORMAL;
		if (buf->lcnt > 0) {
			ce_buffer_constrain_cursor_column(buf);
			ce_term_setpos(buf->cursor_line, buf->column);
		}
		return;
	}

	if (mode == CE_EDITOR_MODE_DIRLIST)
		ce_dirlist_close(buflist);

	if (mode == CE_EDITOR_MODE_SELECT)
		ce_editor_dirty();

	if (mode == CE_EDITOR_MODE_COMMAND ||
	    mode == CE_EDITOR_MODE_BUFLIST ||
	    mode == CE_EDITOR_MODE_DIRLIST ||
	    mode == CE_EDITOR_MODE_SEARCH)
		ce_buffer_restore();

	if (mode == CE_EDITOR_MODE_COMMAND || mode == CE_EDITOR_MODE_SEARCH)
		editor_cmd_reset();

	if (mode != lastmode && (lastmode == CE_EDITOR_MODE_BUFLIST ||
	    lastmode == CE_EDITOR_MODE_DIRLIST)) {
		mode = lastmode;
	} else {
		lastmode = mode;
		mode = CE_EDITOR_MODE_NORMAL;
	}
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

	free(msg.message);
	msg.message = NULL;

	ce_editor_dirty();
}

