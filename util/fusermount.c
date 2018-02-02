/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/
/* This program does the mounting and unmounting of FUSE filesystems */
#if 0
#define _GNU_SOURCE /* for clone */
#include <config.h>

#include "mount_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <mntent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/fsuid.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sched.h>
#include <paths.h>
#include <mntent.h>

#define FUSE_COMMFD_ENV		"_FUSE_COMMFD"

#define FUSE_DEV_OLD "/proc/fs/fuse/dev"
#define FUSE_DEV_NEW "/dev/fuse"
#define FUSE_VERSION_FILE_OLD "/proc/fs/fuse/version"
#define FUSE_CONF "/etc/fuse.conf"

#ifndef MS_DIRSYNC
#define MS_DIRSYNC 128
#endif
#ifndef MS_REC
#define MS_REC 16384
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1<<18)
#endif

static const char *progname;

static int user_allow_other = 0;
static int mount_max = 1000;

static const char *get_user_name(void)
{
	struct passwd *pw = getpwuid(getuid());
	if (pw != NULL && pw->pw_name != NULL)
		return pw->pw_name;
	else {
		fprintf(stderr, "%s: could not determine username\n", progname);
		return NULL;
	}
}

static uid_t oldfsuid;
static gid_t oldfsgid;

static void drop_privs(void)
{
#if !defined(__ANDROID__)
	if (getuid() != 0) {
		oldfsuid = setfsuid(getuid());
		oldfsgid = setfsgid(getgid());
	}
#endif
}

static void restore_privs(void)
{
#if !defined(__ANDROID__)
	if (getuid() != 0) {
		setfsuid(oldfsuid);
		setfsgid(oldfsgid);
	}
#endif
}

#ifndef IGNORE_MTAB
/*
 * Make sure that /etc/mtab is checked and updated atomically
 */
static int lock_umount(void)
{
	const char *mtab_lock = _PATH_MOUNTED ".fuselock";
	int mtablock;
	int res;
	struct stat mtab_stat;

	/* /etc/mtab could be a symlink to /proc/mounts */
	if (lstat(_PATH_MOUNTED, &mtab_stat) == 0 && S_ISLNK(mtab_stat.st_mode))
		return -1;

	mtablock = open(mtab_lock, O_RDWR | O_CREAT, 0600);
	if (mtablock == -1) {
		fprintf(stderr, "%s: unable to open fuse lock file: %s\n",
			progname, strerror(errno));
		return -1;
	}
	res = lockf(mtablock, F_LOCK, 0);
	if (res < 0) {
		fprintf(stderr, "%s: error getting lock: %s\n", progname,
			strerror(errno));
		close(mtablock);
		return -1;
	}

	return mtablock;
}

static void unlock_umount(int mtablock)
{
	lockf(mtablock, F_ULOCK, 0);
	close(mtablock);
}

static int add_mount(const char *source, const char *mnt, const char *type,
		     const char *opts)
{
	return fuse_mnt_add_mount(progname, source, mnt, type, opts);
}

static int may_unmount(const char *mnt, int quiet)
{
	struct mntent *entp;
	FILE *fp;
	const char *user = NULL;
	char uidstr[32];
	unsigned uidlen = 0;
	int found;
	const char *mtab = _PATH_MOUNTED;

	user = get_user_name();
	if (user == NULL)
		return -1;

	fp = setmntent(mtab, "r");
	if (fp == NULL) {
		fprintf(stderr, "%s: failed to open %s: %s\n", progname, mtab,
			strerror(errno));
		return -1;
	}

	uidlen = sprintf(uidstr, "%u", getuid());

	found = 0;
	while ((entp = getmntent(fp)) != NULL) {
		if (!found && strcmp(entp->mnt_dir, mnt) == 0 &&
		    (strcmp(entp->mnt_type, "fuse") == 0 ||
		     strcmp(entp->mnt_type, "fuseblk") == 0 ||
		     strncmp(entp->mnt_type, "fuse.", 5) == 0 ||
		     strncmp(entp->mnt_type, "fuseblk.", 8) == 0)) {
			char *p = strstr(entp->mnt_opts, "user=");
			if (p &&
			    (p == entp->mnt_opts || *(p-1) == ',') &&
			    strcmp(p + 5, user) == 0) {
				found = 1;
				break;
			}
			/* /etc/mtab is a link pointing to
			   /proc/mounts: */
			else if ((p =
				  strstr(entp->mnt_opts, "user_id=")) &&
				 (p == entp->mnt_opts ||
				  *(p-1) == ',') &&
				 strncmp(p + 8, uidstr, uidlen) == 0 &&
				 (*(p+8+uidlen) == ',' ||
				  *(p+8+uidlen) == '\0')) {
				found = 1;
				break;
			}
		}
	}
	endmntent(fp);

	if (!found) {
		if (!quiet)
			fprintf(stderr,
				"%s: entry for %s not found in %s\n",
				progname, mnt, mtab);
		return -1;
	}

	return 0;
}

/*
 * Check whether the file specified in "fusermount -u" is really a
 * mountpoint and not a symlink.  This is necessary otherwise the user
 * could move the mountpoint away and replace it with a symlink
 * pointing to an arbitrary mount, thereby tricking fusermount into
 * unmounting that (umount(2) will follow symlinks).
 *
 * This is the child process running in a separate mount namespace, so
 * we don't mess with the global namespace and if the process is
 * killed for any reason, mounts are automatically cleaned up.
 *
 * First make sure nothing is propagated back into the parent
 * namespace by marking all mounts "private".
 *
 * Then bind mount parent onto a stable base where the user can't move
 * it around.
 *
 * Finally check /proc/mounts for an entry matching the requested
 * mountpoint.  If it's found then we are OK, and the user can't move
 * it around within the parent directory as rename() will return
 * EBUSY.  Be careful to ignore any mounts that existed before the
 * bind.
 */
static int check_is_mount_child(void *p)
{
	const char **a = p;
	const char *last = a[0];
	const char *mnt = a[1];
	int res;
	const char *procmounts = "/proc/mounts";
	int found;
	FILE *fp;
	struct mntent *entp;
	int count;

	res = mount("", "/", "", MS_PRIVATE | MS_REC, NULL);
	if (res == -1) {
		fprintf(stderr, "%s: failed to mark mounts private: %s\n",
			progname, strerror(errno));
		return 1;
	}

	fp = setmntent(procmounts, "r");
	if (fp == NULL) {
		fprintf(stderr, "%s: failed to open %s: %s\n", progname,
			procmounts, strerror(errno));
		return 1;
	}

	count = 0;
	while ((entp = getmntent(fp)) != NULL)
		count++;
	endmntent(fp);

	fp = setmntent(procmounts, "r");
	if (fp == NULL) {
		fprintf(stderr, "%s: failed to open %s: %s\n", progname,
			procmounts, strerror(errno));
		return 1;
	}

	res = mount(".", "/", "", MS_BIND | MS_REC, NULL);
	if (res == -1) {
		fprintf(stderr, "%s: failed to bind parent to /: %s\n",
			progname, strerror(errno));
		return 1;
	}

	found = 0;
	while ((entp = getmntent(fp)) != NULL) {
		if (count > 0) {
			count--;
			continue;
		}
		if (entp->mnt_dir[0] == '/' &&
		    strcmp(entp->mnt_dir + 1, last) == 0) {
			found = 1;
			break;
		}
	}
	endmntent(fp);

	if (!found) {
		fprintf(stderr, "%s: %s not mounted\n", progname, mnt);
		return 1;
	}

	return 0;
}

static pid_t clone_newns(void *a)
{
	char buf[131072];
	char *stack = buf + (sizeof(buf) / 2 - ((size_t) buf & 15));

#ifdef __ia64__
	extern int __clone2(int (*fn)(void *),
			    void *child_stack_base, size_t stack_size,
			    int flags, void *arg, pid_t *ptid,
			    void *tls, pid_t *ctid);

	return __clone2(check_is_mount_child, stack, sizeof(buf) / 2,
			CLONE_NEWNS, a, NULL, NULL, NULL);
#else
	return clone(check_is_mount_child, stack, CLONE_NEWNS, a);
#endif
}

static int check_is_mount(const char *last, const char *mnt)
{
	pid_t pid, p;
	int status;
	const char *a[2] = { last, mnt };

	pid = clone_newns((void *) a);
	if (pid == (pid_t) -1) {
		fprintf(stderr, "%s: failed to clone namespace: %s\n",
			progname, strerror(errno));
		return -1;
	}
	p = waitpid(pid, &status, __WCLONE);
	if (p == (pid_t) -1) {
		fprintf(stderr, "%s: waitpid failed: %s\n",
			progname, strerror(errno));
		return -1;
	}
	if (!WIFEXITED(status)) {
		fprintf(stderr, "%s: child terminated abnormally (status %i)\n",
			progname, status);
		return -1;
	}
	if (WEXITSTATUS(status) != 0)
		return -1;

	return 0;
}

static int chdir_to_parent(char *copy, const char **lastp, int *currdir_fd)
{
	char *tmp;
	const char *parent;
	char buf[65536];
	int res;

	tmp = strrchr(copy, '/');
	if (tmp == NULL || tmp[1] == '\0') {
		fprintf(stderr, "%s: internal error: invalid abs path: <%s>\n",
			progname, copy);
		return -1;
	}
	if (tmp != copy) {
		*tmp = '\0';
		parent = copy;
		*lastp = tmp + 1;
	} else if (tmp[1] != '\0') {
		*lastp = tmp + 1;
		parent = "/";
	} else {
		*lastp = ".";
		parent = "/";
	}

	*currdir_fd = open(".", O_RDONLY);
	if (*currdir_fd == -1) {
		fprintf(stderr,
			"%s: failed to open current directory: %s\n",
			progname, strerror(errno));
		return -1;
	}

	res = chdir(parent);
	if (res == -1) {
		fprintf(stderr, "%s: failed to chdir to %s: %s\n",
			progname, parent, strerror(errno));
		return -1;
	}

	if (getcwd(buf, sizeof(buf)) == NULL) {
		fprintf(stderr, "%s: failed to obtain current directory: %s\n",
			progname, strerror(errno));
		return -1;
	}
	if (strcmp(buf, parent) != 0) {
		fprintf(stderr, "%s: mountpoint moved (%s -> %s)\n", progname,
			parent, buf);
		return -1;

	}

	return 0;
}

static int unmount_fuse_locked(const char *mnt, int quiet, int lazy)
{
	int currdir_fd = -1;
	char *copy;
	const char *last;
	int res;

	if (getuid() != 0) {
		res = may_unmount(mnt, quiet);
		if (res == -1)
			return -1;
	}

	copy = strdup(mnt);
	if (copy == NULL) {
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
		return -1;
	}

	res = chdir_to_parent(copy, &last, &currdir_fd);
	if (res == -1)
		goto out;

	res = check_is_mount(last, mnt);
	if (res == -1)
		goto out;

	res = umount2(last, lazy ? 2 : 0);
	if (res == -1 && !quiet) {
		fprintf(stderr,
			"%s: failed to unmount %s: %s\n",
			progname, mnt, strerror(errno));
	}

	if (res == 0)
		res = fuse_mnt_remove_mount(progname, mnt);
out:
	free(copy);
	if (currdir_fd != -1) {
		fchdir(currdir_fd);
		close(currdir_fd);
	}

	return res;
}

static int unmount_fuse(const char *mnt, int quiet, int lazy)
{
	int res;
	int mtablock = lock_umount();

	res = unmount_fuse_locked(mnt, quiet, lazy);
	unlock_umount(mtablock);

	return res;
}

static int count_fuse_fs(void)
{
	struct mntent *entp;
	int count = 0;
	const char *mtab = _PATH_MOUNTED;
	FILE *fp = setmntent(mtab, "r");
	if (fp == NULL) {
		fprintf(stderr, "%s: failed to open %s: %s\n", progname, mtab,
			strerror(errno));
		return -1;
	}
	while ((entp = getmntent(fp)) != NULL) {
		if (strcmp(entp->mnt_type, "fuse") == 0 ||
		    strncmp(entp->mnt_type, "fuse.", 5) == 0)
			count ++;
	}
	endmntent(fp);
	return count;
}


#else /* IGNORE_MTAB */
static int count_fuse_fs()
{
	return 0;
}

static int add_mount(const char *source, const char *mnt, const char *type,
		     const char *opts)
{
	(void) source;
	(void) mnt;
	(void) type;
	(void) opts;
	return 0;
}

static int unmount_fuse(const char *mnt, int quiet, int lazy)
{
	return fuse_mnt_umount(progname, mnt, mnt, lazy);
}
#endif /* IGNORE_MTAB */

static void strip_line(char *line)
{
	char *s = strchr(line, '#');
	if (s != NULL)
		s[0] = '\0';
	for (s = line + strlen(line) - 1;
	     s >= line && isspace((unsigned char) *s); s--);
	s[1] = '\0';
	for (s = line; isspace((unsigned char) *s); s++);
	if (s != line)
		memmove(line, s, strlen(s)+1);
}

static void parse_line(char *line, int linenum)
{
	int tmp;
	if (strcmp(line, "user_allow_other") == 0)
		user_allow_other = 1;
	else if (sscanf(line, "mount_max = %i", &tmp) == 1)
		mount_max = tmp;
	else if(line[0])
		fprintf(stderr,
			"%s: unknown parameter in %s at line %i: '%s'\n",
			progname, FUSE_CONF, linenum, line);
}

static void read_conf(void)
{
	FILE *fp = fopen(FUSE_CONF, "r");
	if (fp != NULL) {
		int linenum = 1;
		char line[256];
		int isnewline = 1;
		while (fgets(line, sizeof(line), fp) != NULL) {
			if (isnewline) {
				if (line[strlen(line)-1] == '\n') {
					strip_line(line);
					parse_line(line, linenum);
				} else {
					isnewline = 0;
				}
			} else if(line[strlen(line)-1] == '\n') {
				fprintf(stderr, "%s: reading %s: line %i too long\n", progname, FUSE_CONF, linenum);

				isnewline = 1;
			}
			if (isnewline)
				linenum ++;
		}
		if (!isnewline) {
			fprintf(stderr, "%s: reading %s: missing newline at end of file\n", progname, FUSE_CONF);

		}
		fclose(fp);
	} else if (errno != ENOENT) {
		fprintf(stderr, "%s: failed to open %s: %s\n",
			progname, FUSE_CONF, strerror(errno));
	}
}

static int begins_with(const char *s, const char *beg)
{
	if (strncmp(s, beg, strlen(beg)) == 0)
		return 1;
	else
		return 0;
}

struct mount_flags {
	const char *opt;
	unsigned long flag;
	int on;
	int safe;
};

static struct mount_flags mount_flags[] = {
	{"rw",	    MS_RDONLY,	    0, 1},
	{"ro",	    MS_RDONLY,	    1, 1},
	{"suid",    MS_NOSUID,	    0, 0},
	{"nosuid",  MS_NOSUID,	    1, 1},
	{"dev",	    MS_NODEV,	    0, 0},
	{"nodev",   MS_NODEV,	    1, 1},
	{"exec",    MS_NOEXEC,	    0, 1},
	{"noexec",  MS_NOEXEC,	    1, 1},
	{"async",   MS_SYNCHRONOUS, 0, 1},
	{"sync",    MS_SYNCHRONOUS, 1, 1},
	{"atime",   MS_NOATIME,	    0, 1},
	{"noatime", MS_NOATIME,	    1, 1},
	{"dirsync", MS_DIRSYNC,	    1, 1},
	{NULL,	    0,		    0, 0}
};

static int find_mount_flag(const char *s, unsigned len, int *on, int *flag)
{
	int i;

	for (i = 0; mount_flags[i].opt != NULL; i++) {
		const char *opt = mount_flags[i].opt;
		if (strlen(opt) == len && strncmp(opt, s, len) == 0) {
			*on = mount_flags[i].on;
			*flag = mount_flags[i].flag;
			if (!mount_flags[i].safe && getuid() != 0) {
				*flag = 0;
				fprintf(stderr,
					"%s: unsafe option %s ignored\n",
					progname, opt);
			}
			return 1;
		}
	}
	return 0;
}

static int add_option(char **optsp, const char *opt, unsigned expand)
{
	char *newopts;
	if (*optsp == NULL)
		newopts = strdup(opt);
	else {
		unsigned oldsize = strlen(*optsp);
		unsigned newsize = oldsize + 1 + strlen(opt) + expand + 1;
		newopts = (char *) realloc(*optsp, newsize);
		if (newopts)
			sprintf(newopts + oldsize, ",%s", opt);
	}
	if (newopts == NULL) {
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
		return -1;
	}
	*optsp = newopts;
	return 0;
}

static int get_mnt_opts(int flags, char *opts, char **mnt_optsp)
{
	int i;
	int l;

	if (!(flags & MS_RDONLY) && add_option(mnt_optsp, "rw", 0) == -1)
		return -1;

	for (i = 0; mount_flags[i].opt != NULL; i++) {
		if (mount_flags[i].on && (flags & mount_flags[i].flag) &&
		    add_option(mnt_optsp, mount_flags[i].opt, 0) == -1)
			return -1;
	}

	if (add_option(mnt_optsp, opts, 0) == -1)
		return -1;
	/* remove comma from end of opts*/
	l = strlen(*mnt_optsp);
	if ((*mnt_optsp)[l-1] == ',')
		(*mnt_optsp)[l-1] = '\0';
	if (getuid() != 0) {
		const char *user = get_user_name();
		if (user == NULL)
			return -1;

		if (add_option(mnt_optsp, "user=", strlen(user)) == -1)
			return -1;
		strcat(*mnt_optsp, user);
	}
	return 0;
}

static int opt_eq(const char *s, unsigned len, const char *opt)
{
	if(strlen(opt) == len && strncmp(s, opt, len) == 0)
		return 1;
	else
		return 0;
}

static int get_string_opt(const char *s, unsigned len, const char *opt,
			  char **val)
{
	int i;
	unsigned opt_len = strlen(opt);
	char *d;

	if (*val)
		free(*val);
	*val = (char *) malloc(len - opt_len + 1);
	if (!*val) {
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
		return 0;
	}

	d = *val;
	s += opt_len;
	len -= opt_len;
	for (i = 0; i < len; i++) {
		if (s[i] == '\\' && i + 1 < len)
			i++;
		*d++ = s[i];
	}
	*d = '\0';
	return 1;
}

static int do_mount(const char *mnt, char **typep, mode_t rootmode,
		    int fd, const char *opts, const char *dev, char **sourcep,
		    char **mnt_optsp, int64_t rootsize)
{
	int res;
	int flags = MS_NOSUID | MS_NODEV;
	char *optbuf;
	char *mnt_opts = NULL;
	const char *s;
	char *d;
	char *fsname = NULL;
	char *subtype = NULL;
	char *source = NULL;
	char *type = NULL;
	int check_empty = 1;
	int blkdev = 0;

	optbuf = (char *) malloc(strlen(opts) + 128);
	if (!optbuf) {
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
		return -1;
	}

	for (s = opts, d = optbuf; *s;) {
		unsigned len;
		const char *fsname_str = "fsname=";
		const char *subtype_str = "subtype=";
		for (len = 0; s[len]; len++) {
			if (s[len] == '\\' && s[len + 1])
				len++;
			else if (s[len] == ',')
				break;
		}
		if (begins_with(s, fsname_str)) {
			if (!get_string_opt(s, len, fsname_str, &fsname))
				goto err;
		} else if (begins_with(s, subtype_str)) {
			if (!get_string_opt(s, len, subtype_str, &subtype))
				goto err;
		} else if (opt_eq(s, len, "blkdev")) {
			if (getuid() != 0) {
				fprintf(stderr,
					"%s: option blkdev is privileged\n",
					progname);
				goto err;
			}
			blkdev = 1;
		} else if (opt_eq(s, len, "nonempty")) {
			check_empty = 0;
		} else if (!begins_with(s, "fd=") &&
			   !begins_with(s, "rootmode=") &&
			   !begins_with(s, "user_id=") &&
			   !begins_with(s, "group_id=")) {
			int on;
			int flag;
			int skip_option = 0;
			if (opt_eq(s, len, "large_read")) {
				struct utsname utsname;
				unsigned kmaj, kmin;
				res = uname(&utsname);
				if (res == 0 &&
				    sscanf(utsname.release, "%u.%u",
					   &kmaj, &kmin) == 2 &&
				    (kmaj > 2 || (kmaj == 2 && kmin > 4))) {
					fprintf(stderr, "%s: note: 'large_read' mount option is deprecated for %i.%i kernels\n", progname, kmaj, kmin);
					skip_option = 1;
				}
			}
			if (getuid() != 0 && !user_allow_other &&
			    (opt_eq(s, len, "allow_other") ||
			     opt_eq(s, len, "allow_root"))) {
				fprintf(stderr, "%s: option %.*s only allowed if 'user_allow_other' is set in /etc/fuse.conf\n", progname, len, s);
				goto err;
			}
			if (!skip_option) {
				if (find_mount_flag(s, len, &on, &flag)) {
					if (on)
						flags |= flag;
					else
						flags  &= ~flag;
				} else {
					memcpy(d, s, len);
					d += len;
					*d++ = ',';
				}
			}
		}
		s += len;
		if (*s)
			s++;
	}
	*d = '\0';
	res = get_mnt_opts(flags, optbuf, &mnt_opts);
	if (res == -1)
		goto err;

	sprintf(d, "fd=%i,rootmode=%o,user_id=%i,group_id=%i",
		fd, rootmode, getuid(), getgid());

	if (check_empty &&
	    fuse_mnt_check_empty(progname, mnt, rootmode, rootsize) == -1)
		goto err;

	source = malloc((fsname ? strlen(fsname) : 0) +
			(subtype ? strlen(subtype) : 0) + strlen(dev) + 32);

	type = malloc((subtype ? strlen(subtype) : 0) + 32);
	if (!type || !source) {
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
		goto err;
	}

	if (subtype)
		sprintf(type, "%s.%s", blkdev ? "fuseblk" : "fuse", subtype);
	else
		strcpy(type, blkdev ? "fuseblk" : "fuse");

	if (fsname)
		strcpy(source, fsname);
	else
		strcpy(source, subtype ? subtype : dev);

	res = mount(source, mnt, type, flags, optbuf);
	if (res == -1 && errno == ENODEV && subtype) {
		/* Probably missing subtype support */
		strcpy(type, blkdev ? "fuseblk" : "fuse");
		if (fsname) {
			if (!blkdev)
				sprintf(source, "%s#%s", subtype, fsname);
		} else {
			strcpy(source, type);
		}

		res = mount(source, mnt, type, flags, optbuf);
	}
	if (res == -1 && errno == EINVAL) {
		/* It could be an old version not supporting group_id */
		sprintf(d, "fd=%i,rootmode=%o,user_id=%i",
			fd, rootmode, getuid());
		res = mount(source, mnt, type, flags, optbuf);
	}
	if (res == -1) {
		int errno_save = errno;
		if (blkdev && errno == ENODEV && !fuse_mnt_check_fuseblk())
			fprintf(stderr, "%s: 'fuseblk' support missing\n",
				progname);
		else
			fprintf(stderr, "%s: mount failed: %s\n", progname,
				strerror(errno_save));
		goto err;
	} else {
		*sourcep = source;
		*typep = type;
		*mnt_optsp = mnt_opts;
	}
	free(fsname);
	free(optbuf);

	return res;

err:
	free(fsname);
	free(subtype);
	free(source);
	free(type);
	free(mnt_opts);
	free(optbuf);
	return -1;
}

static int check_version(const char *dev)
{
	int res;
	int majorver;
	int minorver;
	const char *version_file;
	FILE *vf;

	if (strcmp(dev, FUSE_DEV_OLD) != 0)
		return 0;

	version_file = FUSE_VERSION_FILE_OLD;
	vf = fopen(version_file, "r");
	if (vf == NULL) {
		fprintf(stderr, "%s: kernel interface too old\n", progname);
		return -1;
	}
	res = fscanf(vf, "%i.%i", &majorver, &minorver);
	fclose(vf);
	if (res != 2) {
		fprintf(stderr, "%s: error reading %s\n", progname,
			version_file);
		return -1;
	}
	if (majorver < 3) {
		fprintf(stderr, "%s: kernel interface too old\n", progname);
		return -1;
	}
	return 0;
}

static int check_perm(const char **mntp, struct stat *stbuf, int *currdir_fd,
		      int *mountpoint_fd, int *isdir)
{
	int res;
	const char *mnt = *mntp;
	const char *origmnt = mnt;

	res = lstat(mnt, stbuf);
	if (res == -1) {
		fprintf(stderr, "%s: failed to access mountpoint %s: %s\n",
			progname, mnt, strerror(errno));
		return -1;
	}

	/* No permission checking is done for root */
	if (getuid() == 0)
		return 0;

	if (S_ISDIR(stbuf->st_mode)) {
		*isdir = 1;
		*currdir_fd = open(".", O_RDONLY);
		if (*currdir_fd == -1) {
			fprintf(stderr,
				"%s: failed to open current directory: %s\n",
				progname, strerror(errno));
			return -1;
		}
		*mountpoint_fd = open(mnt, O_RDONLY);
		if (*mountpoint_fd == -1) {
			fprintf(stderr, "%s: failed to open %s: %s\n",
				progname, mnt, strerror(errno));
			return -1;
		}
		res = fchdir(*mountpoint_fd);
		if (res == -1) {
			fprintf(stderr,
				"%s: failed to fchdir to mountpoint: %s\n",
				progname, strerror(errno));
			return -1;
		}
		mnt = *mntp = ".";
		res = lstat(mnt, stbuf);
		if (res == -1) {
			fprintf(stderr,
				"%s: failed to access mountpoint %s: %s\n",
				progname, origmnt, strerror(errno));
			return -1;
		}

		if ((stbuf->st_mode & S_ISVTX) && stbuf->st_uid != getuid()) {
			fprintf(stderr, "%s: mountpoint %s not owned by user\n",
				progname, origmnt);
			return -1;
		}

		res = access(mnt, W_OK);
		if (res == -1) {
			fprintf(stderr, "%s: user has no write access to mountpoint %s\n",
				progname, origmnt);
			return -1;
		}
	} else if (S_ISREG(stbuf->st_mode)) {
		static char procfile[256];
		*mountpoint_fd = open(mnt, O_WRONLY);
		if (*mountpoint_fd == -1) {
			fprintf(stderr, "%s: failed to open %s: %s\n",
				progname, mnt, strerror(errno));
			return -1;
		}
		res = fstat(*mountpoint_fd, stbuf);
		if (res == -1) {
			fprintf(stderr,
				"%s: failed to access mountpoint %s: %s\n",
				progname, mnt, strerror(errno));
			return -1;
		}
		if (!S_ISREG(stbuf->st_mode)) {
			fprintf(stderr,
				"%s: mountpoint %s is no longer a regular file\n",
				progname, mnt);
			return -1;
		}

		sprintf(procfile, "/proc/self/fd/%i", *mountpoint_fd);
		*mntp = procfile;
	} else {
		fprintf(stderr,
			"%s: mountpoint %s is not a directory or a regular file\n",
			progname, mnt);
		return -1;
	}


	return 0;
}

static int try_open(const char *dev, char **devp, int silent)
{
	int fd = open(dev, O_RDWR);
	if (fd != -1) {
		*devp = strdup(dev);
		if (*devp == NULL) {
			fprintf(stderr, "%s: failed to allocate memory\n",
				progname);
			close(fd);
			fd = -1;
		}
	} else if (errno == ENODEV ||
		   errno == ENOENT)/* check for ENOENT too, for the udev case */
		return -2;
	else if (!silent) {
		fprintf(stderr, "%s: failed to open %s: %s\n", progname, dev,
			strerror(errno));
	}
	return fd;
}

static int try_open_fuse_device(char **devp)
{
	int fd;
	int err;

	drop_privs();
	fd = try_open(FUSE_DEV_NEW, devp, 0);
	restore_privs();
	if (fd >= 0)
		return fd;

	err = fd;
	fd = try_open(FUSE_DEV_OLD, devp, 1);
	if (fd >= 0)
		return fd;

	return err;
}

static int open_fuse_device(char **devp)
{
	int fd = try_open_fuse_device(devp);
	if (fd >= -1)
		return fd;

	fprintf(stderr,
		"%s: fuse device not found, try 'modprobe fuse' first\n",
		progname);

	return -1;
}


static int mount_fuse(const char *mnt, const char *opts)
{
	int res;
	int fd;
	char *dev;
	struct stat stbuf;
	char *type = NULL;
	char *source = NULL;
	char *mnt_opts = NULL;
	const char *real_mnt = mnt;
	int currdir_fd = -1;
	int mountpoint_fd = -1;
	int isdir = 0;

	fd = open_fuse_device(&dev);
	if (fd == -1)
		return -1;

	drop_privs();
	read_conf();

	if (getuid() != 0 && mount_max != -1) {
		int mount_count = count_fuse_fs();
		if (mount_count >= mount_max) {
			fprintf(stderr, "%s: too many FUSE filesystems mounted; mount_max=N can be set in /etc/fuse.conf\n", progname);
			close(fd);
			return -1;
		}
	}

	res = check_version(dev);
	if (res != -1) {
		res = check_perm(&real_mnt, &stbuf, &currdir_fd,
				 &mountpoint_fd, &isdir);
		restore_privs();
		if (res != -1)
			res = do_mount(real_mnt, &type, stbuf.st_mode & S_IFMT,
				       fd, opts, dev, &source, &mnt_opts,
				       stbuf.st_size);
	} else
		restore_privs();

	if (currdir_fd != -1) {
		fchdir(currdir_fd);
		close(currdir_fd);
	}
	if (mountpoint_fd != -1)
		fcntl(mountpoint_fd, F_SETFD, FD_CLOEXEC);

	if (res == -1) {
		close(fd);
		if (mountpoint_fd != -1)
			close(mountpoint_fd);
		return -1;
	}

	if (geteuid() == 0) {
		res = add_mount(source, mnt, type, mnt_opts);
		if (res == -1) {
			if (isdir && mountpoint_fd != -1) {
				res = fchdir(mountpoint_fd);
				if (res == -1) {
					close(mountpoint_fd);
					close(fd);
					return -1;
				}
			}
			umount2(real_mnt, 2); /* lazy umount */
			if (mountpoint_fd != -1)
				close(mountpoint_fd);
			close(fd);
			return -1;
		}
	}

	if (mountpoint_fd != -1)
		close(mountpoint_fd);

	free(source);
	free(type);
	free(mnt_opts);
	free(dev);

	return fd;
}

static int send_fd(int sock_fd, int fd)
{
	int retval;
	struct msghdr msg;
	struct cmsghdr *p_cmsg;
	struct iovec vec;
	size_t cmsgbuf[CMSG_SPACE(sizeof(fd)) / sizeof(size_t)];
	int *p_fds;
	char sendchar = 0;

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	p_cmsg = CMSG_FIRSTHDR(&msg);
	p_cmsg->cmsg_level = SOL_SOCKET;
	p_cmsg->cmsg_type = SCM_RIGHTS;
	p_cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
	p_fds = (int *) CMSG_DATA(p_cmsg);
	*p_fds = fd;
	msg.msg_controllen = p_cmsg->cmsg_len;
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &vec;
	msg.msg_iovlen = 1;
	msg.msg_flags = 0;
	/* "To pass file descriptors or credentials you need to send/read at
	 * least one byte" (man 7 unix) */
	vec.iov_base = &sendchar;
	vec.iov_len = sizeof(sendchar);
	while ((retval = sendmsg(sock_fd, &msg, 0)) == -1 && errno == EINTR);
	if (retval != 1) {
		perror("sending file descriptor");
		return -1;
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"%s: [options] mountpoint\n"
		"Options:\n"
		" -h		    print help\n"
		" -V		    print version\n"
		" -o opt[,opt...]   mount options\n"
		" -u		    unmount\n"
		" -q		    quiet\n"
		" -z		    lazy unmount\n",
		progname);
	exit(1);
}

static void show_version(void)
{
	printf("fusermount version: %s\n", PACKAGE_VERSION);
	exit(0);
}

int main(int argc, char *argv[])
{
	int ch;
	int fd;
	int res;
	char *origmnt;
	char *mnt;
	static int unmount = 0;
	static int lazy = 0;
	static int quiet = 0;
	char *commfd;
	int cfd;
	const char *opts = "";

	static const struct option long_opts[] = {
		{"unmount", no_argument, NULL, 'u'},
		{"lazy",    no_argument, NULL, 'z'},
		{"quiet",   no_argument, NULL, 'q'},
		{"help",    no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{0, 0, 0, 0}};

	progname = strdup(argv[0]);
	if (progname == NULL) {
		fprintf(stderr, "%s: failed to allocate memory\n", argv[0]);
		exit(1);
	}

	while ((ch = getopt_long(argc, argv, "hVo:uzq", long_opts,
				 NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage();
			break;

		case 'V':
			show_version();
			break;

		case 'o':
			opts = optarg;
			break;

		case 'u':
			unmount = 1;
			break;

		case 'z':
			lazy = 1;
			break;

		case 'q':
			quiet = 1;
			break;

		default:
			exit(1);
		}
	}

	if (lazy && !unmount) {
		fprintf(stderr, "%s: -z can only be used with -u\n", progname);
		exit(1);
	}

	if (optind >= argc) {
		fprintf(stderr, "%s: missing mountpoint argument\n", progname);
		exit(1);
	} else if (argc > optind + 1) {
		fprintf(stderr, "%s: extra arguments after the mountpoint\n",
			progname);
		exit(1);
	}

	origmnt = argv[optind];

	drop_privs();
	mnt = fuse_mnt_resolve_path(progname, origmnt);
	restore_privs();
	if (mnt == NULL)
		exit(1);

	umask(033);
	if (unmount) {
		if (geteuid() == 0)
			res = unmount_fuse(mnt, quiet, lazy);
		else {
			res = umount2(mnt, lazy ? 2 : 0);
			if (res == -1 && !quiet)
				fprintf(stderr,
					"%s: failed to unmount %s: %s\n",
					progname, mnt, strerror(errno));
		}
		if (res == -1)
			exit(1);
		return 0;
	}

	commfd = getenv(FUSE_COMMFD_ENV);
	if (commfd == NULL) {
		fprintf(stderr, "%s: old style mounting not supported\n",
			progname);
		exit(1);
	}

	fd = mount_fuse(mnt, opts);
	if (fd == -1)
		exit(1);

	cfd = atoi(commfd);
	res = send_fd(cfd, fd);
	if (res == -1)
		exit(1);

	return 0;
}
#endif
int main(){
    return 0;
}
