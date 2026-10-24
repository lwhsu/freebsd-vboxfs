/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * mount_vboxvfs -- mount a VirtualBox shared folder in a FreeBSD guest.
 *
 * Everything is handed to the kernel through nmount(2) as name/value pairs;
 * this program only resolves names to numbers and validates the file system
 * specific options.  The iovec it builds, which the kernel side
 * (vboxvfs_mount.c) mirrors, is:
 *
 *	fstype		"vboxvfs"
 *	fspath		mount point, resolved with realpath(3), a directory
 *	from		shared folder name, exactly as given
 *	errmsg		buffer the kernel fills with an error description
 *	uid		owner shown for every file, decimal
 *			(from uid=<name|number>; the kernel defaults to 0)
 *	gid		group shown for every file, decimal
 *			(from gid=<name|number>; the kernel defaults to 0)
 *	file_mode	octal with a leading 0, 0..07777; 0 means "use the
 *			mode reported by the host" (the kernel default)
 *	dir_mode	likewise for directories
 *	fmask		octal, bits cleared from the host's mode of files
 *	dmask		likewise for directories
 *	ttl		attribute cache lifetime in milliseconds, decimal;
 *			0 means no caching (the kernel defaults to 200)
 *
 * Every other option (ro, rw, noatime, nosuid, noexec, update, force, ...)
 * is passed through unchanged for the VFS layer to interpret; the kernel
 * rejects names it does not know.  The fstab-only words auto, noauto, late
 * and failok are dropped here because mount(8) consumes them and the kernel
 * has never heard of them.  -r is the same as -o ro and -w the same as
 * -o rw; nothing else is implied (in particular no async).
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/uio.h>

#include <err.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <mntopts.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

static struct iovec	*iov;
static int		 iovlen;
static bool		 rdonly;

static void	add_id(const char *, const char *, bool);
static void	add_octal(const char *, const char *);
static void	add_ttl(const char *, const char *);
static void	add_option(const char *, const char *);
static char	*catopt(char *, const char *);
static void	dump_iovec(int);
static bool	is_fstab_only(const char *);
static void	parse_options(char *);
static void	usage(void) __dead2;

int
main(int argc, char *argv[])
{
	struct iovec *optiov;
	char mntpath[MAXPATHLEN];
	char errmsg[255];
	char *options;
	int ch, i, mntflags, optiovlen;
	bool dryrun;

	options = NULL;
	dryrun = false;
	while ((ch = getopt(argc, argv, "do:rw")) != -1) {
		switch (ch) {
		case 'd':
			dryrun = true;
			break;
		case 'o':
			options = catopt(options, optarg);
			break;
		case 'r':
			options = catopt(options, "ro");
			break;
		case 'w':
			options = catopt(options, "rw");
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 2)
		usage();
	if (argv[0][0] == '\0')
		errx(EX_USAGE, "the shared folder name must not be empty");

	/*
	 * Resolve the mount point with realpath(3) and make sure it is a
	 * directory; checkpath(3) sets errno for err(3).
	 */
	if (checkpath(argv[1], mntpath) != 0)
		err(EX_USAGE, "%s", argv[1]);

	/*
	 * Collect the options first so that the fixed entries can be put at
	 * the front of the vector, which keeps -d output readable.
	 */
	if (options != NULL)
		parse_options(options);
	optiov = iov;
	optiovlen = iovlen;
	iov = NULL;
	iovlen = 0;

	errmsg[0] = '\0';
	build_iovec(&iov, &iovlen, "fstype", __DECONST(void *, "vboxvfs"),
	    (size_t)-1);
	build_iovec(&iov, &iovlen, "fspath", mntpath, (size_t)-1);
	build_iovec(&iov, &iovlen, "from", argv[0], (size_t)-1);
	build_iovec(&iov, &iovlen, "errmsg", errmsg, sizeof(errmsg));
	for (i = 0; i < optiovlen; i += 2)
		build_iovec(&iov, &iovlen, optiov[i].iov_base,
		    optiov[i + 1].iov_base, optiov[i + 1].iov_len);
	free(optiov);

	mntflags = rdonly ? MNT_RDONLY : 0;
	if (dryrun) {
		dump_iovec(mntflags);
		exit(0);
	}

	if (nmount(iov, iovlen, mntflags) < 0) {
		if (errmsg[0] != '\0')
			err(1, "%s: %s", mntpath, errmsg);
		else
			err(1, "%s", mntpath);
	}
	exit(0);
}

/*
 * Append a comma separated option string to another one.
 */
static char *
catopt(char *s0, const char *s1)
{
	char *cp;

	if (s1 == NULL || *s1 == '\0')
		return (s0);
	if (s0 == NULL || *s0 == '\0') {
		if ((cp = strdup(s1)) == NULL)
			err(EX_OSERR, NULL);
	} else {
		if (asprintf(&cp, "%s,%s", s0, s1) == -1)
			err(EX_OSERR, NULL);
	}
	free(s0);
	return (cp);
}

/*
 * Split a comma separated option string into name/value pairs and add
 * each of them to the iovec, canonicalising the ones we know about.
 */
static void
parse_options(char *options)
{
	char *name, *value;

	while ((name = strsep(&options, ",")) != NULL) {
		if (*name == '\0')
			continue;
		value = strchr(name, '=');
		if (value != NULL)
			*value++ = '\0';

		if (is_fstab_only(name))
			continue;
		if (strcmp(name, "uid") == 0)
			add_id(name, value, false);
		else if (strcmp(name, "gid") == 0)
			add_id(name, value, true);
		else if (strcmp(name, "file_mode") == 0 ||
		    strcmp(name, "dir_mode") == 0 ||
		    strcmp(name, "fmask") == 0 ||
		    strcmp(name, "dmask") == 0)
			add_octal(name, value);
		else if (strcmp(name, "ttl") == 0)
			add_ttl(name, value);
		else
			add_option(name, value);
	}
}

/*
 * Options that only mount(8) and fstab(5) know; the kernel rejects them.
 */
static bool
is_fstab_only(const char *name)
{

	return (strcmp(name, "auto") == 0 || strcmp(name, "noauto") == 0 ||
	    strcmp(name, "late") == 0 || strcmp(name, "failok") == 0);
}

/*
 * uid=<name|number> and gid=<name|number>: always handed over as a number.
 */
static void
add_id(const char *name, const char *value, bool isgroup)
{
	struct passwd *pw;
	struct group *gr;
	char *ep;
	unsigned long n;

	if (value == NULL || *value == '\0')
		errx(EX_USAGE, "-o %s: a value is required", name);
	errno = 0;
	n = strtoul(value, &ep, 10);
	if (ep != value && *ep == '\0' && errno == 0) {
		if (n > UINT_MAX)
			errx(EX_USAGE, "-o %s=%s: out of range", name, value);
		build_iovec_argf(&iov, &iovlen, name, "%lu", n);
		return;
	}
	if (isgroup) {
		if ((gr = getgrnam(value)) == NULL)
			errx(EX_USAGE, "-o %s=%s: unknown group", name, value);
		build_iovec_argf(&iov, &iovlen, name, "%u",
		    (unsigned int)gr->gr_gid);
	} else {
		if ((pw = getpwnam(value)) == NULL)
			errx(EX_USAGE, "-o %s=%s: unknown user", name, value);
		build_iovec_argf(&iov, &iovlen, name, "%u",
		    (unsigned int)pw->pw_uid);
	}
}

/*
 * file_mode=, dir_mode=, fmask= and dmask=: an octal mode, 0 to 07777,
 * handed over with a leading 0 so that it cannot be mistaken for decimal.
 */
static void
add_octal(const char *name, const char *value)
{
	char *ep;
	unsigned long n;

	if (value == NULL || *value == '\0')
		errx(EX_USAGE, "-o %s: a value is required", name);
	errno = 0;
	n = strtoul(value, &ep, 8);
	if (ep == value || *ep != '\0' || errno != 0 || n > 07777)
		errx(EX_USAGE, "-o %s=%s: not an octal mode", name, value);
	build_iovec_argf(&iov, &iovlen, name, "%#lo", n);
}

/*
 * ttl=<milliseconds>.
 */
static void
add_ttl(const char *name, const char *value)
{
	char *ep;
	unsigned long n;

	if (value == NULL || *value == '\0')
		errx(EX_USAGE, "-o %s: a value is required", name);
	errno = 0;
	n = strtoul(value, &ep, 10);
	if (ep == value || *ep != '\0' || errno != 0 || n > INT_MAX)
		errx(EX_USAGE, "-o %s=%s: not a number of milliseconds", name,
		    value);
	build_iovec_argf(&iov, &iovlen, name, "%lu", n);
}

/*
 * Anything else goes through unchanged.  Keep track of the read-only
 * state so that the flags argument of nmount(2) agrees with the options.
 */
static void
add_option(const char *name, const char *value)
{

	if (strcmp(name, "ro") == 0 || strcmp(name, "rdonly") == 0 ||
	    strcmp(name, "norw") == 0)
		rdonly = true;
	else if (strcmp(name, "rw") == 0 || strcmp(name, "noro") == 0)
		rdonly = false;
	build_iovec(&iov, &iovlen, name,
	    value == NULL ? NULL : __DECONST(void *, value), (size_t)-1);
}

/*
 * -d: show what would be passed to nmount(2) and stop.
 */
static void
dump_iovec(int mntflags)
{
	int i;

	printf("flags=%#x\n", (unsigned int)mntflags);
	for (i = 0; i < iovlen; i += 2) {
		if (strcmp(iov[i].iov_base, "errmsg") == 0)
			printf("errmsg=<%zu byte buffer>\n", iov[i + 1].iov_len);
		else if (iov[i + 1].iov_len == 0)
			printf("%s\n", (const char *)iov[i].iov_base);
		else
			printf("%s=%s\n", (const char *)iov[i].iov_base,
			    (const char *)iov[i + 1].iov_base);
	}
}

static void
usage(void)
{

	fprintf(stderr,
	    "usage: mount_vboxvfs [-d] [-r | -w] [-o options] share node\n");
	exit(EX_USAGE);
}
