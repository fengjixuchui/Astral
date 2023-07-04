#include <logging.h>
#include <arch/e9.h>
#include <arch/cpu.h>

#define SYSCALL_COUNT 23
#define LOGSTR(x) arch_e9_puts(x)

static char *name[] = {
	"print",
	"mmap",
	"openat",
	"read",
	"seek",
	"close",
	"archctl",
	"write",
	"getpid",
	"fstat",
	"fstatat",
	"fork",
	"execve",
	"exit",
	"waitpid",
	"munmap",
	"getdents",
	"dup",
	"dup2",
	"dup3",
	"fcntl",
	"chdir",
	"pipe2"
};

static char *args[] = {
	"N/A", // print will not have its argument printed
	"hint %p length %lu prot %d flags %d fd %d offset %ld", // mmap
	"dirfd %d path %s flags %d mode %o", // openat
	"fd %d buffer %p size %lu", // read
	"fd %d offset %ld whence %d", // seek
	"fd %d", // close
	"func %d arg %p", // archctl
	"fd %d buffer %p size %lu", // write
	"N/A", // getpid
	"fd %d ustat %p", // fstat
	"dirfd %d path %s ustat %p flags %d", // fstatat
	"N/A", // fork
	"path %s uargv %p uenvp %p", // execve
	"status %d", // exit
	"pid %d statusp %p options %d", // waitpid
	"addr %p length %lu", // munmap
	"dirfd %d buffer %p readmax %lu", // getdents
	"oldfd %d", // dup
	"oldfd %d newfd %d", // dup2
	"oldfd %d newfd %d flags %d", //dup3
	"fd %d cmd %d arg %lu", // fcntl
	"path %s", // chdir
	"flags %d", // pipe2
};

__attribute__((no_caller_saved_registers)) void arch_syscall_log(int syscall, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
	char argbuff[768];
	char printbuff[1024];

	thread_t *thread = _cpu()->thread;
	proc_t *proc = thread->proc;

	snprintf(argbuff, 768, syscall < SYSCALL_COUNT ? args[syscall] : "N/A", a1, a2, a3, a4, a5, a6);
	snprintf(printbuff, 1024, "\e[92msyscall: pid %d tid %d: %s: %s\n\e[0m", proc->pid, thread->tid, syscall < SYSCALL_COUNT ? name[syscall] : "invalid syscall", argbuff);

	LOGSTR(printbuff);
}

__attribute__((no_caller_saved_registers)) void arch_syscall_log_return(uint64_t ret, uint64_t errno) {
	char printbuff[1024];

	thread_t *thread = _cpu()->thread;
	proc_t *proc = thread->proc;

	snprintf(printbuff, 1024, "\e[94msyscall return: pid %d tid %d: %lu %lu\n\e[0m", proc->pid, thread->tid, ret, errno);
	LOGSTR(printbuff);
}