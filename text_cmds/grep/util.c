/*	$NetBSD: util.c,v 1.9 2011/02/27 17:33:37 joerg Exp $	*/
/*	$FreeBSD: src/usr.bin/grep/util.c,v 1.19 2011/12/07 12:25:28 gabor Exp $	*/
/*	$OpenBSD: util.c,v 1.39 2010/07/02 22:18:03 tedu Exp $	*/

/*-
 * Copyright (c) 1999 James Howard and Dag-Erling Coïdan Smørgrav
 * Copyright (C) 2008-2010 Gabor Kovesdan <gabor@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/usr.bin/grep/util.c,v 1.19 2011/12/07 12:25:28 gabor Exp $");

#include <sys/stat.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <sys/param.h>
#endif

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fnmatch.h>
#include <fts.h>
#include <libgen.h>
#ifdef __APPLE__
#include <locale.h>
#endif /* __APPLE__ */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#ifndef WITHOUT_FASTMATCH
#include "fastmatch.h"
#endif
#include "grep.h"
#include "ios_error.h"

static __thread int	 linesqueued;
static int	 procline(struct str *l, int);

bool
file_matching(const char *fname)
{
	char *fname_base;
	bool ret;

	ret = finclude ? false : true;
#ifdef __APPLE__
	fname_base = basename((char *)fname);
#else
	fname_base = basename(fname);
#endif

	for (unsigned int i = 0; i < fpatterns; ++i) {
		if (fnmatch(fpattern[i].pat, fname, 0) == 0 ||
		    fnmatch(fpattern[i].pat, fname_base, 0) == 0) {
			if (fpattern[i].mode == EXCL_PAT)
				return (false);
			else
				ret = true;
		}
	}
	return (ret);
}

static inline bool
dir_matching(const char *dname)
{
	bool ret;

	ret = dinclude ? false : true;

	for (unsigned int i = 0; i < dpatterns; ++i) {
		if (dname != NULL &&
		    fnmatch(dpattern[i].pat, dname, 0) == 0) {
			if (dpattern[i].mode == EXCL_PAT)
				return (false);
			else
				ret = true;
		}
	}
	return (ret);
}

/*
 * Processes a directory when a recursive search is performed with
 * the -R option.  Each appropriate file is passed to procfile().
 */
int
grep_tree(char **argv)
{
	FTS *fts;
	FTSENT *p;
	int c, fts_flags;
	bool ok;

	c = fts_flags = 0;

	switch(linkbehave) {
	case LINK_EXPLICIT:
		fts_flags = FTS_COMFOLLOW;
		break;
	case LINK_SKIP:
		fts_flags = FTS_PHYSICAL;
		break;
	default:
		fts_flags = FTS_LOGICAL;
			
	}

	fts_flags |= FTS_NOSTAT | FTS_NOCHDIR;

    if (!(fts = fts_open(argv, fts_flags, NULL))) {
		err(2, "fts_open");
    }
	while ((p = fts_read(fts)) != NULL) {
		switch (p->fts_info) {
		case FTS_DNR:
			/* FALLTHROUGH */
		case FTS_ERR:
			file_err = true;
			if(!sflag)
                warnx("%s: %s", p->fts_path, strerror(p->fts_errno));
			break;
		case FTS_D:
			/* FALLTHROUGH */
		case FTS_DP:
			if (dexclude || dinclude)
				if (!dir_matching(p->fts_name) ||
				    !dir_matching(p->fts_path))
					fts_set(fts, p, FTS_SKIP);
			break;
		case FTS_DC:
			/* Print a warning for recursive directory loop */
			warnx("warning: %s: recursive directory loop",
				p->fts_path);
			break;
		default:
			/* Check for file exclusion/inclusion */
			ok = true;
			if (fexclude || finclude)
				ok &= file_matching(p->fts_path);

			if (ok)
				c += procfile(p->fts_path);
			break;
		}
	}

	fts_close(fts);
	return (c);
}

/*
 * Opens a file and processes it.  Each file is processed line-by-line
 * passing the lines to procline().
 */
int
procfile(const char *fn)
{
	struct file *f;
	struct stat sb;
	struct str ln;
	mode_t s;
	int c, t;

	if (mflag && (mcount <= 0))
		return (0);

	if (strcmp(fn, "-") == 0) {
#ifdef __APPLE__
		/* 4053512, 10290183 */
		if (dirbehave == DIR_RECURSE && ios_isatty(STDIN_FILENO)) {
            warnx("warning: recursive search of stdin");
		}
#endif
		fn = label != NULL ? label : getstr(1);
		f = grep_open(NULL);
	} else {
		if (!stat(fn, &sb)) {
			/* Check if we need to process the file */
			s = sb.st_mode & S_IFMT;
			if (s == S_IFDIR && dirbehave == DIR_SKIP)
				return (0);
			if ((s == S_IFIFO || s == S_IFCHR || s == S_IFBLK
				|| s == S_IFSOCK) && devbehave == DEV_SKIP)
					return (0);
		}
		f = grep_open(fn);
	}
	if (f == NULL) {
		file_err = true;
		if (!sflag)
            warn("%s", fn);
		return (0);
	}

	ln.file = grep_malloc(strlen(fn) + 1);
	strcpy(ln.file, fn);
	ln.line_no = 0;
	ln.len = 0;
	linesqueued = 0;
	tail = 0;
	ln.off = -1;

	for (c = 0;  c == 0 || !(lflag || qflag); ) {
		ln.off += ln.len + 1;
		if ((ln.dat = grep_fgetln(f, &ln.len)) == NULL || ln.len == 0) {
            if (ln.line_no == 0 && matchall)
				exit(0);
            else
				break;
		}
		if (ln.len > 0 && ln.dat[ln.len - 1] == '\n')
			--ln.len;
		ln.line_no++;

		/* Return if we need to skip a binary file */
		if (f->binary && binbehave == BINFILE_SKIP) {
			grep_close(f);
			free(ln.file);
			free(f);
			return (0);
		}
		/* Process the file line-by-line */
		if ((t = procline(&ln, f->binary)) == 0 && Bflag > 0) {
			enqueue(&ln);
			linesqueued++;
		}
		c += t;
		if (mflag && mcount <= 0)
			break;
	}
	if (Bflag > 0)
		clearqueue();
	grep_close(f);

#ifdef __APPLE__
	/* 10680370 */
	if (cflag && !qflag) {
#else
	if (cflag) {
#endif
		if (!hflag)
			fprintf(thread_stdout, "%s:", ln.file);
		fprintf(thread_stdout, "%u\n", c);
	}
	if (lflag && !qflag && c != 0)
		fprintf(thread_stdout, "%s%c", fn, nullflag ? 0 : '\n');
	if (Lflag && !qflag && c == 0)
		fprintf(thread_stdout, "%s%c", fn, nullflag ? 0 : '\n');
	if (c && !cflag && !lflag && !Lflag &&
	    binbehave == BINFILE_BIN && f->binary && !qflag)
		fprintf(thread_stdout, getstr(8), fn);

	free(ln.file);
	free(f);
	return (c);
}

#ifdef __APPLE__
static int
mbtowc_reverse(wchar_t *pwc, const char *s, size_t n)
{
	int result;
	size_t i;

	result = -1;
	for (i = 1; i <= n; i++) {
		result = mbtowc(pwc, s - i, i);
		if (result != -1) {
			break;
		}
	}

	return result;
}
#endif

#define iswword(x)	(iswalnum((x)) || (x) == L'_')

/*
 * Processes a line comparing it with the specified patterns.  Each pattern
 * is looped to be compared along with the full string, saving each and every
 * match, which is necessary to colorize the output and to count the
 * matches.  The matching lines are passed to printline() to display the
 * appropriate output.
 */
static int
procline(struct str *l, int nottext)
{
#ifdef __APPLE__
	int nmatches;
	regmatch_t *matches;
#else
	regmatch_t matches[MAX_LINE_MATCHES];
#endif
	regmatch_t pmatch;
	size_t st = 0;
	unsigned int i;
	int c = 0, m = 0, r = 0;

#ifdef __APPLE__
	nmatches = MIN_LINE_MATCHES;
	matches = grep_malloc(nmatches * sizeof(regmatch_t));
#endif

	/* Loop to process the whole line */
	do {
#ifdef WITHOUT_FASTMATCH
		if (matchall) {
			c = !vflag;
			break;
		}
#endif

		pmatch.rm_so = st;
		pmatch.rm_eo = l->len;

		/* Loop to compare with all the patterns */
		for (i = 0; i < patterns; i++) {
#ifdef __APPLE__
			/* 10462853: Treat binary files as binary. */
			if (nottext) {
				setlocale(LC_ALL, "C");
			}
#endif /* __APPLE__ */
#ifndef WITHOUT_FASTMATCH
			if (fg_pattern[i].pattern)
				r = fastexec(&fg_pattern[i],
				    l->dat, 1, &pmatch, eflags);
			else
#endif
				r = regexec(&r_pattern[i], l->dat, 1,
				    &pmatch, eflags);
#ifdef __APPLE__
			if (nottext) {
				setlocale(LC_ALL, "");
			}
#endif /* __APPLE__ */
			r = (r == 0) ? 0 : REG_NOMATCH;
			st = (cflags & REG_NOSUB)
				? (size_t)l->len
				: (size_t)pmatch.rm_eo;
			if (r == REG_NOMATCH)
				continue;
			/* Check for full match */
			if (r == 0 && xflag)
				if (pmatch.rm_so != 0 ||
				    (size_t)pmatch.rm_eo != l->len)
					r = REG_NOMATCH;
			/* Check for whole word match */
#ifndef WITHOUT_FASTMATCH
			if (r == 0 && (wflag || fg_pattern[i].word)) {
#else
			if (r == 0 && (wflag)) {
#endif
				wchar_t wbegin, wend;

				wbegin = wend = L' ';
				if (pmatch.rm_so != 0 &&
#ifdef __APPLE__
				    mbtowc_reverse(&wbegin, &l->dat[pmatch.rm_so], MAX(MB_CUR_MAX, pmatch.rm_so)) == -1)
#else
				    sscanf(&l->dat[pmatch.rm_so - 1],
				    "%lc", &wbegin) != 1)
#endif
					r = REG_NOMATCH;
				else if ((size_t)pmatch.rm_eo !=
				    l->len &&
#ifdef __APPLE__
				    mbtowc(&wend, &l->dat[pmatch.rm_eo], MAX(MB_CUR_MAX, l->len - (size_t)pmatch.rm_eo)) == -1)
#else
				    sscanf(&l->dat[pmatch.rm_eo],
				    "%lc", &wend) != 1)
#endif
					r = REG_NOMATCH;
				else if (iswword(wbegin) ||
				    iswword(wend))
					r = REG_NOMATCH;
			}
			if (r == 0) {
				if (m == 0)
					c++;
#ifdef __APPLE__
				if (m < MAX_LINE_MATCHES) {
					if (m >= nmatches) {
						nmatches += MIN_LINE_MATCHES;
						matches = grep_realloc(matches, nmatches * sizeof(regmatch_t));
					}
					matches[m++] = pmatch;
				}
#else
				if (m < MAX_LINE_MATCHES)
					matches[m++] = pmatch;
#endif
				/* matches - skip further patterns */
				if ((color == NULL && !oflag) ||
				    qflag || lflag)
					break;
			}
		}

		if (vflag) {
			c = !c;
			break;
		}

		/* One pass if we are not recording matches */
#ifdef __APPLE__
		/* 10593340: If -w didn't match, keep going. */
		if (!(wflag && r == REG_NOMATCH) && ((color == NULL && !oflag) || qflag || lflag))
#else
		if ((color == NULL && !oflag) || qflag || lflag)
#endif
			break;

		if (st == (size_t)pmatch.rm_so)
			break; 	/* No matches */
	} while (st < l->len);


	/* Count the matches if we have a match limit */
	if (mflag)
		mcount -= c;

	if (c && binbehave == BINFILE_BIN && nottext) {
#ifdef __APPLE__
		free(matches);
#endif
		return (c); /* Binary file */
	}

	/* Dealing with the context */
	if ((tail || c) && !cflag && !qflag && !lflag && !Lflag) {
		if (c) {
			if (!first && !prev && !tail && Aflag)
				fprintf(thread_stdout, "--\n");
			tail = Aflag;
			if (Bflag > 0) {
				if (!first && !prev)
					fprintf(thread_stdout, "--\n");
				printqueue();
			}
			linesqueued = 0;
			printline(l, ':', matches, m);
		} else {
			printline(l, '-', matches, m);
			tail--;
		}
	}

	if (c) {
		prev = true;
		first = false;
	} else
		prev = false;

#ifdef __APPLE__
	free(matches);
#endif
	return (c);
}

/*
 * Safe malloc() for internal use.
 */
void *
grep_malloc(size_t size)
{
	void *ptr;

    if ((ptr = malloc(size)) == NULL) {
		err(2, "malloc");
    }
	return (ptr);
}

/*
 * Safe calloc() for internal use.
 */
void *
grep_calloc(size_t nmemb, size_t size)
{
	void *ptr;

    if ((ptr = calloc(nmemb, size)) == NULL) {
        err(2, "calloc");
    }
	return (ptr);
}

/*
 * Safe realloc() for internal use.
 */
void *
grep_realloc(void *ptr, size_t size)
{

    if ((ptr = realloc(ptr, size)) == NULL) {
        err(2, "realloc");
    }
	return (ptr);
}

/*
 * Safe strdup() for internal use.
 */
char *
grep_strdup(const char *str)
{
	char *ret;

    if ((ret = strdup(str)) == NULL) {
        err(2, "strdup");
    }
	return (ret);
}

/*
 * Prints a matching line according to the command line options.
 */
void
printline(struct str *line, int sep, regmatch_t *matches, int m)
{
	size_t a = 0;
	int i, n = 0;

	if (!hflag) {
		if (!nullflag) {
			fputs(line->file, thread_stdout);
			++n;
		} else {
			fprintf(thread_stdout, "%s", line->file);
			putchar(0);
		}
	}
	if (nflag) {
		if (n > 0)
			putchar(sep);
		fprintf(thread_stdout, "%d", line->line_no);
		++n;
	}
	if (bflag) {
		if (n > 0)
			putchar(sep);
		fprintf(thread_stdout, "%lld", (long long)line->off);
		++n;
	}
	if (n)
		putchar(sep);
	/* --color and -o */
	if ((oflag || color) && m > 0) {
		for (i = 0; i < m; i++) {
			if (!oflag)
				fwrite(line->dat + a, matches[i].rm_so - a, 1,
				    thread_stdout);
			if (color) 
				fprintf(thread_stdout, "\33[%sm\33[K", color);

				fwrite(line->dat + matches[i].rm_so, 
				    matches[i].rm_eo - matches[i].rm_so, 1,
				    thread_stdout);
			if (color) 
				fprintf(thread_stdout, "\33[m\33[K");
			a = matches[i].rm_eo;
			if (oflag)
				putchar('\n');
		}
		if (!oflag) {
			if (line->len - a > 0)
				fwrite(line->dat + a, line->len - a, 1, thread_stdout);
			putchar('\n');
		}
	} else {
		fwrite(line->dat, line->len, 1, thread_stdout);
		putchar('\n');
	}
}
