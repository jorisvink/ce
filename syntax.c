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
#include <sys/stat.h>

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ce.h"


#define SYNTAX_CLEAR_COMMENT	0x0001

struct state {
	const u_int8_t	*p;

	size_t		len;
	size_t		off;
	size_t		col;
	size_t		avail;

	int		r;
	int		g;
	int		b;

	int		bold;
	int		dirty;
	int		highlight;
	int		selection;

	int		keepcolor;
	int		diffcolor;
	int		stringcolor;

	int		inside_string;
	int		inside_comment;

	const u_int8_t	*ppword;
	size_t		ppwlen;
	int		inside_preproc;

	struct cebuf	*buf;
	size_t		index;

	int		color;
	u_int32_t	flags;
};

static void	syntax_write(struct state *, size_t);
static void	syntax_term_write(struct state *, const void *, size_t, int);

static int	syntax_escaped_quote(struct state *);
static int	syntax_is_word(struct state *, size_t);

static void	syntax_state_selection(struct state *);

static void	syntax_state_term_reset(struct state *);
static void	syntax_state_term_highlight(struct state *);
static void	syntax_state_term_bold(struct state *, int);
static void	syntax_state_foreground_color(struct state *, int, int, int);

static void	syntax_state_color(struct state *, int);
static void	syntax_state_color_clear(struct state *);

static void	syntax_highlight_js(struct state *);
static void	syntax_highlight_diff(struct state *);

static void	syntax_highlight_shell(struct state *);
static int	syntax_highlight_shell_variable(struct state *);

static void	syntax_highlight_c(struct state *);
static int	syntax_highlight_c_label(struct state *);
static int	syntax_highlight_c_comment(struct state *);
static int	syntax_highlight_c_preproc(struct state *);

static void	syntax_highlight_zig(struct state *);

static void	syntax_highlight_lua(struct state *);
static int	syntax_highlight_lua_comment(struct state *);

static void	syntax_highlight_go(struct state *);
static void	syntax_highlight_yaml(struct state *);
static void	syntax_highlight_swift(struct state *);
static void	syntax_highlight_latex(struct state *);
static void	syntax_highlight_dirlist(struct state *);

static void	syntax_highlight_python(struct state *);
static int	syntax_highlight_python_decorator(struct state *);
static int	syntax_highlight_python_multiline_string(struct state *);

static int	syntax_highlight_string(struct state *);
static int	syntax_highlight_numeric(struct state *);
static void	syntax_highlight_format_string(struct state *);
static int	syntax_highlight_pound_comment(struct state *);
static int	syntax_highlight_word(struct state *, const char *[]);
static void	syntax_highlight_span(struct state *, char, char, int);

static const char *tags[] = {
	"XXX",
	"TODO",
	NULL
};

static const char *lua_kw[] = {
	"and", "break", "do", "else", "elseif",
	"end", "false", "for", "function", "if",
	"in", "local", "nil", "not", "or",
	"repeat", "return", "then", "true", "until", "while",
	NULL
};

static const char *c_kw[] = {
	"if", "do", "for", "else", "while", "return", "sizeof",
	"case", "switch", "default", "break", "goto", "continue",
	NULL
};

static const char *c_type[] = {
	"int", "char", "short", "long", "double", "float", "size_t",
	"ssize_t", "const", "struct", "static", "unsigned", "void",
	"uint8_t", "uint16_t", "uint32_t", "uint64_t",
	"int8_t", "int16_t", "int32_t", "int64_t",
	"u_int8_t", "u_int16_t", "u_int32_t", "u_int64_t",
	"extern", "volatile", "sig_atomic_t", "time_t", "FILE",
	"enum", "union", "va_list", "bool", "inline", "typedef",
	NULL
};

static const char *c_special[] = {
	"NULL", "__file__", "__func__", "__LINE__",
	"SIGHUP", "SIGINT", "SIGQUIT", "SIGILL",
	"SIGABRT", "SIGFPE", "SIGKILL", "SIGSEGV",
	"SIGPIPE", "SIGALRM", "SIGTERM", "SIGUSR1",
	"SIGUSR2", "SIGCHLD", "SIGCONT", "SIGSTOP",
	"SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGBUS",
	"SIGPOLL", "SIGPROF", "SIGSYS", "SIGTRAP",
	"SIGURG", "SIGVTALRM", "SIGXCPU", "SIGXFSZ",
	NULL
};

static const char *py_kw[] = {
	"and", "del", "for", "is", "raise", "assert", "elif",
	"lambda", "return", "break", "else", "global", "not", "try",
	"class", "except", "if", "or", "while", "continue", "exec",
	"pass", "def", "finally", "in", "await", "async", "as",
	NULL
};

static const char *py_types[] = {
	"None", "False", "True", "print",
	"abs", "delattr", "hash", "memoryview", "set",
	"all", "dict", "help", "min", "setattr",
	"any", "dir", "hex", "next", "slice",
	"ascii", "divmod", "id", "object", "sorted",
	"bin", "enumerate", "input", "oct", "staticmethod",
	"bool", "eval", "int", "open", "str",
	"breakpoint", "exec", "isinstance", "ord", "sum",
	"bytearray", "filter", "issubclass", "pow", "super",
	"bytes", "float", "iter", "print", "tuple",
	"chr", "frozenset", "list", "range", "vars",
	"classmethod", "getattr", "locals", "repr", "zip",
	"compile", "globals", "map", "reversed", "__import__",
	"complex", "hasattr", "max", "round", "len",
	NULL
};

static const char *py_special[] = {
	"import", "from", NULL
};

static const char *swift_kw[] = {
	"associatedtype", "class", "deinit", "enum", "extension",
	"fileprivate", "func", "import", "init", "inout", "internal",
	"let", "open", "operator", "private", "protocol", "public",
	"rethrows", "static", "struct", "subscript", "typealias", "var",

	"break", "case", "continue", "default", "defer", "do", "else",
	"fallthrough", "for", "guard", "if", "in", "repeat", "return",
	"switch", "where", "while",

	"as", "Any", "catch", "false", "is", "nil", "super", "self",
	"Self", "throw", "throws", "true", "try",

	"_",

	"#available", "#colorLiteral", "#column", "#else", "#elseif",
	"#endif", "#error", "#file", "#fileID", "#fileLiteral", "#filePath",
	"#function", "#if", "#imageLiteral", "#line", "#selector",
	"#sourceLocation", "#warning",
	NULL
};

static const char *js_kw[] = {
	"break", "case", "catch", "continue", "debugger", "default",
	"delete", "do", "else", "finally", "for", "if", "in", "new",
	"return", "switch", "throw", "try", "void", "while", "width",
	NULL
};

static const char *js_other[] = {
	"function", "instanceof", "this", "typeof", "var",
	NULL
};

static const char *sh_kw[] = {
	"if", "fi", "while", "do", "exit", "return",
	"shift", "case", "esac", "echo", "print", "set",
	"then", "for", "while", "in", "done", "else", NULL
};

static const char *go_kw[] = {
	"break", "default", "func", "interface", "select",
	"case", "defer", "go", "map", "struct",  "chan",
	"else", "goto", "package", "switch", "const",
	"fallthrough", "if", "range", "type", "continue",
	"for", "import", "return", "var", NULL
};

/* I really should add macro support to my editor.... */
static const char *zig_kw[] = {
	"addrspace", "align", "and", "asm", "async",
	"await", "break", "catch", "comptime", "const",
	"continue", "defer", "else", "enum", "errdefer",
	"error", "export", "extern", "for", "if", "inline",
	"noalias", "noinline", "nosuspend", "opaque", "or",
	"orelse", "packed", "anyframe", "pub", "resume", "return",
	"linksection", "callconv", "struct", "suspend", "switch",
	"test", "threadlocal", "try", "union", "unreachable", "usingnamespace",
	"var", "volatile", "allowzero", "while", "anytype", "fn",
	NULL
};

enum {
	SYNTAX_COLOR_BLACK = 0,
	SYNTAX_COLOR_RED,
	SYNTAX_COLOR_GREEN,
	SYNTAX_COLOR_YELLOW,
	SYNTAX_COLOR_BLUE,
	SYNTAX_COLOR_MAGENTA,
	SYNTAX_COLOR_CYAN,
	SYNTAX_COLOR_WHITE,
	SYNTAX_COLOR_TEAL,
	SYNTAX_COLOR_COMMENT
};

static struct {
	int	r;
	int	g;
	int	b;
} rgb[] = {
	{ 128, 128, 128 },
	{ 192, 0, 0 },
	{ 0, 255, 0 },
	{ 255, 255, 51 },
	{ 0, 0, 255 },
	{ 255, 0, 255 },
	{ 0, 255, 255 },
	{ 255, 255, 255 },
	{ 52, 139, 115 },
	{ 32, 128, 128 },
};

static struct state	syntax_state = { 0 };

void
ce_syntax_init(void)
{
	memset(&syntax_state, 0, sizeof(syntax_state));

	syntax_state.color = -1;

	syntax_state.r = -1;
	syntax_state.g = -1;
	syntax_state.b = -1;

	ce_term_attr_off();
}

void
ce_syntax_finalize(void)
{
	syntax_state_term_reset(&syntax_state);
}

void
ce_syntax_write(struct cebuf *buf, struct celine *line, size_t index,
    size_t towrite)
{
	const u_int8_t		*p;
	size_t			spaces, i, tw;
	const char		*tabstart, *tabpos;

	p = line->data;
	tw = config.tab_width;

	syntax_state.col = 1;
	syntax_state.off = 0;
	syntax_state.buf = buf;
	syntax_state.index = index;
	syntax_state.keepcolor = 0;
	syntax_state.diffcolor = -1;
	syntax_state.avail = towrite;

	if (syntax_state.flags & SYNTAX_CLEAR_COMMENT) {
		syntax_state.flags &= ~SYNTAX_CLEAR_COMMENT;
		syntax_state.inside_comment = 0;

		syntax_state_term_reset(&syntax_state);
		syntax_state.color = -1;
		syntax_state.r = -1;
		syntax_state.g = -1;
		syntax_state.b = -1;
	}

	if (towrite == 1 && p[0] == '\n') {
		syntax_state.ppwlen = 0;
		syntax_state.ppword = NULL;
		syntax_state.inside_preproc = 0;
		if (syntax_state.inside_string == 0)
			syntax_state_color_clear(&syntax_state);
		return;
	}

	while (syntax_state.off != towrite) {
		switch (p[syntax_state.off]) {
		case '\t':
			if (config.tab_show) {
				tabpos = "\xc2\xb7";
				tabstart = ">";
				syntax_state_term_bold(&syntax_state, 0);
				syntax_state_foreground_color(&syntax_state,
				    64, 64, 64);
			} else {
				tabpos = " ";
				tabstart = " ";
			}

			if ((syntax_state.col % tw) == 0)
				spaces = 1;
			else
				spaces = tw - (syntax_state.col % tw) + 1;

			syntax_state.col += spaces;

			syntax_term_write(&syntax_state, tabstart, 1, 0);
			for (i = 1; i < spaces; i++)
				syntax_term_write(&syntax_state, tabpos, 2, 0);

			syntax_state.off++;

			if (syntax_state.inside_comment) {
				syntax_state_color(&syntax_state,
				    SYNTAX_COLOR_BLACK);
			}
			break;
		case '\f':
		case '\n':
			syntax_state.off++;
			break;
		default:
			syntax_state.p = &p[syntax_state.off];
			syntax_state.len = towrite - syntax_state.off;

			switch (buf->type) {
			case CE_FILE_TYPE_C:
				syntax_highlight_c(&syntax_state);
				break;
			case CE_FILE_TYPE_PYTHON:
				syntax_highlight_python(&syntax_state);
				break;
			case CE_FILE_TYPE_DIFF:
				syntax_highlight_diff(&syntax_state);
				break;
			case CE_FILE_TYPE_JS:
				syntax_highlight_js(&syntax_state);
				break;
			case CE_FILE_TYPE_SHELL:
				syntax_highlight_shell(&syntax_state);
				break;
			case CE_FILE_TYPE_SWIFT:
				syntax_highlight_swift(&syntax_state);
				break;
			case CE_FILE_TYPE_YAML:
				syntax_highlight_yaml(&syntax_state);
				break;
			case CE_FILE_TYPE_DIRLIST:
				syntax_highlight_dirlist(&syntax_state);
				break;
			case CE_FILE_TYPE_GO:
				syntax_highlight_go(&syntax_state);
				break;
			case CE_FILE_TYPE_LATEX:
				syntax_highlight_latex(&syntax_state);
				break;
			case CE_FILE_TYPE_LUA:
				syntax_highlight_lua(&syntax_state);
				break;
			case CE_FILE_TYPE_ZIG:
				syntax_highlight_zig(&syntax_state);
				break;
			default:
				syntax_state_color_clear(&syntax_state);
				syntax_write(&syntax_state, 1);
				break;
			}
			break;
		}
	}
}

void
ce_syntax_guess(struct cebuf *buf)
{
	size_t			idx;
	const char		*ptr;
	struct celine		*line;

	for (idx = 0; idx < buf->lcnt; idx++) {
		line = &buf->lines[idx];
		ptr = (const char *)line->data;

		if (line->length <= 1)
			continue;

		if (line->length >= 4 && ptr[3] == ' ' &&
		    (!strncmp(ptr, "+++", 3) || !strncmp(ptr, "---", 3))) {
			buf->type = CE_FILE_TYPE_DIFF;
			break;
		}

		if (idx == 0 && line->length >= 10 &&
		    !strncasecmp(ptr, "<!DOCTYPE>", 10)) {
			buf->type = CE_FILE_TYPE_HTML;
			break;
		}
	}
}

static void
syntax_state_selection(struct state *state)
{
	int		prev, color, bold;

	prev = state->selection;
	state->selection = 0;

	if (ce_editor_mode() != CE_EDITOR_MODE_SELECT)
		goto out;

	if (state->col == state->buf->column)
		goto out;

	if (state->buf->selstart.line == state->buf->selend.line &&
	    state->index == state->buf->selstart.line) {
		if (state->col >= state->buf->selstart.col &&
		    state->col <= state->buf->selend.col)
			state->selection = 1;
		goto out;
	}

	if (state->index > state->buf->selstart.line &&
	    state->index < state->buf->selend.line) {
		state->selection = 1;
		goto out;
	}

	if (state->index == state->buf->selstart.line) {
		if (state->col >= state->buf->selstart.col)
			state->selection = 1;
		goto out;
	}

	if (state->index == state->buf->selend.line) {
		if (state->col <= state->buf->selend.col)
			state->selection = 1;
		goto out;
	}

out:
	if (prev != state->selection)
		state->dirty = 1;

	if (state->selection) {
		syntax_state_term_highlight(state);
	} else if (state->highlight) {
		bold = state->bold;
		color = state->color;
		syntax_state_term_reset(state);

		if (bold)
			syntax_state_term_bold(state, 1);
		if (color != -1)
			syntax_state_color(state, color);
	}
}

static void
syntax_state_term_reset(struct state *state)
{
	if (state->dirty) {
		ce_term_attr_off();

		state->bold = 0;
		state->dirty = 0;
		state->color = -1;
		state->r = -1;
		state->g = -1;
		state->b = -1;
		state->highlight = 0;

		if (state->selection) {
			state->highlight = 1;
			ce_term_writestr(TERM_SEQUENCE_ATTR_REVERSE);
		}
	}
}

static void
syntax_state_term_bold(struct state *state, int onoff)
{
	int	color;

	color = state->color;

	if (state->bold != onoff) {
		if (state->bold) {
			syntax_state_term_reset(state);
			if (color != -1)
				syntax_state_color(state, color);
		} else {
			state->dirty = 1;
			ce_term_attr_bold();
		}

		state->bold = onoff;
	}
}

static void
syntax_state_term_highlight(struct state *state)
{
	if (state->highlight == 0) {
		state->dirty = 1;
		state->highlight = 1;
		ce_term_writestr(TERM_SEQUENCE_ATTR_REVERSE);
	}
}

static void
syntax_state_color(struct state *state, int color)
{
	if (state->color == color)
		return;

	syntax_state_foreground_color(state,
	    rgb[color].r, rgb[color].g, rgb[color].b);

	state->color = color;
}

static void
syntax_state_foreground_color(struct state *state, int r, int g, int b)
{
	if (state->r == r && state->g == g && state->b == b)
		return;

	state->dirty = 1;
	state->color = -1;

	state->r = r;
	state->g = g;
	state->b = b;

	ce_term_foreground_rgb(state->r, state->g, state->b);

	if (state->selection && state->highlight == 0) {
		state->highlight = 1;
		ce_term_writestr(TERM_SEQUENCE_ATTR_REVERSE);
	}
}

static void
syntax_state_color_clear(struct state *state)
{
	int		bold;

	bold = state->bold;

	syntax_state_term_reset(state);
	syntax_state_term_bold(state, bold);
}

static void
syntax_highlight_format_string(struct state *state)
{
	size_t		idx;

	syntax_state_color(state, SYNTAX_COLOR_BLACK);
	syntax_state_term_bold(state, 1);
	syntax_write(state, 1);

	for (idx = 1; idx < state->len; idx++) {
		switch (state->p[idx]) {
		case 'c':
		case 'h':
		case 'l':
		case 'j':
		case 't':
		case 'z':
		case 'd':
		case 'i':
		case 'o':
		case 'u':
		case 'x':
		case 'X':
		case 'n':
		case 'p':
		case 's':
		case '*':
		case '.':
			syntax_term_write(state, &state->p[idx], 1, 1);
			break;
		default:
			if (isdigit(state->p[idx])) {
				syntax_term_write(state, &state->p[idx], 1, 1);
			} else {
				syntax_state_term_bold(state, 0);
				return;
			}
		}
	}

	syntax_state_term_bold(state, 0);
}

static int
syntax_highlight_string(struct state *state)
{
	if (state->p[0] != '"' && state->p[0] != '\'' && state->p[0] != '`') {
		if (state->inside_string) {
			switch (state->p[0]) {
			case '%':
				syntax_highlight_format_string(state);
				break;
			case '\\':
				state->keepcolor = 1;
				syntax_state_color(state, SYNTAX_COLOR_MAGENTA);
				syntax_write(state, 1);
				break;
			default:
				if (state->keepcolor == 0) {
					syntax_state_color(state,
					    state->stringcolor);
				} else {
					state->keepcolor--;
				}
				syntax_write(state, 1);
				break;
			}

			return (0);
		}
		return (-1);
	}

	if (state->inside_string == state->p[0] &&
	    syntax_escaped_quote(state) == -1) {
		syntax_state_color(state, state->stringcolor);
		syntax_write(state, 1);
		state->inside_string = 0;
	} else if (state->inside_string == 0) {
		state->inside_string = state->p[0];

		if (state->p[0] == '\'' && state->p[1] == '\\')
			state->stringcolor = SYNTAX_COLOR_MAGENTA;
		else
			state->stringcolor = SYNTAX_COLOR_RED;

		syntax_state_color(state, state->stringcolor);
		syntax_write(state, 1);
	} else {
		syntax_write(state, 1);
	}

	return (0);
}

static int
syntax_highlight_numeric(struct state *state)
{
	const u_int8_t		*end, *p;

	if (!isdigit(*state->p))
		return (-1);

	p = state->p;
	end = state->p + state->len;

	while ((isxdigit(*p) || *p == '.' || *p == 'x') && p < end)
		p++;

	if (syntax_is_word(state, p - state->p) == -1)
		return (-1);

	syntax_state_color(state, SYNTAX_COLOR_RED);
	syntax_write(state, p - state->p);

	return (0);
}

static void
syntax_highlight_c(struct state *state)
{
	if (syntax_highlight_word(state, tags) == 0)
		return;

	if (syntax_highlight_c_comment(state) == 0)
		return;

	if (syntax_highlight_c_preproc(state) == 0)
		return;

	if (syntax_highlight_c_label(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	if (syntax_highlight_word(state, c_kw) == 0)
		return;

	if (syntax_highlight_word(state, c_type) == 0)
		return;

	if (syntax_highlight_word(state, c_special) == 0)
		return;

	if (state->p[0] == ' ' && state->p[1] == '\n') {
		syntax_state_color(state, SYNTAX_COLOR_BLUE);
		syntax_term_write(state, ".", 1, 1);
	} else {
		syntax_state_color_clear(state);
		syntax_write(state, 1);
	}
}

static int
syntax_highlight_c_comment(struct state *state)
{
	if (state->inside_string)
		return (-1);

	if (state->inside_comment == 0) {
		if (state->len >= 2 &&
		    state->p[0] == '/' && state->p[1] == '/') {
			state->inside_comment = 1;
			syntax_state_color(state, SYNTAX_COLOR_COMMENT);
			syntax_write(state, 2);
			state->flags |= SYNTAX_CLEAR_COMMENT;
			return (0);
		}

		if (state->len >= 2 &&
		    state->p[0] == '/' && state->p[1] == '*') {
			state->inside_comment = 1;
			syntax_state_color(state, SYNTAX_COLOR_COMMENT);
			syntax_write(state, 2);
			return (0);
		}
	} else {
		if (state->len >= 2 &&
		    state->p[0] == '*' && state->p[1] == '/') {
			syntax_write(state, 2);
			state->inside_comment = 0;
			return (0);
		}

		syntax_state_color(state, SYNTAX_COLOR_COMMENT);
		syntax_write(state, 1);
		return (0);
	}

	return (-1);
}

static int
syntax_highlight_c_label(struct state *state)
{
	if (state->off == 0 && state->len > 2 &&
	    state->p[state->len - 2] == ':') {
		syntax_state_color(state, SYNTAX_COLOR_TEAL);
		syntax_write(state, state->len - 2);
		return (0);
	}

	return (-1);
}

static int
syntax_highlight_c_preproc(struct state *state)
{
	const char		*w;
	const u_int8_t		*p, *end;

	if (state->inside_preproc) {
		w = (const char *)state->ppword;

		if (!strncmp(w, "#include", state->ppwlen)) {
			if (state->p[0] == '<') {
				syntax_highlight_span(state,
				    '<', '>', SYNTAX_COLOR_RED);
				syntax_state.ppwlen = 0;
				syntax_state.ppword = NULL;
				syntax_state.inside_preproc = 0;
				return (0);
			}
		}

		if (state->p[0] == '"') {
			syntax_highlight_span(state, '"', '"',
			    SYNTAX_COLOR_RED);
			return (0);
		}

		if (syntax_highlight_numeric(state) == 0)
			return (0);

		if (syntax_highlight_word(state, c_kw) == 0)
			return (0);

		if (syntax_highlight_word(state, c_type) == 0)
			return (0);

		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		syntax_write(state, 1);

		if (state->off == state->avail - 1) {
			if (state->p[0] != '\\') {
				syntax_state_color_clear(state);
				state->inside_preproc = 0;
			}
		}

		return (0);
	}

	if (state->off == 0 && state->p[0] == '#') {
		state->inside_preproc = 1;

		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		syntax_write(state, 1);

		state->ppword = NULL;
		state->ppwlen = 0;

		p = state->p;
		end = p + state->len;

		while (!isspace(*p) && p < end)
			p++;

		state->ppword = state->p;
		state->ppwlen = p - state->p;

		return (0);
	}

	return (-1);
}

static void
syntax_highlight_python(struct state *state)
{
	if (syntax_highlight_word(state, tags) == 0)
		return;

	if (syntax_highlight_pound_comment(state) == 0)
		return;

	if (syntax_highlight_python_multiline_string(state) == 0)
		return;

	if (syntax_highlight_python_decorator(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	if (syntax_highlight_word(state, py_kw) == 0)
		return;

	if (syntax_highlight_word(state, py_types) == 0)
		return;

	if (syntax_highlight_word(state, py_special) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static int
syntax_highlight_python_decorator(struct state *state)
{
	size_t		idx;

	if (state->off == 0 && state->p[0] == '@' &&
	    state->len > 1 && !isspace(state->p[1])) {
		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		syntax_write(state, 1);

		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		for (idx = 1; idx < state->len - 1; idx++) {
			if (state->p[idx] == '(')
				return (0);
			syntax_term_write(state, &state->p[idx], 1, 1);
		}

		return (0);
	}

	return (-1);
}

static int
syntax_highlight_python_multiline_string(struct state *state)
{
	int		hit;

	if (state->len >= 3 && state->p[0] == '"' &&
	    state->p[1] == '"' && state->p[2] == '"') {
		hit = 1;
	} else {
		hit = 0;
	}

	if (state->inside_string == 0) {
		if (hit) {
			state->inside_string = 0xff;
			syntax_state_color(state, SYNTAX_COLOR_RED);
			syntax_write(state, 3);
			return (0);
		}
	} else if (state->inside_string == 0xff) {
		if (hit) {
			state->inside_string = 0;
			syntax_write(state, 3);
		} else {
			syntax_write(state, 1);
		}

		return (0);
	}

	return (-1);
}

static void
syntax_highlight_diff(struct state *state)
{
	if (state->off > 0) {
		if (state->diffcolor != -1)
			syntax_state_color(state, state->diffcolor);
		else
			syntax_state_color_clear(state);
		syntax_write(state, 1);
		return;
	}

	if (state->p[0] == '+' && state->p[1] != '+') {
		state->diffcolor = SYNTAX_COLOR_TEAL;
		syntax_state_color(state, SYNTAX_COLOR_TEAL);
		syntax_write(state, 1);
		return;
	} else if (state->len > 3 && state->p[1] == '+' && state->p[2] == '+') {
		state->diffcolor = SYNTAX_COLOR_TEAL;
		syntax_state_color(state, SYNTAX_COLOR_TEAL);
		syntax_write(state, 1);
		return;
	}

	if (state->p[0] == '-' && state->p[1] != '-') {
		state->diffcolor = SYNTAX_COLOR_RED;
		syntax_state_color(state, SYNTAX_COLOR_RED);
		syntax_write(state, 1);
		return;
	} else if (state->len > 3 && state->p[1] == '-' && state->p[2] == '-') {
		state->diffcolor = SYNTAX_COLOR_RED;
		syntax_state_color(state, SYNTAX_COLOR_RED);
		syntax_write(state, 1);
		return;
	}

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static void
syntax_highlight_js(struct state *state)
{
	if (syntax_highlight_word(state, tags) == 0)
		return;

	if (syntax_highlight_c_comment(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	if (syntax_highlight_word(state, js_kw) == 0)
		return;

	if (syntax_highlight_word(state, js_other) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static void
syntax_highlight_go(struct state *state)
{
	if (syntax_highlight_word(state, tags) == 0)
		return;

	if (syntax_highlight_c_comment(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	if (syntax_highlight_word(state, go_kw) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static void
syntax_highlight_latex(struct state *state)
{
	size_t		len;
	int		prev, bold;

	len = 1;
	bold = state->bold;
	prev = state->color;

	switch (state->p[0]) {
	case '{':
	case '[':
		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		syntax_state_term_bold(state, 1);
		for (len = 0; len < state->len; len++) {
			if (state->p[len] == state->p[0] + 2) {
				len++;
				break;
			}
		}
		break;
	case '\\':
		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		for (len = 0; len < state->len; len++) {
			if (ce_editor_word_byte(state->p[len]) == 0)
				break;
		}
		break;
	default:
		syntax_state_color_clear(state);
		syntax_write(state, 1);
		return;
	}

	syntax_write(state, len);

	if (prev != -1)
		syntax_state_color(state, prev);
	else
		syntax_state_color_clear(state);

	if (bold != state->bold)
		syntax_state_term_bold(state, bold);
}

static void
syntax_highlight_lua(struct state *state)
{
	if (syntax_highlight_word(state, lua_kw) == 0)
		return;

	if (syntax_highlight_lua_comment(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static int
syntax_highlight_lua_comment(struct state *state)
{
	if (state->inside_string)
		return (-1);

	if (state->len >= 2 &&
	    state->p[0] == '-' && state->p[1] == '-') {
		syntax_state_color(state, SYNTAX_COLOR_COMMENT);
		syntax_write(state, state->len - 1);
		state->flags |= SYNTAX_CLEAR_COMMENT;
		return (0);
	}

	return (-1);
}

static void
syntax_highlight_zig(struct state *state)
{
	if (syntax_highlight_c_comment(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	if (syntax_highlight_word(state, zig_kw) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static void
syntax_highlight_shell(struct state *state)
{
	if (syntax_highlight_word(state, tags) == 0)
		return;

	if (syntax_highlight_pound_comment(state) == 0)
		return;

	if (syntax_highlight_shell_variable(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	if (syntax_highlight_word(state, sh_kw) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static int
syntax_highlight_shell_variable(struct state *state)
{
	size_t		len;
	int		prev;

	if (state->p[0] == '$') {
		prev = state->color;
		syntax_state_color(state, SYNTAX_COLOR_BLACK);

		for (len = 0; len < state->len; len++) {
			if (state->p[len] == '{' ||
			    state->p[len] == '}' ||
			    state->p[len] == '$')
				continue;

			if (ce_editor_word_byte(state->p[len]) == 0)
				break;
		}

		syntax_write(state, len);

		if (prev != -1)
			syntax_state_color(state, prev);
		else
			syntax_state_color_clear(state);

		return (0);
	}

	return (-1);
}

static void
syntax_highlight_span(struct state *state, char start, char end, int color)
{
	const u_int8_t		*p;
	size_t			len;

	p = state->p;
	if (*p != start)
		fatal("%s: p (0x%02x) != start (0x%02x)", __func__, *p, start);

	len = 1;
	syntax_state_color(state, color);

	while (len < state->len) {
		if (p[len] == '\n')
			break;

		if (p[len] == end) {
			len++;
			break;
		}

		len++;
	}

	syntax_write(state, len);
}

static int
syntax_highlight_word(struct state *state, const char *words[])
{
	size_t		len;
	int		i, bold;

	for (i = 0; words[i] != NULL; i++) {
		if (*state->p != *words[i])
			continue;

		len = strlen(words[i]);
		if (len > state->len)
			continue;

		if (memcmp(state->p, words[i], len))
			continue;

		if (syntax_is_word(state, len) == -1)
			continue;

		bold = state->bold;

		syntax_state_term_bold(state, 1);

		if (words == tags)
			syntax_state_foreground_color(state, 64, 192, 192);
		else
			syntax_state_foreground_color(state, 52, 139, 115);

		syntax_term_write(state, words[i], len, 1);

		if (!bold)
			syntax_state_term_bold(state, 0);

		return (0);
	}

	return (-1);
}

static int
syntax_highlight_pound_comment(struct state *state)
{
	if (state->inside_comment == 0) {
		if (state->p[0] == '#' && state->inside_string == 0) {
			state->inside_comment = 1;
			syntax_state_color(state, SYNTAX_COLOR_COMMENT);
			syntax_write(state, 1);
			state->flags |= SYNTAX_CLEAR_COMMENT;
			return (0);
		}
	} else {
		syntax_write(state, 1);
		return (0);
	}

	return (-1);
}

static int
syntax_escaped_quote(struct state *state)
{
	if (state->off == 0)
		return (-1);

	switch (state->p[0]) {
	case '\'':
	case '"':
		if (state->p[-1] == '\\') {
			if (state->off > 2 && state->p[-2] == '\\')
				return (-1);
			return (0);
		}
		break;
	}

	return (-1);
}

static int
syntax_is_word(struct state *state, size_t hlen)
{
	const u_int8_t		*next, *prev;

	if (hlen >= state->len)
		next = NULL;
	else
		next = (state->p + hlen);

	if (state->off == 0)
		prev = NULL;
	else
		prev = (state->p - 1);

	if (prev && ce_editor_word_separator(*prev) == 0)
		return (-1);

	if (next && ce_editor_word_separator(*next) == 0)
		return (-1);

	return (0);
}

static void
syntax_write(struct state *state, size_t len)
{
	size_t		seqlen;

	if (len > state->len) {
		fatal("%s: invalid write %zu > %zu",
		    __func__, len, state->len);
	}

	if (len == 1) {
		if (ce_utf8_sequence(state->p, state->len, 0, &seqlen) == 0)
			seqlen = 1;
	} else {
		seqlen = len;
	}

	syntax_term_write(state, state->p, seqlen, 1);
}

static void
syntax_term_write(struct state *state, const void *data, size_t len, int count)
{
	const u_int8_t		*ptr;
	size_t			off, seqlen;

	off = 0;
	ptr = data;

	while (off != len) {
		syntax_state_selection(state);

		if (ce_utf8_sequence(ptr, len, off, &seqlen) == 0)
			seqlen = 1;

		ce_term_write(&ptr[off], seqlen);
		off += seqlen;

		if (count) {
			state->col++;
			state->off += seqlen;
		}
	}
}

static void
syntax_highlight_swift(struct state *state)
{
	if (syntax_highlight_c_comment(state) == 0)
		return;

	if (syntax_highlight_word(state, swift_kw) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	if (syntax_highlight_python_decorator(state) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static void
syntax_highlight_yaml(struct state *state)
{
	if (syntax_highlight_c_comment(state) == 0)
		return;

	if (syntax_highlight_pound_comment(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_string(state) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static void
syntax_highlight_dirlist(struct state *state)
{
	mode_t		mode;

	switch (state->index) {
	case 0:
		syntax_state_term_bold(state, 1);
		syntax_write(state, state->len - 1);
		syntax_state_term_bold(state, 0);
		goto out;
	case 1:
		syntax_state_term_bold(state, 1);
		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		syntax_write(state, state->len - 1);
		syntax_state_color_clear(state);
		syntax_state_term_bold(state, 0);
		goto out;
	case 2:
		syntax_write(state, state->len - 1);
		goto out;
	}

	mode = ce_dirlist_index2mode(state->buf, state->index - 3);

	if (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
		syntax_state_term_bold(state, 1);
		syntax_state_color(state, SYNTAX_COLOR_BLACK);
		syntax_write(state, state->len - 1);
		syntax_state_term_bold(state, 0);
		syntax_state_color_clear(state);
		goto out;
	}

	syntax_write(state, state->len - 1);

out:
	state->off++;
}
