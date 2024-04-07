#include <kernel/vfs.h>
#include <kernel/slab.h>
#include <kernel/alloc.h>
#include <kernel/timekeeper.h>
#include <kernel/vmm.h>
#include <logging.h>
#include <errno.h>
#include <kernel/abi.h>
#include <kernel/poll.h>
#include <kernel/sock.h>

static scache_t *nodecache;
static uintmax_t currentinode;

int sockfs_open(vnode_t **node, int flags, cred_t *cred) {
	return ENOSYS; // not needed
}

int sockfs_close(vnode_t *node, int flags, cred_t *cred) {
	return ENOSYS; // not needed
}

int sockfs_read(vnode_t *node, void *buffer, size_t size, uintmax_t offset, int flags, size_t *readc, cred_t *cred) {
	socket_t *socket = SOCKFS_SOCKET_FROM_NODE(node);
	return socket->ops->recv(socket, NULL, buffer, size, flags, readc);
}

int sockfs_write(vnode_t *node, void *buffer, size_t size, uintmax_t offset, int flags, size_t *writec, cred_t *cred) {
	socket_t *socket = SOCKFS_SOCKET_FROM_NODE(node);
	return socket->ops->send(socket, NULL, buffer, size, flags, writec);
}

int sockfs_poll(vnode_t *node, polldata_t *data, int events) {
	socket_t *socket = SOCKFS_SOCKET_FROM_NODE(node);
	return socket->ops->poll(socket, data, events);
}

int sockfs_getattr(vnode_t *node, vattr_t *attr, cred_t *cred) {
	socketnode_t *socketnode = (socketnode_t *)node;

	VOP_LOCK(node);
	*attr = socketnode->attr;
	attr->type = node->type;
	VOP_UNLOCK(node);

	return 0;
}

int sockfs_setattr(vnode_t *node, vattr_t *attr, cred_t *cred) {
	socketnode_t *socketnode = (socketnode_t *)node;
	VOP_LOCK(node);

	socketnode->attr.gid = attr->gid;
	socketnode->attr.uid = attr->uid;
	socketnode->attr.mode = attr->mode;
	socketnode->attr.atime = attr->atime;
	socketnode->attr.mtime = attr->mtime;
	socketnode->attr.ctime = attr->ctime;

	VOP_UNLOCK(node);
	return 0;
}

// arg can be in userspace
int sockfs_ioctl(vnode_t *node, unsigned long request, void *arg, int *result) {
	switch (request) {
		case SIOCGIFHWADDR:
			ifreq_t *ifreq = arg;
			netdev_t *netdev = netdev_getdev(ifreq->name);
			if (netdev) {
				memcpy(ifreq->addr.addr, netdev->mac.address, sizeof(mac_t));
				return 0;
			} else {
				return ENODEV;
			}
		default:
			return EINVAL;
	}
}

int sockfs_inactive(vnode_t *node) {
	socket_t *socket = SOCKFS_SOCKET_FROM_NODE(node);
	VOP_LOCK(node);
	socket->ops->destroy(socket);
	slab_free(nodecache, node);
	return 0;
}

static int sockfs_enodev() {
	return ENODEV;
}

static vops_t vnops = {
	.create = sockfs_enodev,
	.open = sockfs_open,
	.close = sockfs_close,
	.getattr = sockfs_getattr,
	.setattr = sockfs_setattr,
	.lookup = sockfs_enodev,
	.poll = sockfs_poll,
	.read = sockfs_read,
	.write = sockfs_write,
	.access = sockfs_enodev,
	.unlink = sockfs_enodev,
	.link = sockfs_enodev,
	.symlink = sockfs_enodev,
	.readlink = sockfs_enodev,
	.inactive = sockfs_inactive,
	.mmap = sockfs_enodev,
	.munmap = sockfs_enodev,
	.getdents = sockfs_enodev,
	.resize = sockfs_enodev,
	.rename = sockfs_enodev,
	.ioctl = sockfs_ioctl
};

static void ctor(scache_t *cache, void *obj) {
	socketnode_t *node = obj;
	memset(node, 0, sizeof(socketnode_t));
	VOP_INIT(&node->vnode, &vnops, 0, V_TYPE_SOCKET, NULL);
	node->attr.inode = ++currentinode;
}

void sockfs_init() {
	nodecache = slab_newcache(sizeof(socketnode_t), 0, ctor, ctor);
	__assert(nodecache);
}

int sockfs_newsocket(vnode_t **nodep, socket_t *socket) {
	__assert(_cpu()->intstatus);
	vnode_t *vnode = slab_allocate(nodecache);
	socketnode_t *node = (socketnode_t *)vnode;
	if (node == NULL)
		return ENOMEM;

	node->socket = socket;
	*nodep = vnode;
	POLL_INITHEADER(&socket->pollheader);

	return 0;
}