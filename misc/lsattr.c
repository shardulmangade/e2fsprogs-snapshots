/*
 * lsattr.c		- List file attributes on an ext2 file system
 *
 * Copyright (C) 1993, 1994  Remy Card <card@masi.ibp.fr>
 *                           Laboratoire MASI, Institut Blaise Pascal
 *                           Universite Pierre et Marie Curie (Paris VI)
 *
 * This file can be redistributed under the terms of the GNU General
 * Public License
 */

/*
 * History:
 * 93/10/30	- Creation
 * 93/11/13	- Replace stat() calls by lstat() to avoid loops
 * 94/02/27	- Integrated in Ted's distribution
 * 98/12/29	- Display version info only when -V specified (G M Sipe)
 */

#define _LARGEFILE64_SOURCE

#include <sys/types.h>
#include <dirent.h>
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <fcntl.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern int optind;
extern char *optarg;
#endif
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "ext2fs/ext2_fs.h"
#include "et/com_err.h"
#include "e2p/e2p.h"

#include "../version.h"
#include "nls-enable.h"

#ifdef __GNUC__
#define EXT2FS_ATTR(x) __attribute__(x)
#else
#define EXT2FS_ATTR(x)
#endif

static const char * program_name = "lsattr";

static int all;
static int dirs_opt;
static unsigned pf_options;
static int recursive;
static int verbose;
static int generation_opt;

#ifdef _LFS64_LARGEFILE
#define LSTAT		lstat64
#define STRUCT_STAT	struct stat64
#else
#define LSTAT		lstat
#define STRUCT_STAT	struct stat
#endif

static void usage(void)
{
#ifdef EXT2FS_SNAPSHOT_ON_DISK_MIGRATE
	fprintf(stderr, _("Usage: %s [-XRVadlv] [files...]\n"), program_name);
#else
	fprintf(stderr, _("Usage: %s [-RVadlv] [files...]\n"), program_name);
#endif
	exit(1);
}

static int list_attributes (const char * name)
{
	unsigned long flags;
	unsigned long generation;

#ifdef EXT2FS_SNAPSHOT_CTL
	if (fgetpflags (name, &flags, pf_options) == -1) {
#else
	if (fgetflags (name, &flags) == -1) {
#endif
		com_err (program_name, errno, _("While reading flags on %s"),
			 name);
		return -1;
	}
	if (generation_opt) {
		if (fgetversion (name, &generation) == -1) {
			com_err (program_name, errno,
				 _("While reading version on %s"),
				 name);
			return -1;
		}
		printf ("%5lu ", generation);
	}
	if (pf_options & PFOPT_LONG) {
		printf("%-28s ", name);
		print_flags(stdout, flags, pf_options);
		fputc('\n', stdout);
	} else {
		print_flags(stdout, flags, pf_options);
		printf(" %s\n", name);
	}
	return 0;
}

static int lsattr_dir_proc (const char *, struct dirent *, void *);

static int lsattr_args (const char * name)
{
	STRUCT_STAT	st;
	int retval = 0;

	if (LSTAT (name, &st) == -1) {
		com_err (program_name, errno, _("while trying to stat %s"),
			 name);
		retval = -1;
	} else {
		if (S_ISDIR(st.st_mode) && !dirs_opt)
			retval = iterate_on_dir (name, lsattr_dir_proc, NULL);
		else
			retval = list_attributes (name);
	}
	return retval;
}

static int lsattr_dir_proc (const char * dir_name, struct dirent * de,
			    void * private EXT2FS_ATTR((unused)))
{
	STRUCT_STAT	st;
	char *path;
	int dir_len = strlen(dir_name);

	path = malloc(dir_len + strlen (de->d_name) + 2);

	if (dir_len && dir_name[dir_len-1] == '/')
		sprintf (path, "%s%s", dir_name, de->d_name);
	else
		sprintf (path, "%s/%s", dir_name, de->d_name);
	if (LSTAT (path, &st) == -1)
		perror (path);
	else {
		if (de->d_name[0] != '.' || all) {
			list_attributes (path);
			if (S_ISDIR(st.st_mode) && recursive &&
			    strcmp(de->d_name, ".") &&
			    strcmp(de->d_name, "..")) {
				printf ("\n%s:\n", path);
				iterate_on_dir (path, lsattr_dir_proc, NULL);
				printf ("\n");
			}
		}
	}
	free(path);
	return 0;
}

int main (int argc, char ** argv)
{
	int c;
	int i;
	int err, retval = 0;

#ifdef ENABLE_NLS
	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");
	bindtextdomain(NLS_CAT_NAME, LOCALEDIR);
	textdomain(NLS_CAT_NAME);
#endif
	if (argc && *argv)
		program_name = *argv;
#ifdef EXT2FS_SNAPSHOT_CTL
	i = strlen(program_name);
	if (i >= 6 && !strcmp(program_name + i - 6, "lssnap"))
		pf_options |= PFOPT_SNAPSHOT;

#endif
#ifdef EXT2FS_SNAPSHOT_ON_DISK_MIGRATE
	while ((c = getopt (argc, argv, "XRVadlv")) != EOF)
#else
	while ((c = getopt (argc, argv, "RVadlv")) != EOF)
#endif
		switch (c)
		{
#ifdef EXT2FS_SNAPSHOT_ON_DISK_MIGRATE
			case 'X':
				/* for backward compatibility with next3 */
				pf_options &= ~PFOPT_SNAPSHOT;
				pf_options |= PFOPT_SNAPSHOT_X;
				break;
#endif
			case 'R':
				recursive = 1;
				break;
			case 'V':
				verbose = 1;
				break;
			case 'a':
				all = 1;
				break;
			case 'd':
				dirs_opt = 1;
				break;
			case 'l':
#ifdef EXT2FS_SNAPSHOT_CTL
				pf_options |= PFOPT_LONG;
#else
				pf_options = PFOPT_LONG;
#endif
				break;
			case 'v':
				generation_opt = 1;
				break;
			default:
				usage();
		}

	if (verbose)
		fprintf (stderr, "lsattr %s (%s)\n",
			 E2FSPROGS_VERSION, E2FSPROGS_DATE);
	if (optind > argc - 1) {
		if (lsattr_args (".") == -1)
			retval = 1;
	} else {
		for (i = optind; i < argc; i++) {
			err = lsattr_args (argv[i]);
			if (err)
				retval = 1;
		}
	}
	exit(retval);
}
