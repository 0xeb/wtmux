/* $OpenBSD$ */

/*
 * Copyright (c) 2026 arndawg <https://github.com/arndawg>
 * Copyright (c) 2026 Elias Bachaalany <elias.bachaalany@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"

static const struct win32_terminfo_entry win32_terminfo[] = {
    {"am", NULL, 0, 1, 3},
    {"bce", NULL, 0, 1, 3},
    {"AX", NULL, 0, 1, 3},
    {"hs", NULL, 0, 0, 3},
    {"mir", NULL, 0, 1, 3},
    {"xenl", NULL, 0, 1, 3},
    {"XT", NULL, 0, 1, 3},

    {"colors", NULL, 256, 0, 2}, {"cols", NULL, 80, 0, 2},
    {"lines", NULL, 24, 0, 2}, {"pairs", NULL, 32767, 0, 2},

    {"cup", "\033[%i%p1%d;%p2%dH", 0, 0, 1},
    {"cub1", "\010", 0, 0, 1},
    {"cub", "\033[%p1%dD", 0, 0, 1},
    {"cud1", "\012", 0, 0, 1},
    {"cud", "\033[%p1%dB", 0, 0, 1},
    {"cuf1", "\033[C", 0, 0, 1},
    {"cuf", "\033[%p1%dC", 0, 0, 1},
    {"cuu1", "\033[A", 0, 0, 1},
    {"cuu", "\033[%p1%dA", 0, 0, 1},
    {"home", "\033[H", 0, 0, 1},
    {"hpa", "\033[%i%p1%dG", 0, 0, 1},

    {"csr", "\033[%i%p1%d;%p2%dr", 0, 0, 1},
    {"indn", "\033[%p1%dS", 0, 0, 1},
    {"ind", "\012", 0, 0, 1},
    {"ri", "\033M", 0, 0, 1},
    {"rin", "\033[%p1%dT", 0, 0, 1},

    {"clear", "\033[H\033[2J", 0, 0, 1},
    {"ed", "\033[J", 0, 0, 1},
    {"el", "\033[K", 0, 0, 1},
    {"el1", "\033[1K", 0, 0, 1},
    {"ech", "\033[%p1%dX", 0, 0, 1},
    {"dch1", "\033[P", 0, 0, 1},
    {"dch", "\033[%p1%dP", 0, 0, 1},
    {"dl1", "\033[M", 0, 0, 1},
    {"dl", "\033[%p1%dM", 0, 0, 1},
    {"ich", "\033[%p1%d@", 0, 0, 1},
    {"il1", "\033[L", 0, 0, 1},
    {"il", "\033[%p1%dL", 0, 0, 1},

    {"bold", "\033[1m", 0, 0, 1},
    {"dim", "\033[2m", 0, 0, 1},
    {"sitm", "\033[3m", 0, 0, 1},
    {"smul", "\033[4m", 0, 0, 1},
    {"blink", "\033[5m", 0, 0, 1},
    {"rev", "\033[7m", 0, 0, 1},
    {"invis", "\033[8m", 0, 0, 1},
    {"smxx", "\033[9m", 0, 0, 1},
    {"sgr0", "\033(B\033[m", 0, 0, 1},
    {"ritm", "\033[23m", 0, 0, 1},
    {"rmul", "\033[24m", 0, 0, 1},
    {"rmxx", "\033[29m", 0, 0, 1},

    {"setaf",
        "\033[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m", 0,
        0, 1},
    {"setab",
        "\033[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m", 0,
        0, 1},
    {"op", "\033[39;49m", 0, 0, 1},

    {"civis", "\033[?25l", 0, 0, 1},
    {"cnorm", "\033[?12l\033[?25h", 0, 0, 1},
    {"cvvis", "\033[?12;25h", 0, 0, 1},

    {"smcup", "\033[?1049h\033[22;0;0t", 0, 0, 1},
    {"rmcup", "\033[?1049l\033[23;0;0t", 0, 0, 1},

    {"smkx", "\033[?1h\033=", 0, 0, 1},
    {"rmkx", "\033[?1l\033>", 0, 0, 1},

    {"ht", "\011", 0, 0, 1},
    {"tbc", "\033[3g", 0, 0, 1},
    {"hts", "\033H", 0, 0, 1},

    {"bel", "\007", 0, 0, 1},

    {"cr", "\015", 0, 0, 1},
    {"E3", "\033[3J", 0, 0, 1},
    {"enacs", "", 0, 0, 1},
    {"smacs", "\033(0", 0, 0, 1},
    {"rmacs", "\033(B", 0, 0, 1},

    {"acsc", "``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~", 0, 0, 1},

    {"tsl", "\033]0;", 0, 0, 1},
    {"fsl", "\007", 0, 0, 1},

    {"kcub1", "\033OD", 0, 0, 1},
    {"kcuf1", "\033OC", 0, 0, 1},
    {"kcuu1", "\033OA", 0, 0, 1},
    {"kcud1", "\033OB", 0, 0, 1},
    {"kbs", "\177", 0, 0, 1},
    {"kdch1", "\033[3~", 0, 0, 1},
    {"khome", "\033OH", 0, 0, 1},
    {"kend", "\033OF", 0, 0, 1},
    {"kpp", "\033[5~", 0, 0, 1},
    {"knp", "\033[6~", 0, 0, 1},
    {"kich1", "\033[2~", 0, 0, 1},
    {"kcbt", "\033[Z", 0, 0, 1},
    {"kf1", "\033OP", 0, 0, 1}, {"kf2", "\033OQ", 0, 0, 1},
    {"kf3", "\033OR", 0, 0, 1}, {"kf4", "\033OS", 0, 0, 1},
    {"kf5", "\033[15~", 0, 0, 1}, {"kf6", "\033[17~", 0, 0, 1},
    {"kf7", "\033[18~", 0, 0, 1}, {"kf8", "\033[19~", 0, 0, 1},
    {"kf9", "\033[20~", 0, 0, 1}, {"kf10", "\033[21~", 0, 0, 1},
    {"kf11", "\033[23~", 0, 0, 1}, {"kf12", "\033[24~", 0, 0, 1},

    {"Enbp", "\033[?2004h", 0, 0, 1}, {"Dsbp", "\033[?2004l", 0, 0, 1},

    {"Eneks", "\033[>4;1m", 0, 0, 1}, {"Dseks", "\033[>4;0m", 0, 0, 1},

    {"Enfcs", "\033[?1004h", 0, 0, 1}, {"Dsfcs", "\033[?1004l", 0, 0, 1},

    {"kmous", "\033[M", 0, 0, 1},

    {"smso", "\033[7m", 0, 0, 1},
    {"rmso", "\033[27m", 0, 0, 1},
    {"vpa", "\033[%i%p1%dd", 0, 0, 1},

    {"Ss", "\033[%p1%d q", 0, 0, 1}, {"Se", "\033[2 q", 0, 0, 1},
    {"Cs", "\033]12;%p1%s\007", 0, 0, 1}, {"Cr", "\033]112\007", 0, 0, 1},

    {"Cmg", "\033[%i%p1%d;%p2%ds", 0, 0, 1}, {"Clmg", "\033[s", 0, 0, 1},
    {"Enmg", "\033[?69h", 0, 0, 1}, {"Dsmg", "\033[?69l", 0, 0, 1},

    {"Hls", "\033]8;%p1%s;%p2%s\033\\", 0, 0, 1},

    {"Smulx", "\033[4:%p1%dm", 0, 0, 1}, {"Smol", "\033[53m", 0, 0, 1},
    {"Spb", "\033]9;4;%p1%d;%p2%d\033\\", 0, 0, 1},

    {NULL, NULL, 0, 0, 0}};

const struct win32_terminfo_entry *
win32_terminfo_find(const char *name)
{
	const struct win32_terminfo_entry *e;

	for (e = win32_terminfo; e->name != NULL; e++) {
		if (strcmp(e->name, name) == 0)
			return (e);
	}
	return (NULL);
}

u_int
win32_terminfo_count(void)
{
	u_int n = 0;
	const struct win32_terminfo_entry *e;

	for (e = win32_terminfo; e->name != NULL; e++)
		n++;
	return (n);
}

#include "win32-terminfo-ncurses.h"

const struct win32_terminfo_entry *
win32_terminfo_table(const char *term, u_int *count)
{
	const struct win32_terminfo_entry	*table = win32_terminfo, *e;
	u_int					 n = 0;

	if (strncmp(term, "screen", 6) == 0) {
		if (strstr(term, "256color") != NULL)
			table = win32_terminfo_screen_256color;
		else
			table = win32_terminfo_screen;
	} else if (strncmp(term, "tmux", 4) == 0) {
		if (strstr(term, "256color") != NULL)
			table = win32_terminfo_tmux_256color;
		else
			table = win32_terminfo_tmux;
	}
	if (count != NULL) {
		for (e = table; e->name != NULL; e++)
			n++;
		*count = n;
	}
	return (table);
}