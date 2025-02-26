#include <kernel/syscalls.h>
#include <kernel/abi.h>
#include <kernel/sock.h>
#include <errno.h>

syscallret_t syscall_socket(context_t *, int domain, int type, int protocol) {
	syscallret_t ret = {
		.ret = -1
	};

	int socktype = -1;

	switch (domain) {
		case AF_INET:
			if (type == SOCK_STREAM)
				socktype = SOCKET_TYPE_TCP;
			if (type == SOCK_DGRAM)
				socktype = SOCKET_TYPE_UDP;
			break;
		case AF_LOCAL:
			socktype = SOCKET_TYPE_LOCAL;
	}

	if (socktype == -1) {
		ret.errno = EAFNOSUPPORT;
		return ret;
	}

	socket_t *socket = socket_create(socktype);
	if (socket == NULL) {
		ret.errno = ENOMEM;
		return ret;
	}

	vnode_t *vnode;
	ret.errno = sockfs_newsocket(&vnode, socket);
	if (ret.errno) {
		socket->ops->destroy(socket);
		return ret;
	}

	file_t *file;
	int fd;
	
	ret.errno = fd_new(0, &file, &fd);
	if (ret.errno) {
		// socket gets deleted by node cleanup
		VOP_RELEASE(vnode);
		return ret;
	}

	file->vnode = vnode;
	file->flags = FILE_READ | FILE_WRITE;
	file->offset = 0;
	file->mode = 0777;

	ret.ret = fd;

	return ret;
}
