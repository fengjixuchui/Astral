#include <kernel/syscalls.h>
#include <arch/cpu.h>

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

syscallret_t syscall_sigprocmask(context_t *, int how, sigset_t *set, sigset_t *oldset) {
	syscallret_t ret = {
		.ret = -1
	};

	sigset_t new, old;

	if (set) {
		if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
			ret.errno = EINVAL;
			return ret;
		}

		new = *set;
	}
	
	signal_changemask(_cpu()->thread, how, set ? &new : NULL, oldset ? &old : NULL);

	if (oldset)
		*oldset = old;

	ret.ret = 0;
	ret.errno = 0;
	return ret;
}