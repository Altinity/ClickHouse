/// Implementation based on Musl's posix_spawn, with file actions support
/// restored for use by AWS-LC's split-handshake tests (and any other caller).

#define _GNU_SOURCE
#include <spawn.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <syscall.h>
#include <sys/signal.h>
#include <pthread.h>
#include <spawn.h>
#include <errno.h>
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

void __get_handler_set(sigset_t *);

static int child(void *args_vp)
{
	int ret;
	struct args *args = args_vp;
	int p = args->p[1];
	const posix_spawnattr_t *restrict attr = args->attr;

	close(args->p[0]);

	/* Close-on-exec flag may have been lost if we moved the pipe
	 * to a different fd. We don't use F_DUPFD_CLOEXEC above because
	 * it would fail on older kernels and atomicity is not needed --
	 * in this process there are no threads or signal handlers. */
	__syscall(SYS_fcntl, p, F_SETFD, FD_CLOEXEC);

	pthread_sigmask(SIG_SETMASK, (attr->__flags & POSIX_SPAWN_SETSIGMASK)
		? &attr->__ss : &args->oldmask, 0);

	/* Process file actions in POSIX-specified order (oldest-added first).
	 * The linked list is built by prepending, so we traverse to the tail
	 * and walk backwards via prev pointers. */
	if (args->fa) {
		struct fdop *op = (struct fdop *)args->fa->__actions;
		struct fdop *tail;
		for (tail = op; tail && tail->next; tail = tail->next);
		for (op = tail; op; op = op->prev) {
			long r;
			switch (op->cmd) {
			case FDOP_CLOSE:
				__syscall(SYS_close, op->fd);
				break;
			case FDOP_DUP2:
				if (op->srcfd == op->fd) {
					r = __syscall(SYS_fcntl, op->fd, F_GETFD);
					if (r < 0) { ret = -(int)r; goto fail; }
					if (r & FD_CLOEXEC) {
						r = __syscall(SYS_fcntl, op->fd, F_SETFD, (int)r & ~FD_CLOEXEC);
						if (r < 0) { ret = -(int)r; goto fail; }
					}
				} else {
#ifdef SYS_dup2
					r = __syscall(SYS_dup2, op->srcfd, op->fd);
#else
					r = __syscall(SYS_dup3, op->srcfd, op->fd, 0);
#endif
					if (r < 0) { ret = -(int)r; goto fail; }
				}
				break;
			case FDOP_OPEN: {
#ifdef SYS_open
				int fd = __syscall(SYS_open, op->path, op->oflag, op->mode);
#else
				int fd = __syscall(SYS_openat, AT_FDCWD, op->path, op->oflag, op->mode);
#endif
				if (fd < 0) { ret = -fd; goto fail; }
				if (fd != op->fd) {
#ifdef SYS_dup2
					r = __syscall(SYS_dup2, fd, op->fd);
#else
					r = __syscall(SYS_dup3, fd, op->fd, 0);
#endif
					__syscall(SYS_close, fd);
					if (r < 0) { ret = -(int)r; goto fail; }
				}
				break;
			}
			case FDOP_CHDIR:
				r = __syscall(SYS_chdir, op->path);
				if (r < 0) { ret = -(int)r; goto fail; }
				break;
			case FDOP_FCHDIR:
				r = __syscall(SYS_fchdir, op->fd);
				if (r < 0) { ret = -(int)r; goto fail; }
				break;
			}
		}
	}

	args->exec(args->path, args->argv, args->envp);
	ret = errno;

fail:
	/* Since sizeof errno < PIPE_BUF, the write is atomic. */
	if (ret) while (__syscall(SYS_write, p, &ret, sizeof ret) < 0);
	_exit(127);
}


int __posix_spawnx(pid_t *restrict res, const char *restrict path,
	int (*exec)(const char *, char *const *, char *const *),
	const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *restrict attr,
	char *const argv[restrict], char *const envp[restrict])
{
	pid_t pid;
	char stack[1024];
	int ec=0, cs;
	struct args args;

	if (pipe2(args.p, O_CLOEXEC))
		return errno;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);

	args.path = path;
	args.exec = exec;
	args.fa = fa;
	args.attr = attr ? attr : &(const posix_spawnattr_t){0};
	args.argv = argv;
	args.envp = envp;

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
