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
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ce.h"

#define SYNTAX_CLEAR_COMMENT	0x0001

struct state {
	const u_int8_t	*p;
	size_t		len;
	size_t		off;

	int		inside_literal;
	int		inside_comment;
	int		inside_preproc;

	int		color;
	int		color_prev;

	u_int32_t	flags;
};

static void	syntax_write(struct state *, size_t);
static int	syntax_is_word(struct state *, size_t);
static void	syntax_state_color(struct state *, int);
static void	syntax_state_color_reset(struct state *);
static void	syntax_state_color_clear(struct state *);

static void	syntax_highlight_c(struct state *);
static int	syntax_highlight_c_comment(struct state *);
static int	syntax_highlight_c_preproc(struct state *);

static int	syntax_highlight_numeric(struct state *);
static int	syntax_highlight_literal(struct state *);
static int	syntax_highlight_word(struct state *, const char *[], int);

static const char *c_kw[] = {
	"if", "do", "for", "else", "while", "return", "sizeof",
	"case", "switch", "default", "break", "goto", "continue",
	NULL
};

static const char *c_types[] = {
	"int", "char", "short", "long", "double", "float", "size_t",
	"ssize_t", "const", "struct", "static", "unsigned", "void",
	"uint8_t", "uint16_t", "uint32_t", "uint64_t",
	"int8_t", "int16_t", "int32_t", "int64_t",
	"u_int8_t", "u_int16_t", "u_int32_t", "u_int64_t",
	NULL
};

static const char *c_special[] = {
	"NULL",
	NULL
};

static struct state	syntax_state = { 0 };

void
ce_syntax_init(void)
{
	memset(&syntax_state, 0, sizeof(syntax_state));

	syntax_state.color = -1;
	syntax_state.color_prev = -1;

	ce_term_reset();
}

void
ce_syntax_write(struct cebuf *buf, struct celine *line, size_t towrite)
{
	const u_int8_t		*p;
	size_t			col, spaces, i;

	col = 1;
	p = line->data;

	syntax_state.off = 0;
	syntax_state.inside_preproc = 0;
	syntax_state.inside_literal = 0;

	if (syntax_state.flags & SYNTAX_CLEAR_COMMENT) {
		syntax_state.flags &= ~SYNTAX_CLEAR_COMMENT;
		syntax_state.inside_comment = 0;

		ce_term_reset();
		syntax_state.color = -1;
		syntax_state.color_prev = -1;
	}

	while (syntax_state.off != line->length) {
		switch (p[syntax_state.off]) {
		case '\t':
			syntax_state_color(&syntax_state, TERM_COLOR_BLUE);

			if ((col % 8) == 0)
				spaces = 1;
			else
				spaces = 8 - (col % 8) + 1;

			col += spaces;

			ce_term_write(">", 1);
			for (i = 1; i < spaces; i++)
				ce_term_write(".", 1);

			syntax_state.off++;
			break;
		default:
			spaces = syntax_state.off;

			syntax_state.p = &p[syntax_state.off];
			syntax_state.len = line->length - syntax_state.off;

			switch (buf->type) {
			case CE_FILE_TYPE_C:
				syntax_highlight_c(&syntax_state);
				break;
			default:
				syntax_state_color_clear(&syntax_state);
				syntax_write(&syntax_state, 1);
				break;
			}

			col += syntax_state.off - spaces;
			break;
		}
	}
}

static void
syntax_state_color(struct state *state, int color)
{
	if (state->color != color) {
		ce_term_color(color + TERM_COLOR_FG);
		state->color_prev = state->color;
		state->color = color;
	}
}

static void
syntax_state_color_clear(struct state *state)
{
	if (state->color != -1)
		ce_term_reset();

	state->color = -1;
	state->color_prev = -1;
}

static void
syntax_state_color_reset(struct state *state)
{
	if (state->color_prev != -1) {
		syntax_state_color(state, state->color_prev);
		state->color_prev = -1;
	} else if (state->color != -1) {
		ce_term_reset();
		state->color = -1;
	}
}

static int
syntax_highlight_literal(struct state *state)
{
	if (state->p[0] != '"' && state->p[0] != '\'') {
		if (state->inside_literal) {
			syntax_write(state, 1);
			return (0);
		}
		return (-1);
	}

	if (state->inside_literal == state->p[0]) {
		syntax_write(state, 1);
		syntax_state_color_reset(state);
		state->inside_literal = 0;
	} else if (state->inside_literal == 0) {
		state->inside_literal = state->p[0];
		syntax_state_color(state, TERM_COLOR_RED);
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

	syntax_state_color(state, TERM_COLOR_RED);
	syntax_write(state, p - state->p);

	return (0);
}

static void
syntax_highlight_c(struct state *state)
{
	if (syntax_highlight_c_comment(state) == 0)
		return;

	if (syntax_highlight_c_preproc(state) == 0)
		return;

	if (syntax_highlight_numeric(state) == 0)
		return;

	if (syntax_highlight_literal(state) == 0)
		return;

	if (syntax_highlight_word(state, c_kw, TERM_COLOR_YELLOW) == 0)
		return;

	if (syntax_highlight_word(state, c_types, TERM_COLOR_GREEN) == 0)
		return;

	if (syntax_highlight_word(state, c_special, TERM_COLOR_RED) == 0)
		return;

	syntax_state_color_clear(state);
	syntax_write(state, 1);
}

static int
syntax_highlight_c_comment(struct state *state)
{
	if (state->inside_comment == 0) {
		if (state->len >= 2 &&
		    state->p[0] == '/' && state->p[1] == '/') {
			state->inside_comment = 1;
			ce_term_writestr(TERM_SEQUENCE_ATTR_BOLD);
			syntax_state_color(state, TERM_COLOR_BLUE);
			syntax_write(state, 2);
			state->flags |= SYNTAX_CLEAR_COMMENT;
			return (0);
		}

		if (state->len >= 2 &&
		    state->p[0] == '/' && state->p[1] == '*') {
			state->inside_comment = 1;
			ce_term_writestr(TERM_SEQUENCE_ATTR_BOLD);
			syntax_state_color(state, TERM_COLOR_BLUE);
			syntax_write(state, 2);
			return (0);
		}
	} else {
		if (state->len >= 2 &&
		    state->p[0] == '*' && state->p[1] == '/') {
			syntax_write(state, 2);
			state->inside_comment = 0;
			syntax_state_color_reset(state);
			return (0);
		}

		syntax_write(state, 1);
		return (0);
	}

	return (-1);
}

static int
syntax_highlight_c_preproc(struct state *state)
{
	if (state->inside_preproc) {
		if (syntax_highlight_numeric(state) == -1 &&
		    syntax_highlight_literal(state) == -1) {
			syntax_state_color(state, TERM_COLOR_MAGENTA);
			syntax_write(state, 1);
		}
		return (0);
	}

	if (state->off == 0 && state->p[0] == '#') {
		syntax_state_color(state, TERM_COLOR_MAGENTA);
		syntax_write(state, 1);
		state->inside_preproc = 1;
		return (0);
	}

	return (-1);
}

static int
syntax_highlight_word(struct state *state, const char *words[], int color)
{
	int		i;
	size_t		len;

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

		syntax_state_color(state, color);
		ce_term_writestr(words[i]);

		state->off = state->off + len;
		return (0);
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

	if (prev) {
		switch (*prev) {
		case ' ':
		case '(':
		case ')':
		case '{':
		case '\t':
		case '\n':
		case ':':
		case ';':
		case ',':
			break;
		default:
			return (-1);
		}
	}

	if (next) {
		switch (*next) {
		case ' ':
		case '(':
		case ')':
		case '{':
		case '\t':
		case '\n':
		case ':':
		case ';':
		case ',':
			break;
		default:
			return (-1);
		}
	}

	return (0);
}

static void
syntax_write(struct state *state, size_t len)
{
	if (len > state->len)
		fatal("%s: invalid write", __func__);

	ce_term_write(state->p, len);
	state->off += len;
}
