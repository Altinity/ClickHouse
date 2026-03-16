/// Based on musl's posix_spawn.c: https://git.musl-libc.org/cgit/musl/tree/src/process/posix_spawn.c
///
/// Compiled with the system compiler and linked only into the ssl-shim
/// and ssl-handshaker archives. The AWS-LC sources are compiled with
/// -Dposix_spawn=__ssl_posix_spawn so only their posix_spawn calls are
/// redirected here; the rest of ClickHouse continues to use the original
/// limited stub in glibc-compatibility, completely unaffected.
///
/// The child function uses raw inline-asm syscalls (not glibc wrappers)
/// because CLONE_VM shares the parent's TLS, and glibc's syscall()
/// would clobber the parent's errno.

#define _GNU_SOURCE
#include <spawn.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>

/* ── raw x86-64 syscall wrappers (no errno, no TLS) ────────────── */

static inline long raw_sc0(long n)
{
	long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n) : "rcx", "r11", "memory");
	return ret;
}

static inline long raw_sc1(long n, long a1)
{
	long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n), "D"(a1) : "rcx", "r11", "memory");
	return ret;
}

static inline long raw_sc2(long n, long a1, long a2)
{
	long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
	return ret;
}

static inline long raw_sc3(long n, long a1, long a2, long a3)
{
	long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
	return ret;
}

static inline long raw_sc4(long n, long a1, long a2, long a3, long a4)
{
	register long r10 __asm__("r10") = a4;
	long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
		: "rcx", "r11", "memory");
	return ret;
}

/* ── file-action constants & struct (must match glibc-compatibility.c) ── */

#define FDOP_CLOSE 1
#define FDOP_DUP2 2
#define FDOP_OPEN 3
#define FDOP_CHDIR 4
#define FDOP_FCHDIR 5

struct fdop {
	struct fdop *next, *prev;
	int cmd, fd, srcfd, oflag;
	mode_t mode;
	char path[];
};

struct spawn_args {
	int p[2];
	sigset_t oldmask;
	const char *path;
	int (*exec)(const char *, char *const *, char *const *);
	const posix_spawn_file_actions_t *fa;
	const posix_spawnattr_t *restrict attr;
	char *const *argv, *const *envp;
};

static int spawn_child(void *args_vp)
{
	int i;
	long ret;
	struct spawn_args *args = args_vp;
	int p = args->p[1];
	const posix_spawn_file_actions_t *fa = args->fa;
	const posix_spawnattr_t *restrict attr = args->attr;

	raw_sc1(SYS_close, args->p[0]);

	if (attr->__flags & POSIX_SPAWN_SETSIGDEF) {
		struct sigaction sa = {0};
		sa.sa_handler = SIG_DFL;
		for (i = 1; i < _NSIG; i++)
			if (sigismember(&attr->__sd, i))
				raw_sc4(SYS_rt_sigaction, i,
					(long)&sa, 0, _NSIG/8);
	}

	if (attr->__flags & POSIX_SPAWN_SETSID)
		if ((ret = raw_sc0(SYS_setsid)) < 0)
			goto fail;

	if (attr->__flags & POSIX_SPAWN_SETPGROUP)
		if ((ret = raw_sc2(SYS_setpgid, 0, attr->__pgrp)))
			goto fail;

	if (attr->__flags & POSIX_SPAWN_RESETIDS)
		if ((ret = raw_sc1(SYS_setgid, raw_sc0(SYS_getgid))) ||
		    (ret = raw_sc1(SYS_setuid, raw_sc0(SYS_getuid))))
			goto fail;

	if (fa && fa->__actions) {
		struct fdop *op;
		int fd;
		for (op = fa->__actions; op->next; op = op->next);
		for (; op; op = op->prev) {
			if (op->fd == p) {
				ret = raw_sc1(SYS_dup, p);
				if (ret < 0) goto fail;
				raw_sc1(SYS_close, p);
				p = ret;
			}
			switch (op->cmd) {
			case FDOP_CLOSE:
				raw_sc1(SYS_close, op->fd);
				break;
			case FDOP_DUP2:
				fd = op->srcfd;
				if (fd == p) {
					ret = -EBADF;
					goto fail;
				}
				if (fd != op->fd) {
					if ((ret = raw_sc2(SYS_dup2,
							fd, op->fd)) < 0)
						goto fail;
				} else {
					ret = raw_sc3(SYS_fcntl, fd,
						F_GETFD, 0);
					if (ret < 0) goto fail;
					ret = raw_sc3(SYS_fcntl, fd,
						F_SETFD, ret & ~FD_CLOEXEC);
					if (ret < 0) goto fail;
				}
				break;
			case FDOP_OPEN:
				fd = raw_sc4(SYS_openat, AT_FDCWD,
					(long)op->path, op->oflag, op->mode);
				if ((ret = fd) < 0) goto fail;
				if (fd != op->fd) {
					if ((ret = raw_sc2(SYS_dup2,
							fd, op->fd)) < 0)
						goto fail;
					raw_sc1(SYS_close, fd);
				}
				break;
			case FDOP_CHDIR:
				ret = raw_sc1(SYS_chdir, (long)op->path);
				if (ret < 0) goto fail;
				break;
			case FDOP_FCHDIR:
				ret = raw_sc1(SYS_fchdir, op->fd);
				if (ret < 0) goto fail;
				break;
			}
		}
	}

	raw_sc3(SYS_fcntl, p, F_SETFD, FD_CLOEXEC);

	raw_sc4(SYS_rt_sigprocmask, SIG_SETMASK,
		(long)((attr->__flags & POSIX_SPAWN_SETSIGMASK)
			? &attr->__ss : &args->oldmask),
		0, _NSIG/8);

	args->exec(args->path, args->argv, args->envp);
	ret = -errno;

fail:
	ret = -ret;
	if (ret) {
		long r;
		do r = raw_sc3(SYS_write, p, (long)&ret, sizeof ret);
		while (r < 0 && r != -EPIPE);
	}
	_exit(127);
}


static int spawnx_impl(pid_t *restrict res, const char *restrict path,
	int (*exec)(const char *, char *const *, char *const *),
	const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *restrict attr,
	char *const argv[restrict], char *const envp[restrict])
{
	pid_t pid;
	char stack[1024+PATH_MAX];
	int ec = 0, cs;
	struct spawn_args args;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);

	args.path = path;
	args.exec = exec;
	args.fa = fa;
	args.attr = attr ? attr : &(const posix_spawnattr_t){0};
	args.argv = argv;
	args.envp = envp;

	sigset_t allsigs;
	sigfillset(&allsigs);
	pthread_sigmask(SIG_BLOCK, &allsigs, &args.oldmask);

	if (pipe2(args.p, O_CLOEXEC)) {
		ec = errno;
		goto out;
	}

	pid = clone(spawn_child, stack + sizeof stack,
		CLONE_VM | CLONE_VFORK | SIGCHLD, &args);
	close(args.p[1]);

	if (pid > 0) {
		if (read(args.p[0], &ec, sizeof ec) != sizeof ec) ec = 0;
		else waitpid(pid, &(int){0}, 0);
	} else {
		ec = -pid;
	}

	close(args.p[0]);

	if (!ec && res) *res = pid;

out:
	pthread_sigmask(SIG_SETMASK, &args.oldmask, 0);
	pthread_setcancelstate(cs, 0);

	return ec;
}

/* Called by AWS-LC objects compiled with -Dposix_spawn=__ssl_posix_spawn.
 * Only those translation units are redirected; all other CH code uses
 * the original posix_spawn from glibc-compatibility. */
int __ssl_posix_spawn(pid_t *restrict res, const char *restrict path,
	const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *restrict attr,
	char *const argv[restrict], char *const envp[restrict])
{
	return spawnx_impl(res, path, execve, fa, attr, argv, envp);
}
