#include <kernel/syscalls.h>
#include <kernel/sock.h>
#include <kernel/abi.h>
#include <kernel/file.h>
#include <errno.h>

syscallret_t syscall_setsockopt(context_t *, int fd, int level, int optname, void *val, size_t len) {
	syscallret_t ret = {
		.ret = -1
	};

	if (level != SOL_SOCKET) {
		ret.errno = ENOPROTOOPT;
		return ret;
	}

	void *buffer = NULL;
	if (val) {
		buffer = alloc(len);
		if (buffer == NULL) {
			ret.errno = ENOMEM;
			return ret;
		}
		memcpy(buffer, val, len);
	}

	file_t *file = fd_get(fd);
	if (file == NULL) {
		free(buffer);
		ret.errno = EBADF;
		return ret;
	}

	socket_t *socket = SOCKFS_SOCKET_FROM_NODE(file->vnode); 


	MUTEX_ACQUIRE(&socket->mutex, false);
	switch (optname) {
		case SO_BINDTODEVICE:
			if (val) {
				socket->netdev = netdev_getdev(val);
				ret.errno = socket->netdev ? 0 : ENODEV;
			} else {
				socket->netdev = NULL;
			}
			break;
		case SO_BROADCAST:
			if (val == NULL) {
				ret.errno = EFAULT;
			} else {
				socket->broadcast = *((int *)buffer);
				ret.errno = 0;
			}
			break;
		default:
		ret.errno = ENOPROTOOPT;
	}

	MUTEX_RELEASE(&socket->mutex);

	ret.ret = ret.errno ? -1 : 0;

	fd_release(file);

	if (val)
		free(buffer);

	return ret;
}