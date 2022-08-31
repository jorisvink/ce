/*
 * Copyright (c) 2021 Joris Vink <joris@coders.se>
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
#include <sys/file.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

static int	game_file_open(struct cegame *);
static void	game_file_flush(int, struct cegame *);

static const char *title_names[] = {
	"Scout",
	"Grunt",
	"Sergeant",
	"Stone Guard",
	"Blood Guard",
	"Legionnaire",
	"Centurion",
	"Champion",
	"General",
	"Warlord"
};

static const char *rank_names[] = {
	"I",
	"II",
	"III",
	"IV",
	"V",
	"VI",
	"VII",
	"VIII",
	"IX",
	"X"
};

void
ce_game_init(void)
{
	ce_game_add_open();
	ce_game_add_xp();
}

void
ce_game_add_xp(void)
{
	struct cegame	game;

	memset(&game, 0, sizeof(game));

	game.xp = CE_XP_PER_AWARD;

	ce_game_update(&game);
}

void
ce_game_add_open(void)
{
	struct cegame	game;

	memset(&game, 0, sizeof(game));

	game.opens = 1;

	ce_game_update(&game);
}

void
ce_game_update(struct cegame *delta)
{
	int		fd;
	struct cegame	game;

	if ((fd = game_file_open(&game)) == -1)
		return;

	game.xp += delta->xp;
	game.opens += delta->opens;

	game_file_flush(fd, &game);
}

u_int32_t
ce_game_xp(void)
{
	int		fd;
	struct cegame	game;

	if ((fd = game_file_open(&game)) == -1)
		return (0);

	close(fd);

	return (game.xp);
}

u_int32_t
ce_game_level(void)
{
	int		fd;
	struct cegame	game;

	if ((fd = game_file_open(&game)) == -1)
		return (0);

	close(fd);

	return ((sqrt(game.xp) / (CE_XP_INITIAL / 10)) - CE_XP_GROWTH);
}

u_int32_t
ce_game_open_count(void)
{
	int		fd;
	struct cegame	game;

	if ((fd = game_file_open(&game)) == -1)
		return (0);

	close(fd);

	return (game.opens);
}

u_int32_t
ce_game_xp_required(u_int32_t level)
{
	return (CE_XP_INITIAL * pow((level + CE_XP_GROWTH), 2));
}

const char *
ce_game_level_name(void)
{
	int		len;
	static char	name[48];
	u_int32_t	level, title, rank;

	level = ce_game_level();

	if (level >= 100) {
		len = snprintf(name, sizeof(name),
		    "High Warlord (prestige lvl %u)", level);
	} else {
		title = level / 10;
		rank = (level % 10);
		if (rank > 0)
			rank--;

		len = snprintf(name, sizeof(name), "%s %s (lvl %u)",
		    title_names[title], rank_names[rank], level);
	}

	if (len == -1 || (size_t)len >= sizeof(name))
		fatal("failed to construct level name");

	return (name);
}

static int
game_file_open(struct cegame *game)
{
	struct stat	st;
	ssize_t		ret;
	char		path[PATH_MAX];
	int		fd, len, tries;

	memset(game, 0, sizeof(*game));

	len = snprintf(path, sizeof(path), "%s/.cegame", ce_editor_home());
	if (len == -1 || (size_t)len >= sizeof(path))
		fatal("failed to construct path to xp file");

	if ((fd = open(path, O_CREAT | O_RDWR, 0600)) == -1) {
		ce_editor_message("cannot open game file: %s", errno_s);
		return (-1);
	}

	tries = 0;
	while (flock(fd, LOCK_EX) == -1 && tries < 5) {
		usleep(50000);
		tries++;
	}

	if (tries == 5) {
		ce_editor_message("cannot lock game file");
		(void)close(fd);
		return (-1);
	}

	if (fstat(fd, &st) == -1) {
		ce_editor_message("cannot fstat game file. %s", errno_s);
		(void)close(fd);
		return (-1);
	}

	if (st.st_size == 0) {
		game->xp = ce_game_xp_required(1);
		return (fd);
	}

	if ((size_t)st.st_size < sizeof(*game)) {
		ce_editor_message("game file is corrupted");
		(void)close(fd);
		return (-1);
	}

	ret = read(fd, game, sizeof(*game));
	if (ret == -1) {
		ce_editor_message("failed to read game file: %s", errno_s);
		(void)close(fd);
		return (-1);
	}

	if ((size_t)ret != sizeof(*game)) {
		ce_editor_message("did not read all game bytes");
		(void)close(fd);
		return (-1);
	}

	return (fd);
}

static void
game_file_flush(int fd, struct cegame *game)
{
	ssize_t		ret;

	if (lseek(fd, 0, SEEK_SET) == -1) {
		ce_editor_message("failed to rewind game file: %s", errno_s);
		(void)close(fd);
		return;
	}

	ret = write(fd, game, sizeof(*game));
	if (ret == -1) {
		ce_editor_message("failed to write game file: %s", errno_s);
		(void)close(fd);
		return;
	}

	if ((size_t)ret != sizeof(*game)) {
		ce_editor_message("game file write corrupted");
		(void)close(fd);
		return;
	}

	if (close(fd) == -1) {
		ce_editor_message("failed to update game file: %s", errno_s);
		return;
	}
}
