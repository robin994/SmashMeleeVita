#pragma once

#include <stdio.h>

/* Returns 0 after the full movie, 1 when START/A skips to title, 2 for
 * SELECT+START application exit, and a negative value on decode/I/O failure. */
int mv_opening_movie_run(FILE *log);
