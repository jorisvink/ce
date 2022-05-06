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
#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "ce.h"

#define CE_SPLASH_TEXT_1	"Coma Editor"
#define CE_SPLASH_TEXT_2	"joris@coders.se"

#define CE_UTF8_U2015		"\xe2\x80\x95"

/* Show messages for 5 seconds. */
#define EDITOR_MESSAGE_DELAY	5

#define EDITOR_CMD_BUFLIST	0x12
#define EDITOR_CMD_PASTE	0x16
#define EDITOR_CMD_HIST_PREV	0x10
#define EDITOR_CMD_HIST_NEXT	0x0e

#define EDITOR_WORD_ERASE	0x17
#define EDITOR_KEY_ESC		0x1b
#define EDITOR_KEY_UP		0xfa
#define EDITOR_KEY_DOWN		0xfb
#define EDITOR_KEY_LEFT		0xfc
#define EDITOR_KEY_RIGHT	0xfd

/* UTF-8 § (U+00A7). */
#define EDITOR_KEY_UTF8_PREFIX	0xc2
#define EDITOR_KEY_UTF8_CONSOLE	0xa7

#define EDITOR_COMMAND_DELETE		1
#define EDITOR_COMMAND_YANK		2
#define EDITOR_COMMAND_WORD_NEXT	3
#define EDITOR_COMMAND_WORD_PREV	4
#define EDITOR_COMMAND_MARK_SET		5
#define EDITOR_COMMAND_MARK_JMP		6
#define EDITOR_COMMAND_ALTER		7
#define EDITOR_COMMAND_PROCESS		8
#define EDITOR_COMMAND_ALIGN		9

#define KEY_MAP_LEN(x)		((sizeof(x) / sizeof(x[0])))

struct keymap {
	u_int8_t	key;
	void		(*command)(void);
};

static void	editor_signal(int);
static void	editor_resume(void);
static void	editor_event_wait(void);
static void	editor_read_input(void);
static void	editor_signal_setup(void);
static void	editor_consume_input(void);
static u_int8_t	editor_process_escape(void);
static void	editor_preset_cmd(const char *);
static int	editor_allowed_command_key(char);
static int	editor_get_input(u_int8_t *, int);
static int	editor_cmd_can_autocomplete(void);
static void	editor_shellbuf_close(struct cebuf *);
static void	editor_autocomplete_path(struct cebuf *);
static void	editor_shellbuf_new(const char *, struct cebuf **);
static void	editor_yank_lines(struct cebuf *, size_t, size_t, int);

static void	editor_draw_status(void);
static void	editor_draw_cmdbuf(void);
static void	editor_draw_suggestions(int);
static void	editor_draw_message(struct timespec *);

static void	editor_cmd_quit(int);
static void	editor_cmd_grep(void);
static void	editor_cmd_find(void);
static void	editor_cmd_exec(void);
static void	editor_cmd_reset(void);
static void	editor_cmd_paste(void);
static void	editor_cmd_suspend(void);
static void	editor_cmd_word_erase(void);
static void	editor_cmd_search_next(void);
static void	editor_cmd_search_prev(void);
static void	editor_cmd_search_word(void);
static void	editor_cmd_buffer_list(void);
static void	editor_cmd_buffer_next(void);
static void	editor_cmd_buffer_prev(void);

static void	editor_cmd_history_prev(void);
static void	editor_cmd_history_next(void);
static void	editor_cmd_history_cycle(int, int);
static void	editor_cmd_suggestions(struct cehist *, size_t);

static void	editor_cmd_directory_list(void);
static void	editor_directory_from_buffer(void);
static void	editor_directory_list(const char *);
static void	editor_directory_change(const char *);

static void	editor_cmd_execute(char *);
static void	editor_cmd_open_file(const char *);

static void	editor_cmd_command_mode(void);
static void	editor_cmd_search_mode(void);
static void	editor_cmd_normal_mode(void);

static void	editor_cmd_change_string(struct cebuf *);
static void	editor_cmd_word_prev(struct cebuf *, long);
static void	editor_cmd_word_next(struct cebuf *, long);
static void	editor_cmd_yank_lines(struct cebuf *, long);
static void	editor_cmd_delete_words(struct cebuf *, long);
static void	editor_cmd_delete_lines(struct cebuf *, long);

static void	editor_cmd_select_mode(void);
static void	editor_cmd_select_execute(void);
static void	editor_cmd_select_yank_delete(int);

static void	editor_cmd_insert_mode(void);
static void	editor_cmd_insert_mode_append(void);
static void	editor_cmd_insert_mode_prepend(void);

static void	editor_select_mode_command(u_int8_t);
static void	editor_normal_mode_command(u_int8_t);
static void	editor_dirlist_mode_command(u_int8_t);

static void	editor_no_input(struct cebuf *, u_int8_t);
static void	editor_cmdbuf_input(struct cebuf *, u_int8_t);
static void	editor_cmdbuf_search(struct cebuf *, u_int8_t);
static void	editor_buflist_input(struct cebuf *, u_int8_t);

static struct keymap normal_map[] = {
	{ 'k',			ce_buffer_move_up },
	{ 'j',			ce_buffer_move_down },
	{ 'l',			ce_buffer_move_right },
	{ 'h',			ce_buffer_move_left },
	{ 'C',			ce_buffer_center },
	{ 'T',			ce_buffer_top },

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

	{ 'g',			editor_cmd_grep },
	{ 'f',			editor_cmd_find },
	{ 'e',			editor_cmd_exec },
	{ ':',			editor_cmd_command_mode },
	{ '/',			editor_cmd_search_mode },

	{ 0x04,			editor_cmd_directory_list },

	{ EDITOR_CMD_BUFLIST,	editor_cmd_buffer_list },
	{ EDITOR_CMD_HIST_PREV,	editor_cmd_buffer_prev },
	{ EDITOR_CMD_HIST_NEXT,	editor_cmd_buffer_next },

	{ 0x1a,			editor_cmd_suspend },
};

static struct keymap insert_map[] = {
	{ EDITOR_CMD_PASTE,	editor_cmd_paste },
	{ EDITOR_KEY_UP,	ce_buffer_move_up },
	{ EDITOR_KEY_DOWN,	ce_buffer_move_down },
	{ EDITOR_KEY_RIGHT,	ce_buffer_move_right },
	{ EDITOR_KEY_LEFT,	ce_buffer_move_left },
	{ EDITOR_WORD_ERASE,	editor_cmd_word_erase },
	{ EDITOR_CMD_HIST_NEXT,	editor_cmd_history_next },
	{ EDITOR_CMD_HIST_PREV,	editor_cmd_history_prev },
	{ EDITOR_KEY_ESC,	editor_cmd_normal_mode },
};

static struct keymap command_map[] = {
	{ 0, NULL },
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
	{ 'n',			editor_cmd_search_next },
	{ 'N',			editor_cmd_search_prev },
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
	{ select_map, KEY_MAP_LEN(select_map) },
};

static struct {
	const char		*cmd;
	void			(*run)(const char *);
} cmdtab[] = {
	{ "cd", editor_directory_change },
	{ NULL, NULL },
};

static struct {
	char			*message;
	time_t			when;
} msg;

static struct {
	size_t			sz;
	size_t			off;
	u_int8_t		data[512];
} inq;

static int			quit = 0;
static int			dirty = 1;
static int			splash = 0;
static int			pasting = 0;
static volatile sig_atomic_t	sig_recv = -1;
static int			normalcmd = -1;
static char			*home = NULL;
static char			*curdir = NULL;
static char			*search = NULL;
static struct cebuf		*cmdbuf = NULL;
static struct cebuf		*buflist = NULL;
static struct cebuf		*pbuffer = NULL;
static struct cebuf		*suggestions = NULL;
static int			suggestions_wipe = 0;
static int			mode = CE_EDITOR_MODE_NORMAL;
static int			lastmode = CE_EDITOR_MODE_NORMAL;

void
ce_editor_init(void)
{
	struct passwd	*pw;
	char		*login, pwd[PATH_MAX];

	if ((home = getenv("HOME")) == NULL) {
		if ((login = getlogin()) == NULL)
			fatal("%s: getlogin: %s", __func__, errno_s);

		if ((pw = getpwnam(login)) == NULL)
			fatal("%s: getpwnam: %s", __func__, errno_s);

		home = pw->pw_dir;
	}

	home = ce_strdup(home);

	if (getcwd(pwd, sizeof(pwd)) == NULL)
		fatal("%s: getpwd: %s", __func__, errno_s);

	memset(&msg, 0, sizeof(msg));
	memset(&inq, 0, sizeof(inq));
	editor_directory_change(pwd);

	ce_debug("homedir is '%s'", home);
	ce_debug("curdir is '%s'", curdir);

	editor_signal_setup();
}

void
ce_editor_loop(void)
{
	struct timespec		ts;
	struct cemark		tmp;
	struct cebuf		*buf;

	pbuffer = ce_buffer_internal("<pb>");
	ce_buffer_reset(pbuffer);

	cmdbuf = ce_buffer_internal("<cmd>");
	ce_buffer_reset(cmdbuf);

	cmdbuf->cb = editor_cmdbuf_input;
	cmdbuf->line = ce_term_height();
	cmdbuf->orig_line = ce_term_height();

	buflist = ce_buffer_internal("<buflist>");
	buflist->cb = editor_buflist_input;

	suggestions = ce_buffer_internal("<suggestions>");

	while (!quit) {
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
				ce_buffer_resize();
				cmdbuf->line = ce_term_height();
				cmdbuf->orig_line = ce_term_height();
				break;
			}
			sig_recv = -1;
		}

		buf = ce_buffer_active();

		if (mode == CE_EDITOR_MODE_SELECT) {
			tmp.line = ce_buffer_line_index(buf);
			tmp.col = buf->column;
			tmp.off = buf->loff;
			tmp.set = 1;

			if (tmp.line < buf->selmark.line) {
				buf->selend = buf->selmark;
				buf->selstart = tmp;
			} else if (tmp.line == buf->selmark.line) {
				if (tmp.col <= buf->selmark.col) {
					buf->selstart.col = tmp.col;
					buf->selstart.off = tmp.off;
					buf->selend = buf->selmark;
				} else {
					buf->selend.col = tmp.col;
					buf->selend.off = tmp.off;
					buf->selstart = buf->selmark;
				}
			} else {
				if (buf->selstart.line != buf->selmark.line &&
				    buf->selstart.col != buf->selmark.col) {
					buf->selstart = buf->selmark;
				}
				buf->selend = tmp;
			}

			/* for now XXX */
			dirty = 1;
		}

		if (dirty) {
			if (mode == CE_EDITOR_MODE_SEARCH &&
			    buf->prev->buftype == CE_BUF_TYPE_DIRLIST) {
				ce_term_writestr(TERM_SEQUENCE_CLEAR_ONLY);
				ce_buffer_map(buf->prev);
			} else if (buf != cmdbuf) {
				ce_term_writestr(TERM_SEQUENCE_CLEAR_ONLY);
				ce_buffer_map(buf);
			} else if (suggestions_wipe) {
				ce_term_writestr(TERM_SEQUENCE_CLEAR_ONLY);
				ce_buffer_map(buf->prev);
			}

			dirty = 0;
			suggestions_wipe = 0;
		}

		if (buf == cmdbuf)
			editor_draw_cmdbuf();

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
		editor_draw_message(&ts);

		ce_term_flush();
		editor_event_wait();

		while (inq.off != inq.sz)
			editor_consume_input();

		if (splash) {
			dirty = 1;
			splash = 0;
		}
	}

	free(search);
	free(msg.message);
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
	va_list		args;

	va_start(args, fmt);
	ce_editor_messagev(fmt, args);
	va_end(args);
}

void
ce_editor_messagev(const char *fmt, va_list args)
{
	struct timespec		ts;
	int			len;
	size_t			off, width;

	free(msg.message);

	if ((len = vasprintf(&msg.message, fmt, args)) == -1)
		fatal("%s: vasprintf: %s", __func__, errno_s);

	width = ce_term_width() - 8;

	if ((size_t)len > width) {
		off = len - width;
		memmove(&msg.message[0], &msg.message[off], (len - off) + 1);
		msg.message[0] = '>';
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	msg.when = ts.tv_sec;
}

int
ce_editor_yesno(void (*cb)(const void *), const void *arg, const char *fmt, ...)
{
	u_int8_t	key;
	va_list		args;

	va_start(args, fmt);
	ce_editor_messagev(fmt, args);
	va_end(args);

	for (;;) {
		editor_draw_message(NULL);
		ce_term_flush();

		if (editor_get_input(&key, 1) == 0)
			return (-1);

		switch (key) {
		case 'y':
			cb(arg);
			return (0);
		case 'n':
		case EDITOR_KEY_ESC:
			free(msg.message);
			msg.message = NULL;
			return (-1);
		}
	}

	return (-1);
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

void
ce_editor_set_pasting(int val)
{
	pasting = val;
}

int
ce_editor_pasting(void)
{
	return (pasting);
}

const char *
ce_editor_fullpath(const char *path)
{
	int		len;
	static char	tmp[PATH_MAX];

	if (path[0] == '~') {
		if (strlen(path) > 1) {
			len = snprintf(tmp, sizeof(tmp), "%s%s%s",
			    home, path[1] == '/' ? "" : "/", &path[1]);
		} else {
			len = snprintf(tmp, sizeof(tmp), "%s", home);
		}
	} else {
		len = snprintf(tmp, sizeof(tmp), "%s", path);
	}

	if (len == -1 || (size_t)len >= sizeof(tmp))
		fatal("%s: failed to construct '%s'", __func__, path);

	return (tmp);
}

const char *
ce_editor_shortpath(const char *path)
{
	int		slen;
	size_t		len, plen;
	static char	tmp[PATH_MAX];

	plen = strlen(path);

	len = strlen(curdir);
	if (!strncmp(curdir, path, len)) {
		if (len == plen)
			return (path);
		return (&path[len + 1]);
	}

	len = strlen(home);
	if (!strncmp(home, path, len)) {
		slen = snprintf(tmp, sizeof(tmp), "~%s", &path[len]);
		if (slen == -1 || (size_t)slen >= sizeof(tmp))
			fatal("%s: snprintf '~%s'", __func__, &path[len]);
		return (tmp);
	}

	return (path);
}

const char *
ce_editor_pwd(void)
{
	return (curdir);
}

const char *
ce_editor_home(void)
{
	return (home);
}

void
ce_editor_settings(struct cebuf *buf)
{
	if (buf == NULL) {
		config.tab_width = CE_TAB_WIDTH_DEFAULT;
		config.tab_expand = CE_TAB_EXPAND_DEFAULT;
		return;
	}

	ce_syntax_guess(buf);

	if (ce_lame_mode()) {
		config.tab_width = 4;
		config.tab_expand = 1;
		return;
	}

	switch (buf->type) {
	case CE_FILE_TYPE_CSS:
	case CE_FILE_TYPE_HTML:
	case CE_FILE_TYPE_YAML:
	case CE_FILE_TYPE_JSON:
	case CE_FILE_TYPE_PYTHON:
		config.tab_width = 4;
		config.tab_expand = 1;
		break;
	default:
		config.tab_width = CE_TAB_WIDTH_DEFAULT;
		config.tab_expand = CE_TAB_EXPAND_DEFAULT;
		break;
	}
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
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		fatal("sigaction: %s", errno_s);
	if (sigaction(SIGWINCH, &sa, NULL) == -1)
		fatal("sigaction: %s", errno_s);

	(void)signal(SIGINT, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGPIPE, SIG_IGN);
}

static void
editor_event_wait(void)
{
	int			nfd;
	struct pollfd		pfd[CE_MAX_POLL];

	pfd[0].events = POLLIN;
	pfd[0].fd = STDIN_FILENO;
	nfd = 1 + ce_buffer_proc_gather(&pfd[1], CE_MAX_POLL - 1);

	if ((nfd = poll(pfd, nfd, -1)) == -1) {
		if (errno == EINTR)
			return;
		fatal("%s: poll %s", __func__, errno_s);
	}

	if (nfd == 0)
		return;

	if (pfd[0].revents & (POLLHUP | POLLERR))
		fatal("%s: stdin error", __func__);

	if (pfd[0].revents & POLLIN)
		editor_read_input();

	ce_buffer_proc_dispatch();
}

static void
editor_read_input(void)
{
	ssize_t		sz;

	for (;;) {
		sz = read(STDIN_FILENO, inq.data, sizeof(inq.data));
		if (sz == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: read: %s", __func__, errno_s);
		}

		if (sz == 0)
			fatal("%s: stdin eof", __func__);

		inq.sz = sz;
		inq.off = 0;
		break;
	}
}

static void
editor_consume_input(void)
{
	size_t			idx;
	u_int8_t		key;
	struct cebuf		*curbuf = ce_buffer_active();

	if (mode >= CE_EDITOR_MODE_MAX)
		fatal("%s: mode %d invalid", __func__, mode);

	if (editor_get_input(&key, 0) == 0)
		return;

	ce_debug("key is 0x%02x", key);

	if (key == EDITOR_KEY_ESC)
		key = editor_process_escape();

	if (mode == CE_EDITOR_MODE_NORMAL_CMD) {
		editor_normal_mode_command(key);
		return;
	}

	for (idx = 0; idx < keymaps[mode].maplen; idx++) {
		if (key == keymaps[mode].map[idx].key &&
		    keymaps[mode].map[idx].command != NULL) {
			keymaps[mode].map[idx].command();
			return;
		}
	}

	switch (mode) {
	case CE_EDITOR_MODE_NORMAL:
		switch (curbuf->buftype) {
		case CE_BUF_TYPE_DEFAULT:
		case CE_BUF_TYPE_SHELLCMD:
			editor_normal_mode_command(key);
			return;
		case CE_BUF_TYPE_DIRLIST:
			editor_dirlist_mode_command(key);
			return;
		}
		break;
	case CE_EDITOR_MODE_SELECT:
		editor_select_mode_command(key);
		return;
	}

	if (mode != CE_EDITOR_MODE_COMMAND && mode != CE_EDITOR_MODE_SEARCH)
		ce_hist_autocomplete_reset(NULL);

	ce_buffer_input(curbuf, key);
}

static u_int8_t
editor_process_escape(void)
{
	u_int8_t	key, ret;

	ret = EDITOR_KEY_ESC;

	if (editor_get_input(&key, 0) == 0)
		goto cleanup;

	if (key != 0x5b)
		goto cleanup;

	if (editor_get_input(&key, 0) == 0)
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

static int
editor_get_input(u_int8_t *out, int fetch)
{
	*out = 0;

	if ((inq.sz == 0 || inq.off == inq.sz) && fetch)
		editor_read_input();

	if (inq.sz == 0 || inq.off == inq.sz)
		return (0);

	*out = inq.data[inq.off];
	inq.off++;

	return (1);
}

static void
editor_draw_status(void)
{
	const u_int8_t		*ptr;
	struct cebuf		*curbuf;
	size_t			cmdoff, width, pc;
	int			flen, slen, llen, procfd;
	const char		*isdirty, *filemode, *modestr;
	char			fline[1024], sline[128], lline[128];

	isdirty = "";
	filemode = "";
	modestr = NULL;
	curbuf = ce_buffer_active();

	switch (mode) {
	case CE_EDITOR_MODE_SEARCH:
		modestr = "- SEARCH -";
		break;
	case CE_EDITOR_MODE_NORMAL:
	case CE_EDITOR_MODE_COMMAND:
	case CE_EDITOR_MODE_BUFLIST:
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

	if (curbuf->internal == 0 && (curbuf->flags & CE_BUFFER_DIRTY))
		isdirty = " [+]";

	if (curbuf->flags & CE_BUFFER_RO)
		filemode = "r-";
	else
		filemode = "rw";

	procfd = (curbuf->proc != NULL) ? curbuf->proc->ofd : -1;

	if (curbuf->top == 0) {
		llen = snprintf(lline, sizeof(lline), "%zuL [Top]%s",
		    curbuf->lcnt, procfd != -1 ? " *" : "");
	} else if (curbuf->lcnt - (curbuf->top + curbuf->line) <
	    ((ce_term_height() - 2) / 2)) {
		llen = snprintf(lline, sizeof(lline), "%zuL [Bot]%s",
		    curbuf->lcnt, procfd != -1 ? " *" : "");
	} else {
		pc = ((curbuf->top + curbuf->line) / (float)curbuf->lcnt) * 100;
		llen = snprintf(lline, sizeof(lline), "%zuL [%zu%%]%s",
		    curbuf->lcnt, pc, procfd != -1 ? " *" : "");
	}

	if (llen == -1 || (size_t)llen >= sizeof(lline))
		fatal("failed to create status percent line");

	flen = snprintf(fline, sizeof(fline), "%s%s",
	    ce_editor_shortpath(curbuf->name), isdirty);
	if (flen == -1)
		fatal("failed to create status file line");

	if (mode == CE_EDITOR_MODE_SELECT) {
		slen = snprintf(sline, sizeof(sline),
		    "[%s] %zu,%zu-%zu %s %zu.%zu <> %zu.%zu", filemode,
		    curbuf->top + curbuf->line, curbuf->loff,
		    curbuf->column, modestr,
		    curbuf->selstart.line + 1, curbuf->selstart.col,
		    curbuf->selend.line + 1, curbuf->selend.col);
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

	if (ce_buffer_active() != cmdbuf) {
		ce_term_setpos(ce_term_height(), TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		ce_term_writestr(ce_editor_shortpath(curdir));
	}

	width = ce_term_width();
	ce_term_setpos(ce_term_height() - 2, TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
	ce_term_color(TERM_COLOR_WHITE + TERM_COLOR_FG);
	while (width > 0) {
		ce_term_writestr(CE_UTF8_U2015);
		width--;
	}

	ce_term_setpos(ce_term_height() - 1, TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
	ce_term_color(TERM_COLOR_WHITE + TERM_COLOR_FG);
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
editor_draw_cmdbuf(void)
{
	const u_int8_t	*ptr;
	size_t		width, off;

	off = ce_term_width() * 0.75f;
	width = off - 10;

	ce_term_setpos(ce_term_height(), TERM_CURSOR_MIN);
	ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);

	if (cmdbuf->length > width)
		off = cmdbuf->length - width - 1;
	else
		off = 0;

	ptr = cmdbuf->data;
	ce_term_write(&ptr[off], cmdbuf->length - off);
}

static void
editor_draw_message(struct timespec *ts)
{
	if (msg.message && mode != CE_EDITOR_MODE_COMMAND) {
		if (ts != NULL &&
		    (ts->tv_sec - msg.when) >= EDITOR_MESSAGE_DELAY) {
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
}

static void
editor_draw_suggestions(int cnt)
{
	size_t		idx, start;

	start = 2 + suggestions->lcnt;
	if (start > ce_term_height() - 2)
		start = ce_term_height() / 2;

	suggestions->orig_line = ce_term_height() - start;

	if (suggestions->lcnt > 0 && cnt > 1) {
		ce_term_writestr(TERM_SEQUENCE_CLEAR_ONLY);
		ce_buffer_map(cmdbuf->prev);

		ce_term_writestr(TERM_SEQUENCE_CURSOR_SAVE);
		ce_term_setpos(ce_term_height() - (start + 1), TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_CLEAR_CURSOR_DOWN);
		for (idx = 0; idx < ce_term_width(); idx++)
			ce_term_writestr(CE_UTF8_U2015);
		ce_term_setpos(ce_term_height() - start, TERM_CURSOR_MIN);
		ce_buffer_map(suggestions);
		ce_term_writestr(TERM_SEQUENCE_CURSOR_RESTORE);
	} else {
		ce_editor_dirty();
	}
}

static void
editor_autocomplete_path(struct cebuf *buf)
{
	struct stat		st;
	struct dirent		*dp;
	const char		*fp;
	DIR			*dir;
	char			*path, *p, *name, *match;
	int			cnt, pref_dirs, islocal, partial;
	size_t			len, off, width, mlen, nlen, idx;

	if (buf->length <= 2)
		return;

	if ((path = calloc(1, buf->length + 1)) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, buf->length, errno_s);

	memcpy(path, buf->data, buf->length);
	path[buf->length] = '\0';

	if (path[1] == 'c' && path[2] == 'd') {
		off = 3;
		pref_dirs = 1;
	} else if (path[1] == 'l') {
		off = 2;
		pref_dirs = 1;
	} else {
		off = 2;
		pref_dirs = 0;
	}

	p = &path[off];
	while (isspace(*(const unsigned char *)p)) {
		p++;
		off++;
	}

	fp = ce_editor_fullpath(p);
	if ((name = strrchr(fp, '/')) != NULL) {
		*(name)++ = '\0';
		if (fp[0] == '\0')
			dir = opendir("/");
		else
			dir = opendir(fp);
		islocal = 0;
	} else {
		name = p;
		islocal = 1;
		dir = opendir(".");
	}

	if (dir == NULL) {
		free(path);
		return;
	}

	cnt = 0;
	partial = 0;
	match = NULL;
	len = strlen(name);

	width = 0;
	ce_buffer_erase(suggestions);

	while ((dp = readdir(dir)) != NULL) {
		if (!strcmp(dp->d_name, ".") ||
		    !strcmp(dp->d_name, ".."))
			continue;

		if (len == 0 && islocal == 1 &&
		    pref_dirs == 0 && dp->d_type != DT_REG)
			continue;

		if (len > 0 && strncmp(dp->d_name, name, len))
			continue;

		if (pref_dirs && dp->d_type != DT_DIR)
			continue;

		cnt++;

		if (match != NULL) {
			mlen = strlen(match);
			nlen = strlen(dp->d_name);

			for (idx = 0; idx < nlen && idx < mlen; idx++) {
				if (match[idx] != dp->d_name[idx])
					break;
			}

			if (idx > 0) {
				free(match);
				if ((match = calloc(1, idx + 1)) == NULL) {
					fatal("%s: calloc:(%zu): %s", __func__,
					    idx + 1, errno_s);
				}

				partial++;
				memcpy(match, dp->d_name, idx);
				match[idx] = '\0';
			}
		} else {
			match = ce_strdup(dp->d_name);
		}

		mlen = strlen(dp->d_name);

		if (width + mlen + 1 > ce_term_width()) {
			width = 0;
			ce_buffer_appendf(suggestions, "\n");
		}

		width += mlen + 1;
		ce_buffer_appendf(suggestions, "%s ", dp->d_name);
	}

	ce_buffer_populate_lines(suggestions);
	editor_draw_suggestions(cnt);

	if (match != NULL && len > 0 && strlen(&match[len]) > 0) {
		ce_buffer_appendf(buf, "%s", &match[len]);
		buf->column += strlen(&match[len]);

		fp = ce_buffer_as_string(buf);
		fp = ce_editor_fullpath(&fp[off]);

		if (lstat(fp, &st) != -1) {
			buf->length--;
			if (S_ISDIR(st.st_mode) && !partial) {
				fp = buf->data;
				if (fp[buf->length - 1] != '/') {
					ce_buffer_appendf(buf, "/");
					buf->column++;
				}
			}
		} else {
			buf->length--;
		}

	}

	closedir(dir);

	free(path);
	free(match);
}

static void
editor_cmdbuf_input(struct cebuf *buf, u_int8_t key)
{
	char			*ep;
	struct cehist		*hist;
	long			linenr;
	struct cebuf		*active;
	const char		*cmd, *path;
	size_t			off, histlen;
	int			force, extcmd;
	u_int8_t		*ptr, *histcmd;

	hist = NULL;
	ptr = buf->data;

	if (buf->length > 1)
		extcmd = ptr[1] == '!';
	else
		extcmd = 0;

	if (extcmd) {
		histcmd = &ptr[2];
		histlen = buf->length - 2;
	} else {
		histcmd = &ptr[1];
		histlen = buf->length - 1;
	}

	switch (key) {
	case EDITOR_CMD_PASTE:
		suggestions_wipe = 1;
		ce_hist_autocomplete_reset(NULL);
		editor_cmd_paste();
		break;
	case '\n':
		ce_buffer_restore();

		cmd = ce_buffer_as_string(buf);
		if (isdigit((unsigned char)cmd[1])) {
			errno = 0;
			linenr = strtol(&cmd[1], &ep, 10);
			if (*ep == '\0' && errno == 0) {
				ce_buffer_jump_line(ce_buffer_active(),
				    linenr, 0);
				ce_buffer_activate(buf);
				editor_cmd_normal_mode();
				return;
			}
		}

		ce_hist_autocomplete_reset(&hist);
		if (hist != NULL) {
			editor_cmd_reset();
			cmdbuf->column = 1;
			ce_buffer_append(cmdbuf, ":", 1);
			if (extcmd) {
				cmdbuf->column++;
				ce_buffer_append(cmdbuf, "!", 1);
			}
			ce_buffer_append(cmdbuf, hist->cmd, strlen(hist->cmd));
			cmdbuf->column += strlen(hist->cmd);
			cmd = ce_buffer_as_string(buf);
		}

		off = 1;
		switch (cmd[1]) {
		case '!':
			off = 2;
			break;
		default:
			break;
		}

		if (cmd[1] != '\0' && editor_cmd_can_autocomplete())
			ce_hist_add(&cmd[off]);

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
			if (strlen(cmd) > 3)
				editor_directory_list(&cmd[3]);
			break;
		case 't':
			if (strlen(cmd) > 3) {
				if (!strcmp(&cmd[3], "show")) {
					config.tab_show = 1;
				} else if (!strcmp(&cmd[3], "hide")) {
					config.tab_show = 0;
				}
				ce_editor_dirty();
			}
			break;
		case 'b':
			switch (cmd[2]) {
			case 'c':
				switch (cmd[3]) {
				case 'o':
					ce_buffer_close_nonactive();
					ce_editor_dirty();
					break;
				case 's':
					ce_buffer_close_shellbufs();
					ce_editor_dirty();
					break;
				default:
					active = ce_buffer_active();
					if (active->buftype ==
					    CE_BUF_TYPE_SHELLCMD) {
						editor_shellbuf_close(active);
						ce_editor_dirty();
						break;
					}
					ce_buffer_free(ce_buffer_active());
					ce_editor_dirty();
					break;
				}
				break;
			}
			break;
		case 'c':
			if (!strcmp(&cmd[1], "cdb")) {
				editor_directory_from_buffer();
				break;
			}

			switch (cmd[2]) {
			case 'd':
				if (strlen(cmd) > 4)
					editor_directory_change(&cmd[4]);
				break;
			}
			break;
		case '!':
			if (strlen(cmd) > 1) {
				ep = (char *)buf->data;
				editor_cmd_execute(&ep[2]);
			}
			break;
		}
		ce_buffer_activate(buf);
		editor_cmd_normal_mode();
		break;
	case '\b':
	case 0x7f:
		ce_hist_autocomplete_reset(NULL);
		if (buf->length <= 1) {
			editor_cmd_normal_mode();
			break;
		}

		buf->length--;
		buf->column--;
		ce_term_setpos(buf->orig_line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		if (histlen > 1) {
			if (editor_cmd_can_autocomplete()) {
				hist = ce_hist_lookup(histcmd, histlen - 1, 1);
			} else {
				histlen = 0;
				hist = NULL;
				suggestions_wipe = 1;
			}
		} else {
			hist = ce_hist_lookup("*", 1, 1);
		}
		break;
	case 0x03:
		editor_cmd_normal_mode();
		break;
	case EDITOR_KEY_ESC:
		if ((hist = ce_hist_current()) != NULL) {
			hist = NULL;
			histlen = 0;
			suggestions_wipe = 1;
			ce_hist_autocomplete_reset(NULL);
		} else {
			editor_cmd_normal_mode();
		}
		break;
	case '\t':
		histlen = 0;
		hist = NULL;
		ce_hist_autocomplete_reset(NULL);
		editor_autocomplete_path(buf);
		break;
	case EDITOR_KEY_UP:
		editor_cmd_history_cycle(1, extcmd);
		break;
	case EDITOR_KEY_DOWN:
		editor_cmd_history_cycle(0, extcmd);
		break;
	case EDITOR_CMD_HIST_NEXT:
		ptr = buf->data;
		if (histlen > 0) {
			hist = ce_hist_lookup(histcmd, histlen, 0);
			if (hist == NULL)
				hist = ce_hist_current();
		}
		break;
	case EDITOR_CMD_HIST_PREV:
		ptr = buf->data;
		if (histlen > 0) {
			hist = ce_hist_lookup(histcmd, histlen, 1);
			if (hist == NULL)
				hist = ce_hist_current();
		}
		break;
	default:
		if (editor_allowed_command_key(key)) {
			ce_buffer_append(buf, &key, sizeof(key));
			buf->column++;

			ce_hist_autocomplete_reset(NULL);

			if (editor_cmd_can_autocomplete()) {
				histlen++;
				hist = ce_hist_lookup(histcmd, histlen, 1);
			} else {
				histlen = 0;
				hist = NULL;
				suggestions_wipe = 1;
			}
		}
		break;
	}

	if (key != '\t')
		editor_cmd_suggestions(hist, histlen);

	cmdbuf->lines[0].length = cmdbuf->length;
	ce_buffer_line_columns(&cmdbuf->lines[0]);
}

static void
editor_cmdbuf_search(struct cebuf *buf, u_int8_t key)
{
	const char		*cmd;
	u_int8_t		*ptr;
	struct cehist		*hist;

	hist = NULL;
	ptr = buf->data;

	switch (key) {
	case '\n':
		ce_hist_autocomplete_reset(&hist);
		if (hist != NULL) {
			editor_cmd_reset();
			cmdbuf->column = 1;
			ce_buffer_append(cmdbuf, ":", 1);
			ce_buffer_append(cmdbuf, hist->cmd, strlen(hist->cmd));
			cmdbuf->column += strlen(hist->cmd);
		}

		cmd = ce_buffer_as_string(buf);
		if (buf->prev->buftype == CE_BUF_TYPE_DIRLIST) {
			ce_dirlist_narrow(buf->prev, &cmd[1]);
			editor_cmd_normal_mode();
			return;
		}

		if (cmd[0] != '\0') {
			ce_hist_add(&cmd[1]);
			free(search);
			search = ce_strdup(cmd + 1);
			ce_buffer_search(buf->prev,
			    search, CE_BUFFER_SEARCH_NORMAL);
		}

		editor_cmd_normal_mode();
		break;
	case '\b':
	case 0x7f:
		ce_hist_autocomplete_reset(NULL);
		if (buf->length <= 1) {
			if (mode == CE_EDITOR_MODE_SEARCH &&
			    buf->prev->buftype == CE_BUF_TYPE_DIRLIST)
				ce_dirlist_narrow(buf->prev, NULL);
			editor_cmd_normal_mode();
			break;
		}
		buf->length--;
		buf->column--;
		ce_term_setpos(buf->orig_line, TERM_CURSOR_MIN);
		ce_term_writestr(TERM_SEQUENCE_LINE_ERASE);
		if (buf->length > 1)
			hist = ce_hist_lookup(&ptr[1], buf->length, 1);
		else
			hist = ce_hist_lookup("*", 1, 1);
		break;
	case EDITOR_CMD_HIST_NEXT:
		if (buf->length > 1)
			hist = ce_hist_lookup(&ptr[1], buf->length - 1, 0);
		if (hist == NULL)
			hist = ce_hist_current();
		break;
	case EDITOR_CMD_HIST_PREV:
		if (buf->length > 1)
			hist = ce_hist_lookup(&ptr[1], buf->length - 1, 1);
		if (hist == NULL)
			hist = ce_hist_current();
		break;
	case EDITOR_KEY_ESC:
		if ((hist = ce_hist_current()) != NULL) {
			hist = NULL;
			suggestions_wipe = 1;
			ce_hist_autocomplete_reset(NULL);
		} else {
			editor_cmd_normal_mode();
		}
		break;
	case EDITOR_KEY_UP:
	case EDITOR_KEY_DOWN:
		return;
	default:
		if (editor_allowed_command_key(key)) {
			ce_buffer_append(buf, &key, sizeof(key));
			buf->column++;
			ce_hist_autocomplete_reset(NULL);
			hist = ce_hist_lookup(&ptr[1], buf->length - 1, 1);
		}
		break;
	}

	editor_cmd_suggestions(hist, buf->length);

	cmdbuf->lines[0].length = buf->length;
	ce_buffer_line_columns(&cmdbuf->lines[0]);
}

static void
editor_cmd_suggestions(struct cehist *hist, size_t histlen)
{
	struct cehist	*histp;
	char		*txt[10];
	int		count, idx;

	for (idx = 0; idx < 10; idx++)
		txt[idx] = NULL;

	ce_buffer_erase(suggestions);

	if (hist != NULL) {
		histp = hist;

		count = 4;
		txt[5] = hist->cmd;

		while (count >= 0) {
			if ((histp = TAILQ_NEXT(histp, list)) == NULL)
				break;
			if (ce_hist_matches(histp))
				txt[count--] = histp->cmd;
		}

		count = 6;
		histp = hist;

		while (count < 10) {
			histp = TAILQ_PREV(histp, ce_histlist, list);
			if (histp == NULL)
				break;
			if (ce_hist_matches(histp))
				txt[count++] = histp->cmd;
		}

		for (idx = 0; idx < 10; idx++) {
			if (txt[idx] == NULL)
				continue;

			if (idx == 5) {
				ce_buffer_appendf(suggestions,
				    TERM_SEQUENCE_FMT_SET_COLOR,
				    TERM_COLOR_GREEN + TERM_COLOR_FG);
				ce_buffer_appendf(suggestions, "%s%s\n",
				    txt[idx], TERM_SEQUENCE_ATTR_OFF);
			} else {
				ce_buffer_appendf(suggestions,
				    "%s\n", txt[idx]);
			}
		}

	} else if (histlen > 0) {
		ce_buffer_appendf(suggestions, "no history matches\n");
	}

	ce_buffer_populate_lines(suggestions);
	editor_draw_suggestions(2);
}

static int
editor_cmd_can_autocomplete(void)
{
	size_t		idx;
	u_int8_t	*ptr;

	ptr = cmdbuf->data;

	for (idx = 0; idx < cmdbuf->length; idx++) {
		if (isspace(ptr[idx]))
			return (1);
	}

	if (cmdbuf->length > 4)
		return (1);

	return (0);
}

static void
editor_buflist_input(struct cebuf *buf, u_int8_t key)
{
	size_t		index;

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
		}

		lastmode = mode;
		mode = CE_EDITOR_MODE_NORMAL;
		break;
	default:
		break;
	}
}

static void
editor_normal_mode_command(u_int8_t key)
{
	long			num;
	const char		*str;
	struct cebuf		*buf;
	int			reset, next_mode;

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

	if (key == EDITOR_KEY_UTF8_PREFIX) {
		if (editor_get_input(&key, 25) == 0)
			return;

		switch (key) {
		case EDITOR_KEY_UTF8_CONSOLE:
			ce_buffer_activate_index(0);
			break;
		}

		return;
	}

	reset = 0;
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
			normalcmd = EDITOR_COMMAND_YANK;
			break;
		case 'w':
			normalcmd = EDITOR_COMMAND_WORD_NEXT;
			goto direct;
		case 'z':
			normalcmd = EDITOR_COMMAND_ALIGN;
			break;
		case 0x17:
			normalcmd = EDITOR_COMMAND_PROCESS;
			break;
		default:
			reset = 1;
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
			case 'd':
				editor_cmd_delete_lines(buf, num);
				break;
			case 'w':
				editor_cmd_delete_words(buf, num);
				break;
			}
			break;
		case EDITOR_COMMAND_YANK:
			editor_cmd_yank_lines(buf, num);
			break;
		case EDITOR_COMMAND_WORD_NEXT:
			editor_cmd_word_next(buf, num);
			break;
		case EDITOR_COMMAND_WORD_PREV:
			editor_cmd_word_prev(buf, num);
			break;
		case EDITOR_COMMAND_ALIGN:
			switch (key) {
			case 'z':
				ce_buffer_center();
				break;
			case 't':
				ce_buffer_top();
				break;
			case 'c':
				ce_buffer_mark_jump(buf, CE_MARK_SELEXEC);
				break;
			}
			break;
		case EDITOR_COMMAND_PROCESS:
			switch (key) {
			case 'k':
				ce_proc_kill(buf->proc);
				break;
			}
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
}

static void
editor_dirlist_mode_command(u_int8_t key)
{
	const char		*fp;
	size_t			idx;
	struct cebuf		*buf;
	struct celine		*line;
	char			*name;
	mode_t			filemode;

	buf = ce_buffer_active();

	idx = ce_buffer_line_index(buf);
	line = ce_buffer_line_current(buf);

	if (idx < 2 || line->length <= 1)
		return;

	if ((name = malloc(line->length)) == NULL)
		fatal("%s: malloc: %s", __func__, errno_s);

	memcpy(name, line->data, line->length - 1);
	name[line->length - 1] = '\0';

	fp = ce_dirlist_index2path(buf, idx - 3);

	switch (key) {
	case 'b':
		editor_cmd_word_prev(buf, 1);
		break;
	case 'w':
		editor_cmd_word_next(buf, 1);
		break;
	case 0x05:
		filemode = ce_dirlist_index2mode(buf, idx - 3);
		if (S_ISREG(filemode)) {
			if (ce_buffer_file(fp) == NULL)
				ce_editor_message("%s", ce_buffer_strerror());
		}
		break;
	case 'd':
		if (ce_editor_yesno(ce_dirlist_rmfile, fp,
		    "delete %s? (y/n)", name) == 0)
			ce_dirlist_rescan(buf);
		break;
	}

	free(name);
}

static void
editor_select_mode_command(u_int8_t key)
{
	switch (key) {
	case 'b':
		ce_buffer_word_prev(ce_buffer_active());
		break;
	case 'd':
		editor_cmd_select_yank_delete(1);
		editor_cmd_normal_mode();
		break;
	case 'y':
		editor_cmd_select_yank_delete(0);
		editor_cmd_normal_mode();
		break;
	case 'w':
		ce_buffer_word_next(ce_buffer_active());
		break;
	case '\n':
		editor_cmd_select_execute();
		editor_cmd_normal_mode();
		break;
	default:
		break;
	}
}

static void
editor_cmd_select_execute(void)
{
	struct stat		st;
	char			nul;
	long			linenr;
	size_t			idx, cmdlen;
	struct cebuf		*curbuf, *buf;
	int			i, try_file, len;
	char			*fp, path[PATH_MAX];
	char			*cmd, *line, *p, *e, *ep, n;

	e = NULL;
	p = NULL;
	line = NULL;
	curbuf = ce_buffer_active();

	linenr = 0;
	try_file = 1;
	editor_cmd_select_yank_delete(0);

	nul = '\0';
	ce_editor_pbuffer_append(&nul, sizeof(nul));

	cmd = pbuffer->data;
	cmdlen = strlen(cmd);

	if (cmdlen == 40) {
		for (idx = 0; idx < cmdlen; idx++) {
			if ((cmd[idx] >= 'a' && cmd[idx] <= 'f') ||
			    (cmd[idx] >= '0' && cmd[idx] <= '9'))
				continue;
			break;
		}

		if (idx == cmdlen) {
			len = snprintf(path, sizeof(path), "git show %s", cmd);
			if (len == -1 || (size_t)len >= sizeof(path))
				fatal("failed to create git show command");

			editor_shellbuf_new(path, &buf);
			ce_proc_run(path, buf, 1);
			ce_editor_pbuffer_reset();
			return;
		}
	}

	if ((p = strchr(cmd, ' ')) != NULL) {
		*p = '\0';
		for (i = 0; cmdtab[i].cmd != NULL; i++) {
			if (!strcmp(cmd, cmdtab[i].cmd)) {
				cmdtab[i].run(p + 1);
				return;
			}
		}
		*p = ' ';
	}

	if ((p = strchr(cmd, ':')) != NULL) {
		*p = '\0';
		line = p + 1;

		e = line;
		while (isdigit(*e))
			e++;

		n = *e;
		*e = '\0';

		errno = 0;
		linenr = strtol(line, &ep, 10);
		if (errno != 0 || *ep != '\0') {
			try_file = 0;
			linenr = 0;
		}
	}

	if (curbuf->buftype == CE_BUF_TYPE_DIRLIST) {
		len = snprintf(path, sizeof(path), "%s/%s", curbuf->path, cmd);
		if (len == -1 || (size_t)len >= sizeof(path))
			fatal("%s: path too long", __func__);
		fp = ce_strdup(ce_editor_fullpath(path));
	} else {
		fp = ce_strdup(ce_editor_fullpath(cmd));
	}

	if (try_file && lstat(fp, &st) != -1) {
		if (S_ISREG(st.st_mode) && !(st.st_mode & S_IXUSR)) {
			editor_cmd_open_file(fp);
			if (linenr) {
				ce_buffer_jump_line(ce_buffer_active(),
				    linenr, TERM_CURSOR_MIN);
			}
			ce_editor_pbuffer_reset();
			free(fp);
			return;
		} else if (S_ISDIR(st.st_mode)) {
			editor_directory_list(fp);
			ce_editor_pbuffer_reset();
			free(fp);
			return;
		}
	}

	free(fp);

	if (p)
		*p = ':';
	if (e)
		*e = n;

	editor_shellbuf_new(cmd, &buf);
	ce_proc_run(cmd, buf, 1);

	ce_editor_pbuffer_reset();
}

static void
editor_cmd_select_yank_delete(int del)
{
	struct cebuf		*buf;
	u_int8_t		*ptr;
	struct celine		*line;
	int			join, killed;
	size_t			idx, len, start, end, linenr;

	buf = ce_buffer_active();
	ce_editor_pbuffer_reset();

	join = 0;
	killed = 0;
	linenr = buf->selstart.line;

	if (linenr > 0)
		ce_buffer_line_allocate(buf, &buf->lines[linenr - 1]);

	for (idx = buf->selstart.line; idx <= buf->selend.line; idx++) {
		line = &buf->lines[linenr];
		ce_buffer_line_allocate(buf, line);

		ptr = line->data;

		if (buf->selstart.line == buf->selend.line) {
			start = buf->selstart.off;
			len = (buf->selend.off - buf->selstart.off) + 1;
		} else if (idx == buf->selstart.line) {
			start = buf->selstart.off;
			len = line->length - start;
		} else if (idx == buf->selend.line) {
			start = 0;
			if (line->length > 1)
				len = buf->selend.off + 1;
			else
				len = 0;
			if (len == 1 && ptr[0] != '\n')
				continue;
			if (len == line->length && len > 0)
				len--;
			if (ptr[len] == '\n')
				len = line->length;
		} else {
			start = 0;
			len = line->length;
		}

		ce_editor_pbuffer_append(&ptr[start], len);

		if (del == 0) {
			linenr++;
			continue;
		}

		if (buf->selstart.line == buf->selend.line) {
			start = buf->selstart.off;
			end = buf->selend.off;
		} else if (idx == buf->selstart.line) {
			start = buf->selstart.off;
			end = line->length - 1;
			if (start == 0)
				killed = 1;
		} else if (idx == buf->selend.line) {
			start = 0;
			end = buf->selend.off;
			if (line->length > 1) {
				if (ptr[end + 1] == '\n')
					end++;
				else
					join = 1;
			}
		} else {
			start = 0;
			end = line->length - 1;
		}

		if (start == 0 && ptr[end] == '\n') {
			if (line->flags & CE_LINE_ALLOCATED)
				free(line->data);
			memmove(&buf->lines[linenr], &buf->lines[linenr + 1],
			    (buf->lcnt - linenr - 1) * sizeof(struct celine));
			buf->lcnt--;
			continue;
		}

		if (start == 0 && end == 0) {
			linenr++;
			continue;
		}

		memmove(&ptr[start], &ptr[end + 1], line->length - (end - 1));
		line->length = line->length - (end - start) - 1;
		ce_buffer_line_columns(line);

		linenr++;
	}

	if (del)
		buf->flags |= CE_BUFFER_DIRTY;

	ce_editor_pbuffer_sync();

	if (buf->lcnt == 0) {
		ce_buffer_line_alloc_empty(buf);
		ce_buffer_jump_line(buf, buf->orig_line, buf->orig_column);
	} else {
		ce_buffer_jump_line(buf,
		    buf->selstart.line + 1, buf->selstart.col);
	}

	if (join && !killed) {
		line = &buf->lines[buf->selstart.line + 1];
		ptr = line->data;

		if (!isspace(*ptr))
			ce_buffer_join_line();
	}
}

static void
editor_cmd_change_string(struct cebuf *buf)
{
	u_int8_t		key;

	if (editor_get_input(&key, 1) == 0)
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
	ce_buffer_delete_lines(buf, start, start + end, 1, 0);

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
	struct cebuf	*buf;

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
	ce_term_update_title();
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
editor_cmd_buffer_next(void)
{
	ce_buffer_cycle(1);
}

static void
editor_cmd_buffer_prev(void)
{
	ce_buffer_cycle(0);
}

static void
editor_cmd_directory_list(void)
{
	struct cebuf	*buf;
	const char	*path;
	char		*rp, *cp, *dname;

	cp = NULL;
	buf = ce_buffer_active();

	if (buf->buftype == CE_BUF_TYPE_DIRLIST) {
		ce_dirlist_rescan(buf);
		return;
	}

	if (buf->path != NULL) {
		cp = ce_strdup(buf->path);

		if ((dname = dirname(cp)) == NULL)
			fatal("%s: dirname: %s", __func__, errno_s);

		path = dname;

		if ((rp = realpath(path, NULL)) == NULL) {
			ce_editor_message("realpath: %s: %s", path, errno_s);
			free(cp);
			return;
		}

		path = rp;
	} else {
		path = curdir;
	}

	editor_directory_list(path);

	if (cp)
		free(cp);
}

static void
editor_directory_list(const char *path)
{
	const char	*fp;
	struct cebuf	*buf;

	fp = ce_editor_fullpath(path);

	if ((buf = ce_buffer_dirlist(fp)) != NULL)
		buf->cb = editor_no_input;
	else
		ce_editor_message("failed to open dirlist for %s", path);
}

static void
editor_directory_change(const char *path)
{
	char		*rp;
	const char	*fp;

	fp = ce_editor_fullpath(path);
	rp = realpath(fp, NULL);

	if (chdir(fp) == -1) {
		ce_editor_message("failed to chdir(%s): %s", path, errno_s);
	} else {
		if (rp == NULL)
			fatal("%s: realpath: %s", __func__, errno_s);

		free(curdir);
		curdir = rp;

		ce_editor_message("%s", ce_editor_shortpath(curdir));
	}
}

static void
editor_directory_from_buffer(void)
{
	struct cebuf	*buf;
	char		*str, *dir;

	buf = ce_buffer_active();
	if (buf->path == NULL) {
		ce_editor_message("active buffer has no path");
		return;
	}

	str = ce_strdup(buf->path);
	dir = dirname(str);

	editor_directory_change(dir);

	free(str);
}

static void
editor_no_input(struct cebuf *buf, u_int8_t key)
{
}

static void
editor_shellbuf_close(struct cebuf *buf)
{
	if (buf->proc != NULL) {
		ce_editor_message("not closing, has active process");
		return;
	}

	ce_buffer_free(buf);
}

static void
editor_shellbuf_new(const char *cmd, struct cebuf **out)
{
	struct cebuf	*buf;
	char		name[128];

	(void)snprintf(name, sizeof(name), "command output <%s>", cmd);

	buf = ce_buffer_alloc(0);
	buf ->flags |= CE_BUFFER_RO;
	buf ->buftype = CE_BUF_TYPE_SHELLCMD;

	ce_buffer_setname(buf, name);

	buf->data = NULL;
	ce_buffer_line_alloc_empty(buf);

	*out = buf;
}

static void
editor_cmd_execute(char *cmd)
{
	struct cebuf	*buf;

	editor_shellbuf_new(cmd, &buf);
	ce_proc_run(cmd, buf, 0);
}

static void
editor_cmd_history_prev(void)
{
	ce_hist_autocomplete(1);
}

static void
editor_cmd_history_next(void)
{
	ce_hist_autocomplete(0);
}

static void
editor_cmd_history_cycle(int next, int extcmd)
{
	struct cehist		*hist;

	if (next)
		hist = ce_hist_next();
	else
		hist = ce_hist_prev();

	if (hist == NULL)
		return;

	editor_cmd_reset();
	cmdbuf->column = 1;
	ce_buffer_append(cmdbuf, ":", 1);

	if (extcmd) {
		cmdbuf->column++;
		ce_buffer_append(cmdbuf, "!", 1);
	}

	ce_buffer_append(cmdbuf, hist->cmd, strlen(hist->cmd));
	cmdbuf->column += strlen(hist->cmd);
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
	struct cebuf		*buf;
	struct celine		*line;
	const u_int8_t		*ptr, *lptr;
	size_t			idx, len, prev, lines;
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
	if (buf == cmdbuf) {
		ce_buffer_append(buf, pbuffer->data, pbuffer->length);
		buf->column += pbuffer->length;
#if defined(__APPLE__)
		goto reset;
#else
		return;
#endif
	}

	if (buf->lcnt > 0) {
		idx = ce_buffer_line_index(buf);
		line = &buf->lines[idx];
		lptr = line->data;
	} else {
		line = NULL;
		lptr = NULL;
	}

	ptr = pbuffer->data;
	editor_cmd_insert_mode();

	if (lptr == NULL || lptr[0] != '\0') {
		if (ptr[pbuffer->length - 1] == '\n') {
			ce_buffer_jump_left();
			ce_buffer_move_down();
			ce_buffer_input(buf, '\n');
			ce_buffer_move_up();
			len = pbuffer->length - 1;
		} else {
			len = pbuffer->length;
		}
	} else {
		len = pbuffer->length;
	}

	lines = 0;
	pasting = 1;
	prev = ce_buffer_line_index(buf) + 1;

	for (idx = 0; idx < len; idx++) {
		ce_buffer_input(buf, ptr[idx]);
		if (ptr[idx] == '\n')
			lines++;
	}

	pasting = 0;

	ce_editor_dirty();

	if (len > 0 && ptr[len - 1] == '\n')
		ce_buffer_jump_left();

	lastmode = mode;
	mode = CE_EDITOR_MODE_NORMAL;

	if (lines > 1)
		ce_buffer_jump_line(buf, prev, 0);

#if defined(__APPLE__)
reset:
	pbuffer->data = old;
	pbuffer->length = old_len;
#endif
}

static void
editor_cmd_select_mode(void)
{
	struct cebuf		*buf = ce_buffer_active();

	if (buf->lcnt == 0)
		return;

	lastmode = mode;
	mode = CE_EDITOR_MODE_SELECT;

	buf->selstart.set = 1;
	buf->selstart.off = buf->loff;
	buf->selstart.col = buf->column;
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
	struct cehist	*hist;

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

	if (cmdbuf->prev->buftype == CE_BUF_TYPE_DIRLIST) {
		ce_dirlist_narrow(cmdbuf->prev, NULL);
	} else {
		hist = ce_hist_lookup("*", 1, 1);
		editor_cmd_suggestions(hist, 0);
	}
}

static void
editor_cmd_normal_mode(void)
{
	struct cebuf	*buf = ce_buffer_active();

	ce_hist_autocomplete_reset(NULL);

	if (mode == CE_EDITOR_MODE_INSERT) {
		lastmode = mode;
		mode = CE_EDITOR_MODE_NORMAL;
		if (buf->lcnt > 0) {
			ce_buffer_constrain_cursor_column(buf);
			ce_term_setpos(buf->cursor_line, buf->column);
		}
		return;
	}

	if (mode == CE_EDITOR_MODE_SELECT) {
		memset(&buf->selmark, 0, sizeof(buf->selmark));
		memset(&buf->selstart, 0, sizeof(buf->selstart));
		memset(&buf->selend, 0, sizeof(buf->selend));
		ce_editor_dirty();
	}

	if (mode == CE_EDITOR_MODE_COMMAND ||
	    mode == CE_EDITOR_MODE_BUFLIST ||
	    mode == CE_EDITOR_MODE_SEARCH) {
		ce_buffer_restore();
	}

	if (mode == CE_EDITOR_MODE_COMMAND || mode == CE_EDITOR_MODE_SEARCH)
		editor_cmd_reset();

	if (mode != lastmode &&
	    (lastmode == CE_EDITOR_MODE_BUFLIST ||
	    lastmode == CE_EDITOR_MODE_SELECT)) {
		mode = lastmode;
	} else {
		lastmode = mode;
		mode = CE_EDITOR_MODE_NORMAL;
	}
}

static void
editor_cmd_grep(void)
{
	editor_preset_cmd(CE_GREP_CMD);
}

static void
editor_cmd_find(void)
{
	editor_preset_cmd(CE_FIND_CMD);
}

static void
editor_cmd_exec(void)
{
	editor_preset_cmd("!");
}

static void
editor_preset_cmd(const char *cmd)
{
	size_t		len;
	struct cehist	*hist;

	len = strlen(cmd);

	editor_cmd_command_mode();
	ce_buffer_append(cmdbuf, cmd, len);

	cmdbuf->length = 1 + len;
	cmdbuf->column = 2 + len;
	cmdbuf->lines[0].length = cmdbuf->length;

	ce_buffer_line_columns(&cmdbuf->lines[0]);

	ce_hist_autocomplete_reset(NULL);

	if (cmd[0] == '!')
		hist = ce_hist_lookup(&cmd[1], strlen(cmd) - 1, 1);
	else
		hist = ce_hist_lookup(cmd, strlen(cmd), 1);

	editor_cmd_suggestions(hist, 0);
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
	if (isprint((unsigned char)key))
		return (1);

	return (0);
}

static void
editor_cmd_open_file(const char *path)
{
	const char	*fp;

	fp = ce_editor_fullpath(path);

	if (ce_buffer_file(fp) == NULL) {
		ce_editor_message("%s", ce_buffer_strerror());
		return;
	}

	free(msg.message);
	msg.message = NULL;

	ce_editor_dirty();
}
