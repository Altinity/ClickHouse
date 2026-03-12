/// Full posix_spawn implementation based on upstream musl.
/// https://git.musl-libc.org/cgit/musl/tree/src/process/posix_spawn.c
///
/// Adaptations from upstream:
/// - Uses glibc sysroot field names (__ss/__sd instead of __mask/__def)
///   since <spawn.h> resolves to the glibc sysroot header in this build.
/// - Takes exec function as a parameter (__posix_spawnx) because glibc's
///   posix_spawnattr_t has no __fn field for posix_spawnp dispatch.
/// - Omits LOCK(__abort_lock) / UNLOCK(__abort_lock) (musl-internal lock
///   guarding against abort() races; not available outside musl).
/// - Omits __get_handler_set / __libc_sigaction signal disposition reset
///   (musl internals not available; signals are blocked for the child's
///   brief pre-exec window, so parent handlers cannot fire).
/// - Uses clone() instead of musl-internal __clone().

#define _GNU_SOURCE
#include <spawn.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <syscall.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include "syscall.h"

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

struct args {
	int p[2];
	sigset_t oldmask;
	const char *path;
	int (*exec)(const char *, char *const *, char *const *);
	const posix_spawn_file_actions_t *fa;
	const posix_spawnattr_t *restrict attr;
	char *const *argv, *const *envp;
};

static int __sys_dup2(int old, int new)
{
#ifdef SYS_dup2
	return __syscall(SYS_dup2, old, new);
#else
	return __syscall(SYS_dup3, old, new, 0);
#endif
}

static int child(void *args_vp)
{
	int i, ret;
	struct sigaction sa = {0};
	struct args *args = args_vp;
	int p = args->p[1];
	const posix_spawn_file_actions_t *fa = args->fa;
	const posix_spawnattr_t *restrict attr = args->attr;

	close(args->p[0]);

	if (attr->__flags & POSIX_SPAWN_SETSIGDEF) {
		for (i=1; i<_NSIG; i++) {
			if (sigismember(&attr->__sd, i)) {
				sa.sa_handler = SIG_DFL;
				__syscall(SYS_rt_sigaction, i, &sa, 0, _NSIG/8);
			}
		}
	}

	if (attr->__flags & POSIX_SPAWN_SETSID)
		if ((ret=__syscall(SYS_setsid)) < 0)
			goto fail;

	if (attr->__flags & POSIX_SPAWN_SETPGROUP)
		if ((ret=__syscall(SYS_setpgid, 0, attr->__pgrp)))
			goto fail;

	if (attr->__flags & POSIX_SPAWN_RESETIDS)
		if ((ret=__syscall(SYS_setgid, __syscall(SYS_getgid))) ||
		    (ret=__syscall(SYS_setuid, __syscall(SYS_getuid))) )
			goto fail;

	if (fa && fa->__actions) {
		struct fdop *op;
		int fd;
		for (op = fa->__actions; op->next; op = op->next);
		for (; op; op = op->prev) {
			/* It's possible that a file operation would clobber
			 * the pipe fd used for synchronizing with the
			 * parent. To avoid that, we dup the pipe onto
			 * an unoccupied fd. */
			if (op->fd == p) {
				ret = __syscall(SYS_dup, p);
				if (ret < 0) goto fail;
				__syscall(SYS_close, p);
				p = ret;
			}
			switch(op->cmd) {
			case FDOP_CLOSE:
				__syscall(SYS_close, op->fd);
				break;
			case FDOP_DUP2:
				fd = op->srcfd;
				if (fd == p) {
					ret = -EBADF;
					goto fail;
				}
				if (fd != op->fd) {
					if ((ret=__sys_dup2(fd, op->fd))<0)
						goto fail;
				} else {
					ret = __syscall(SYS_fcntl, fd, F_GETFD);
					ret = __syscall(SYS_fcntl, fd, F_SETFD,
						ret & ~FD_CLOEXEC);
					if (ret<0)
						goto fail;
				}
				break;
			case FDOP_OPEN:
#ifdef SYS_open
				fd = __syscall(SYS_open, op->path, op->oflag, op->mode);
#else
				fd = __syscall(SYS_openat, AT_FDCWD, op->path, op->oflag, op->mode);
#endif
				if ((ret=fd) < 0) goto fail;
				if (fd != op->fd) {
					if ((ret=__sys_dup2(fd, op->fd))<0)
						goto fail;
					__syscall(SYS_close, fd);
				}
				break;
			case FDOP_CHDIR:
				ret = __syscall(SYS_chdir, op->path);
				if (ret<0) goto fail;
				break;
			case FDOP_FCHDIR:
				ret = __syscall(SYS_fchdir, op->fd);
				if (ret<0) goto fail;
				break;
			}
		}
	}

	/* Close-on-exec flag may have been lost if we moved the pipe
	 * to a different fd. We don't use F_DUPFD_CLOEXEC above because
	 * it would fail on older kernels and atomicity is not needed --
	 * in this process there are no threads or signal handlers. */
	__syscall(SYS_fcntl, p, F_SETFD, FD_CLOEXEC);

	pthread_sigmask(SIG_SETMASK, (attr->__flags & POSIX_SPAWN_SETSIGMASK)
		? &attr->__ss : &args->oldmask, 0);

	args->exec(args->path, args->argv, args->envp);
	ret = -errno;

fail:
	/* Since sizeof errno < PIPE_BUF, the write is atomic. */
	ret = -ret;
	if (ret) {
		int r;
		do r = __syscall(SYS_write, p, &ret, sizeof ret);
		while (r<0 && r!=-EPIPE);
	}
	_exit(127);
}


int __posix_spawnx(pid_t *restrict res, const char *restrict path,
	int (*exec)(const char *, char *const *, char *const *),
	const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *restrict attr,
	char *const argv[restrict], char *const envp[restrict])
{
	pid_t pid;
	char stack[1024+PATH_MAX];
	int ec=0, cs;
	struct args args;

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
		goto fail;
	}

	pid = clone(child, stack+sizeof stack,
		CLONE_VM|CLONE_VFORK|SIGCHLD, &args);
	close(args.p[1]);

	if (pid > 0) {
		if (read(args.p[0], &ec, sizeof ec) != sizeof ec) ec = 0;
		else waitpid(pid, &(int){0}, 0);
	} else {
		ec = -pid;
	}

	close(args.p[0]);

	if (!ec && res) *res = pid;

fail:
	pthread_sigmask(SIG_SETMASK, &args.oldmask, 0);
	pthread_setcancelstate(cs, 0);

	return ec;
}

int posix_spawn(pid_t *restrict res, const char *restrict path,
	const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *restrict attr,
	char *const argv[restrict], char *const envp[restrict])
{
	return __posix_spawnx(res, path, execve, fa, attr, argv, envp);
}
