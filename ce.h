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

#ifndef __H_CE_H

#include <sys/queue.h>

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>

#define CE_GREP_CMD		"!rg -uuu --line-number "
#define CE_FIND_CMD		"!find . -type f -name "
#define errno_s			strerror(errno)

#define CE_MAX_POLL			128
#define CE_MAX_FILE_SIZE		(1024 * 1024 * 1024)

#define CE_BUFFER_SEARCH_NORMAL		0
#define CE_BUFFER_SEARCH_PREVIOUS	1
#define CE_BUFFER_SEARCH_NEXT		2

#define CE_EDITOR_MODE_NORMAL		0
#define CE_EDITOR_MODE_INSERT		1
#define CE_EDITOR_MODE_COMMAND		2
#define CE_EDITOR_MODE_BUFLIST		3
#define CE_EDITOR_MODE_SEARCH		4
#define CE_EDITOR_MODE_SELECT		5
#define CE_EDITOR_MODE_NORMAL_CMD	6
#define CE_EDITOR_MODE_MAX		7

#define TERM_COLOR_BLACK		0
#define TERM_COLOR_RED			1
#define TERM_COLOR_GREEN		2
#define TERM_COLOR_YELLOW		3
#define TERM_COLOR_BLUE			4
#define TERM_COLOR_MAGENTA		5
#define TERM_COLOR_CYAN			6
#define TERM_COLOR_WHITE		7

#define TERM_COLOR_BRIGHT		40
#define TERM_COLOR_FG			30
#define TERM_COLOR_BG			40

#define TERM_CURSOR_MIN			1
#define TERM_ESCAPE			"\33["

#define TERM_SEQUENCE_CLEAR_CURSOR_DOWN	TERM_ESCAPE "J"
#define TERM_SEQUENCE_CLEAR_CURSOR_UP	TERM_ESCAPE "1J"
#define TERM_SEQUENCE_CLEAR_ONLY	TERM_ESCAPE "2J"
#define TERM_SEQUENCE_CLEAR		TERM_ESCAPE "2J" TERM_ESCAPE "1;1H"
#define TERM_SEQUENCE_CURSOR_UP		TERM_ESCAPE "0A"
#define TERM_SEQUENCE_CURSOR_DOWN	TERM_ESCAPE "0B"
#define TERM_SEQUENCE_CURSOR_RIGHT	TERM_ESCAPE "0C"
#define TERM_SEQUENCE_CURSOR_LEFT	TERM_ESCAPE "0D"
#define TERM_SEQUENCE_CURSOR_SAVE	"\0337"
#define TERM_SEQUENCE_CURSOR_RESTORE	"\0338"
#define TERM_SEQUENCE_LINE_ERASE	TERM_ESCAPE "K"

#define TERM_SEQUENCE_ATTR_OFF		TERM_ESCAPE "m"
#define TERM_SEQUENCE_ATTR_BOLD		TERM_ESCAPE "1m"
#define TERM_SEQUENCE_ATTR_REVERSE	TERM_ESCAPE "7m"
#define TERM_SEQUENCE_FMT_SET_COLOR	TERM_ESCAPE "%dm"
#define TERM_SEQUENCE_FMT_SET_CURSOR	TERM_ESCAPE "%zu;%zuH"

#define TERM_SEQUENCE_ALTERNATE_ON	TERM_ESCAPE "?1049h"
#define TERM_SEQUENCE_ALTERNATE_OFF	TERM_ESCAPE "?1049l"

#define CE_FILE_TYPE_PLAIN		0
#define CE_FILE_TYPE_C			1
#define CE_FILE_TYPE_PYTHON		2
#define CE_FILE_TYPE_DIFF		3
#define CE_FILE_TYPE_JS			4
#define CE_FILE_TYPE_SHELL		5
#define CE_FILE_TYPE_SWIFT		6
#define CE_FILE_TYPE_YAML		7
#define CE_FILE_TYPE_JSON		8
#define CE_FILE_TYPE_DIRLIST		9
#define CE_FILE_TYPE_HTML		10
#define CE_FILE_TYPE_CSS		11
#define CE_FILE_TYPE_GO			12

#define CE_TAB_WIDTH_DEFAULT		8
#define CE_TAB_EXPAND_DEFAULT		0

/*
 * Gamified statistics, because I can.
 */

#define CE_XP_GROWTH		15
#define CE_XP_INITIAL		100
#define CE_XP_PER_AWARD		100

struct cegame {
	u_int32_t	xp;
	u_int32_t	opens;
} __attribute__((packed));

/*
 * Configuration options.
 */
struct ceconf {
	/* Tab width (default: 8). */
	int		tab_width;

	/* Tab expand (default: no). */
	int		tab_expand;

	/* Show visual tabs (default: yes). */
	int		tab_show;
};

extern struct ceconf		config;

/*
 * A history entry for a command executed via cmdbuf or select-execute.
 */
struct cehist {
	char			*cmd;
	TAILQ_ENTRY(cehist)	list;
};

TAILQ_HEAD(ce_histlist, cehist);

/*
 * Represents a single line in a file.
 */
#define CE_LINE_ALLOCATED	(1 << 1)

struct celine {
	/* Flags. */
	u_int32_t		flags;

	/* Line data. */
	void			*data;

	/* Size of allocation in case line is allocated. */
	size_t			maxsz;

	/* Length of the line in bytes. */
	size_t			length;

	/* Length of the line in columns. */
	size_t			columns;
};

/*
 * A marker and its associated line in a cebuf.
 */
#define CE_MARK_MIN		'0'
#define CE_MARK_MAX		'z'
#define CE_MARK_OFFSET		CE_MARK_MIN
#define CE_MARK_PREVIOUS	'\''
#define CE_MARK_SELEXEC		'.'

struct cemark {
	/* If this mark has valid data. */
	int			set;

	/* Line this mark is at. */
	size_t			line;

	/* Line column (for select). */
	size_t			col;

	/* Offset in line (for select). */
	size_t			off;
};

/*
 * A running process that is attached to a buffer.
 */
#define CE_PROC_AUTO_SCROLL	(1 << 1)

struct ceproc {
	/* Process id. */
	pid_t			pid;

	/* File descriptor to read from. */
	int			ofd;

	/* Set from ce_buffer_proc_gather() until ce_buffer_proc_dispatch(). */
	struct pollfd		*pfd;

	/* XXX merge into flags? */
	int			first;

	/* Aux flags. */
	int			flags;

	/* Line number index when command started. */
	size_t			idx;

	/* Number of bytes read in total. */
	size_t			cnt;

	/* The command that was run. */
	char			*cmd;

	/* Pointer back to owning buffer. */
	struct cebuf		*buf;
};

/*
 * A buffer from either a file or internal.
 */
#define CE_BUFFER_DIRTY		0x0001
#define CE_BUFFER_RO		0x0004

#define CE_BUF_TYPE_DEFAULT	0
#define CE_BUF_TYPE_DIRLIST	1
#define CE_BUF_TYPE_SHELLCMD	2

struct cebuf {
	/* Internal buffer? */
	int			internal;

	/* Buffer type, if not internal. */
	u_int16_t		buftype;

	/* Buffer flags. */
	u_int32_t		flags;

	/* File type, for syntax and tabs etc. */
	u_int32_t		type;

	/* The data inside the buffer, its max size and current length. */
	void			*data;
	size_t			maxsz;
	size_t			length;

	/* Pointer to previous buffer. */
	struct cebuf		*prev;

	/* If backed by a file, the path. */
	char			*path;

	/* File mode. */
	mode_t			mode;

	/* Last mtime. */
	time_t			mtime;

	/* Name of the buffer. */
	char			*name;

	/* Current cursor line. */
	size_t			cursor_line;

	/* Current line / column offsets (indexed from 1). */
	size_t			line;
	size_t			column;

	/* Size and starting position. */
	size_t			width;
	size_t			height;
	size_t			orig_line;
	size_t			orig_column;

	/* The "top" of where to begin to render the lines (0 based index). */
	size_t			top;

	/* The byte offset in the current line we're at (0 based index). */
	size_t			loff;

	/* Number of lines in this buffer (0 based index). */
	size_t			lcnt;
	struct celine		*lines;

	/* Markers. */
	struct cemark		markers[CE_MARK_MAX];

	/* Special marker (previous). */
	struct cemark		prevmark;

	/* Special markers for selection. */
	struct cemark		selend;
	struct cemark		selmark;
	struct cemark		selstart;
	struct cemark		selexec;

	/* Attached, proc or NULL if none. */
	struct ceproc		*proc;

	/* Callback for special buffers (like cmdbuf). */
	void			(*cb)(struct cebuf *, u_int8_t);

	/* Internal buffer special backing data (for dirlist etc). */
	void			*intdata;

	TAILQ_ENTRY(cebuf)	list;
};

TAILQ_HEAD(cebuflist, cebuf);

void		ce_buffer_cycle(int);
void		ce_buffer_resize(void);
void		ce_buffer_cleanup(void);
void		ce_buffer_restore(void);
void		ce_buffer_init(int, char **);
void		ce_buffer_proc_dispatch(void);
void		ce_buffer_map(struct cebuf *);
void		ce_buffer_free(struct cebuf *);
void		ce_buffer_list(struct cebuf *);
int		ce_buffer_scratch_active(void);
void		ce_buffer_reset(struct cebuf *);
void		ce_buffer_erase(struct cebuf *);
void		ce_buffer_close_nonactive(void);
void		ce_buffer_close_shellbufs(void);
void		ce_buffer_activate_index(size_t);
void		ce_buffer_activate(struct cebuf *);
size_t		ce_buffer_line_index(struct cebuf *);
void		ce_buffer_word_erase(struct cebuf *);
void		ce_buffer_word_delete(struct cebuf *);
void		ce_buffer_insert_line(struct cebuf *);
void		ce_buffer_insert_line(struct cebuf *);
void		ce_buffer_line_columns(struct celine *);
void		ce_buffer_free_internal(struct cebuf *);
void		ce_buffer_populate_lines(struct cebuf *);
int		ce_buffer_save_active(int, const char *);
void		ce_buffer_mark_set(struct cebuf *, char);
void		ce_buffer_mark_jump(struct cebuf *, char);
void		ce_buffer_input(struct cebuf *, u_int8_t);
void		ce_buffer_line_alloc_empty(struct cebuf *);
void		ce_buffer_delete_line(struct cebuf *, int);
void		ce_buffer_mark_last(struct cebuf *, size_t);
void		ce_buffer_center_line(struct cebuf *, size_t);
int		ce_buffer_proc_gather(struct pollfd *, size_t);
void		ce_buffer_setname(struct cebuf *, const char *);
void		ce_buffer_jump_line(struct cebuf *, long, size_t);
void		ce_buffer_constrain_cursor_column(struct cebuf *);
int		ce_buffer_search(struct cebuf *, const char *, int);
void		ce_buffer_append(struct cebuf *, const void *, size_t);
void		ce_buffer_appendl(struct cebuf *, const void *, size_t);
void		ce_buffer_line_allocate(struct cebuf *, struct celine *);
void		ce_buffer_delete_inside_string(struct cebuf *, u_int8_t);
void		ce_buffer_delete_lines(struct cebuf *, size_t,
		    size_t, int, int);
void		ce_buffer_appendf(struct cebuf *, const char *, ...)
		    __attribute__((format (printf, 2, 3)));

void		ce_buffer_word_next(struct cebuf *);
void		ce_buffer_word_prev(struct cebuf *);
int		ce_buffer_word_cursor(struct cebuf *,
		    const u_int8_t **, size_t *);

void		ce_buffer_top(void);
void		ce_buffer_center(void);
void		ce_buffer_page_up(void);
void		ce_buffer_move_up(void);
void		ce_buffer_jump_down(void);
void		ce_buffer_page_down(void);
void		ce_buffer_move_down(void);
void		ce_buffer_move_left(void);
void		ce_buffer_jump_left(void);
void		ce_buffer_join_line(void);
void		ce_buffer_move_right(void);
void		ce_buffer_jump_right(void);
void		ce_buffer_delete_character(void);

const char	*ce_buffer_strerror(void);
const char	*ce_buffer_as_string(struct cebuf *);

struct cebuf	*ce_buffer_alloc(int);
struct cebuf	*ce_buffer_active(void);
struct cebuf	*ce_buffer_first_dirty(void);
struct cebuf	*ce_buffer_file(const char *);
struct cebuf	*ce_buffer_dirlist(const char *);
struct cebuf	*ce_buffer_internal(const char *);
struct celine	*ce_buffer_line_current(struct cebuf *);

void		ce_term_color(int);
void		ce_term_setup(void);
void		ce_term_flush(void);
size_t		ce_term_width(void);
size_t		ce_term_height(void);
void		ce_term_discard(void);
void		ce_term_restore(void);
void		ce_term_update_title(void);
void		ce_term_setpos(size_t, size_t);
void		ce_term_writestr(const char *);
void		ce_term_write(const void *, size_t);
void		ce_term_foreground_rgb(int, int, int);
void		ce_term_background_rgb(int, int, int);
void		ce_term_writef(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

void		ce_term_attr_off(void);
void		ce_term_attr_bold(void);
void		ce_term_attr_reverse(void);

void		ce_dirlist_rmfile(const void *);
void		ce_dirlist_close(struct cebuf *);
void		ce_dirlist_rescan(struct cebuf *);
mode_t		ce_dirlist_index2mode(struct cebuf *, size_t);
void		ce_dirlist_path(struct cebuf *, const char *);
const char	*ce_dirlist_index2path(struct cebuf *, size_t);
void		ce_dirlist_narrow(struct cebuf *, const char *);

void		ce_editor_init(void);
void		ce_editor_loop(void);
int		ce_editor_mode(void);
const char	*ce_editor_pwd(void);
const char	*ce_editor_home(void);
void		ce_editor_dirty(void);
int		ce_editor_pasting(void);
void		ce_editor_set_pasting(int);
void		ce_editor_show_splash(void);
int		ce_editor_word_byte(u_int8_t);
const char	*ce_editor_fullpath(const char *);
void		ce_editor_settings(struct cebuf *);
const char	*ce_editor_shortpath(const char *);
int		ce_editor_word_separator(u_int8_t);
void		ce_editor_message(const char *, ...);
void		ce_editor_messagev(const char *, va_list);
int		ce_editor_yesno(void (*)(const void *),
		    const void *, const char *, ...)
		    __attribute__((format (printf, 3, 4)));

void		ce_editor_pbuffer_sync(void);
void		ce_editor_pbuffer_reset(void);
void		ce_editor_pbuffer_append(const void *, size_t);
void		ce_editor_cmdline_append(const char *, ...)
		    __attribute__((format (printf, 1, 2)));

struct cebuf	*ce_editor_cmdbuf(void);
void		ce_editor_cmdbuf_reset(void);

int		ce_utf8_continuation_byte(u_int8_t);
int		ce_utf8_sequence(const void *, size_t, size_t, size_t *);

void		ce_hist_init(void);
void		ce_hist_add(const char *);
void		ce_hist_autocomplete(int);
int		ce_hist_matches(struct cehist *);
void		ce_hist_autocomplete_reset(struct cehist **);

struct cehist	*ce_hist_prev(void);
struct cehist	*ce_hist_next(void);
struct cehist	*ce_hist_current(void);
struct cehist	*ce_hist_lookup(const void *, size_t, int);

void		ce_proc_reap(struct ceproc *);
void		ce_proc_read(struct ceproc *);
void		ce_proc_kill(struct ceproc *);
void		ce_proc_run(char *, struct cebuf *, int);

void		ce_syntax_init(void);
void		ce_syntax_finalize(void);
void		ce_syntax_guess(struct cebuf *);
void		ce_syntax_write(struct cebuf *, struct celine *,
		    size_t, size_t);

u_int32_t	ce_game_xp(void);
void		ce_game_init(void);
u_int32_t	ce_game_level(void);
void		ce_game_add_xp(void);
void		ce_game_add_open(void);
u_int32_t	ce_game_open_count(void);
const char	*ce_game_level_name(void);
u_int32_t	ce_game_xp_required(u_int32_t);
void		ce_game_update(struct cegame *);

int		ce_lame_mode(void);
void		ce_file_type_detect(struct cebuf *);

char		*ce_strdup(const char *);
void		ce_debug(const char *, ...)
		    __attribute__((format (printf, 1, 2)));
void		fatal(const char *, ...)
		    __attribute__((format (printf, 1, 2)))
		    __attribute__((noreturn));

#if defined(__APPLE__)
void		ce_macos_get_pasteboard_contents(u_int8_t **, size_t *);
void		ce_macos_set_pasteboard_contents(const void *, size_t);
#endif

#endif
