#include <kernel/tmpfs.h>
#include <logging.h>
#include <kernel/slab.h>
#include <errno.h>
#include <kernel/alloc.h>
#include <kernel/timekeeper.h>
#include <kernel/vmm.h>
#include <util.h>
#include <kernel/abi.h>
#include <kernel/devfs.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>
#include <string.h>
#include <kernel/vmmcache.h>

static scache_t *nodecache;
static tmpfsnode_t *newnode(vfs_t *vfs, int type);
static void freenode(tmpfsnode_t *node);
static vfsops_t vfsops;
static uintmax_t currid = 1;

static int tmpfs_mount(vfs_t **vfs, vnode_t *mountpoint, vnode_t *backing, void *data) {
	tmpfs_t *tmpfs = alloc(sizeof(tmpfs_t));
	if (tmpfs == NULL)
		return ENOMEM;

	*vfs = (vfs_t *)tmpfs;
	tmpfs->vfs.ops = &vfsops;
	tmpfs->id = __atomic_fetch_add(&currid, 1, __ATOMIC_SEQ_CST);

	return 0;
}

#define DATAMMUFLAGS (ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_NOEXEC)

static int tmpfs_getattr(vnode_t *node, vattr_t *attr, cred_t *cred) {
	VOP_LOCK(node);
	tmpfsnode_t *tmpnode = (tmpfsnode_t *)node;
	*attr = tmpnode->attr;
	attr->blocksused = ROUND_UP(attr->size, PAGE_SIZE) / PAGE_SIZE;
	attr->devmajor = 0;
	attr->devminor = ((tmpfs_t *)node->vfs)->id;
	VOP_UNLOCK(node);
	return 0;
}

static int tmpfs_setattr(vnode_t *node, vattr_t *attr, int which, cred_t *cred) {
	VOP_LOCK(node);
	tmpfsnode_t *tmpnode = (tmpfsnode_t *)node;

	if (which & V_ATTR_GID)
		tmpnode->attr.gid = attr->gid;
	if (which & V_ATTR_UID)
		tmpnode->attr.uid = attr->uid;
	if (which & V_ATTR_MODE)
		tmpnode->attr.mode = attr->mode;
	if (which & V_ATTR_ATIME)
		tmpnode->attr.atime = attr->atime;
	if (which & V_ATTR_MTIME)
		tmpnode->attr.mtime = attr->mtime;
	if (which & V_ATTR_CTIME)
		tmpnode->attr.ctime = attr->ctime;

	VOP_UNLOCK(node);
	return 0;
}

static int tmpfs_lookup(vnode_t *parent, char *name, vnode_t **result, cred_t *cred) {
	tmpfsnode_t *tmpparent = (tmpfsnode_t *)parent;
	vnode_t *child = NULL;
	if (parent->type != V_TYPE_DIR)
		return ENOTDIR;

	VOP_LOCK(parent);
	void *r;
	int err = hashtable_get(&tmpparent->children, &r, name, strlen(name));
	if (err) {
		VOP_UNLOCK(parent);
		return err;
	}

	child = r;
	VOP_HOLD(child);
	VOP_UNLOCK(parent);
	*result = child;
	return 0;
}

static int tmpfs_create(vnode_t *parent, char *name, vattr_t *attr, int type, vnode_t **result, cred_t *cred) {
	if (parent->type != V_TYPE_DIR)
		return ENOTDIR;

	tmpfsnode_t *parenttmpnode = (tmpfsnode_t *)parent;
	size_t namelen = strlen(name);
	void *v;

	VOP_LOCK(parent);
	if (hashtable_get(&parenttmpnode->children, &v, name, namelen) == 0) {
		VOP_UNLOCK(parent);
		return EEXIST;
	}

	tmpfsnode_t *tmpnode = newnode(parent->vfs, type);
	vnode_t *node = (vnode_t *)tmpnode;
	if (tmpnode == NULL) {
		VOP_UNLOCK(parent);
		return ENOMEM;
	}

	timespec_t time = timekeeper_time();

	vattr_t tmpattr = *attr;
	tmpattr.atime = time;
	tmpattr.ctime = time;
	tmpattr.mtime = time;
	tmpattr.size = 0;
	tmpattr.type = type;
	int error = tmpfs_setattr(node, &tmpattr, V_ATTR_ALL, cred);
	tmpnode->attr.nlinks = 1;

	if (error) {
		VOP_UNLOCK(parent);
		VOP_RELEASE(node);
		return error; 
	}

	// if a dir, create the .. entry (the . entry is already made in newnode() if it has the dir type)
	if (type == V_TYPE_DIR) {
		error = hashtable_set(&tmpnode->children, parenttmpnode, "..", 2, true);
		if (error) {
			VOP_UNLOCK(parent);
			VOP_RELEASE(node);
			return error;
		}
	}

	error = hashtable_set(&parenttmpnode->children, tmpnode, name, namelen, true);
	VOP_UNLOCK(parent);

	if (error) {
		VOP_RELEASE(node);
		return error;
	}

	// hold the .. reference (done here to reduce cleanup)
	if (type == V_TYPE_DIR)
		VOP_HOLD(parent);

	node->vfs = parent->vfs;
	*result = node;
	VOP_HOLD(node);

	return error;
}

static int tmpfs_open(vnode_t **nodep, int flag, cred_t *cred) {
	vnode_t *node = *nodep;
	tmpfsnode_t *tmpnode = (tmpfsnode_t *)node;

	if (node->type == V_TYPE_CHDEV || node->type == V_TYPE_BLKDEV) {
		vnode_t *devnode;
		int err = devfs_getnode(node, tmpnode->attr.rdevmajor, tmpnode->attr.rdevminor, &devnode);
		if (err)
			return err;

		VOP_RELEASE(node);
		VOP_HOLD(devnode);
		*nodep = devnode; 
	}

	return 0;
}

static int tmpfs_close(vnode_t *node, int flag, cred_t *cred) {
	return 0;
}

static int tmpfs_root(vfs_t *vfs, vnode_t **vnode) {
	// FIXME lock vfs
	if (vfs->root) {
		*vnode = vfs->root;
		return 0;
	}

	tmpfsnode_t *node = newnode(vfs, V_TYPE_DIR);
	if (node == NULL)
		return ENOMEM;

	*vnode = (vnode_t *)node;
	vfs->root = *vnode;

	node->vnode.flags |= V_FLAGS_ROOT;

	return 0;
}

static int tmpfs_read(vnode_t *node, void *buffer, size_t size, uintmax_t offset, int flags, size_t *readc, cred_t *cred) {
	if (node->type == V_TYPE_DIR)
		return EISDIR;

	if (node->type != V_TYPE_REGULAR)
		return EINVAL;

	__assert(!"tmpfs_read is handled by the page cache");
}

static int tmpfs_write(vnode_t *node, void *buffer, size_t size, uintmax_t offset, int flags, size_t *writec, cred_t *cred) {
	if (node->type == V_TYPE_DIR)
		return EISDIR;

	if (node->type != V_TYPE_REGULAR)
		return EINVAL;

	__assert(!"tmpfs_read is handled by the page cache");
}

static int tmpfs_access(vnode_t *node, mode_t mode, cred_t *cred) {
	// TODO permission checks
	return 0;
}

static int tmpfs_unlink(vnode_t *node, char *name, cred_t *cred) {
	tmpfsnode_t *tmpnode = (tmpfsnode_t *)node;
	if (node->type != V_TYPE_DIR)
		return ENOTDIR;

	if (node->vfsmounted)
		return EBUSY;

	size_t namelen = strlen(name);
	void *r;
	VOP_LOCK(node);

	int err = hashtable_get(&tmpnode->children, &r, name, namelen);
	if (err) {
		VOP_UNLOCK(node);
		return err;
	}

	vnode_t *unlinknode = r;
	tmpfsnode_t *unlinktmpnode = r;

	err = hashtable_remove(&tmpnode->children, name, namelen);
	VOP_UNLOCK(node);
	if (err)
		return err;

	VOP_LOCK(unlinknode);
	--unlinktmpnode->attr.nlinks;
	VOP_UNLOCK(unlinknode);
	VOP_RELEASE(unlinknode);

	return 0;
}

static int tmpfs_link(vnode_t *node, vnode_t *dir, char *name, cred_t *cred) {
	if (node->vfs != dir->vfs)
		return EXDEV;

	if (dir->type != V_TYPE_DIR)
		return ENOTDIR;

	if (node->type == V_TYPE_DIR)
		return EISDIR;

	tmpfsnode_t *tmpdir = (tmpfsnode_t *)dir;
	size_t namelen = strlen(name);
	void *v;
	VOP_LOCK(dir);
	if (hashtable_get(&tmpdir->children, &v, name, namelen) == 0) {
		VOP_UNLOCK(node);
		return EEXIST;
	}

	int error = hashtable_set(&tmpdir->children, node, name, namelen, true);
	VOP_UNLOCK(dir);
	if (error)
		return error;

	VOP_HOLD(node);
	tmpfsnode_t *tmpnode = (tmpfsnode_t *)node;
	++tmpnode->attr.nlinks;

	return 0;
}

static int tmpfs_rename(vnode_t *source, char *oldname, vnode_t *target, char *newname, int flags) {
	if (source->vfs != target->vfs)
		return EXDEV;

	if (source->type != V_TYPE_DIR || target->type != V_TYPE_DIR)
		return ENOTDIR;

	tmpfsnode_t *sourcedir = (tmpfsnode_t *)source;
	tmpfsnode_t *targetdir = (tmpfsnode_t *)target;

	size_t oldlen = strlen(oldname);
	size_t newlen = strlen(newname);

	if (target != source) {
		VOP_LOCK(source);
		VOP_LOCK(target);
	} else {
		VOP_LOCK(target);
	}

	// get original node
	void *v;
	int error = hashtable_get(&sourcedir->children, &v, oldname, oldlen);
	if (error)
		goto cleanup;

	vnode_t *node = v;
	vnode_t *oldnode = NULL;

	// get old node in target
	error = hashtable_get(&targetdir->children, &v, newname, newlen);
	if (error != ENOENT && error != 0)
		goto cleanup;
	else if (error == 0) // found
		oldnode = v;

	if (node->vfsmounted || (oldnode && oldnode->vfsmounted)) {
		error = EBUSY;
		goto cleanup;
	}

	// same link, rename is a no-op
	if (sourcedir == targetdir && strcmp(oldname, newname) == 0)
		goto cleanup;

	// link node to new name
	error = hashtable_set(&targetdir->children, node, newname, newlen, true);
	if (error)
		goto cleanup;

	// remove old link. should not fail
	__assert(hashtable_remove(&sourcedir->children, oldname, oldlen) == 0);

	if (oldnode)
		VOP_RELEASE(oldnode);

	cleanup:
	if (target != source) {
		VOP_UNLOCK(target);
		VOP_UNLOCK(source);
	} else {
		VOP_UNLOCK(target);
	}

	return error;
}

static int tmpfs_symlink(vnode_t *node, char *name, vattr_t *attr, char *path, cred_t *cred) {
	if (node->type != V_TYPE_DIR)
		return ENOTDIR;

	char *pathbuf = alloc(strlen(path) + 1);
	if (pathbuf == NULL)
		return ENOMEM;

	strcpy(pathbuf, path);

	vnode_t *linknode = NULL;
	int err = tmpfs_create(node, name, attr, V_TYPE_LINK, &linknode, cred);
	if (err) {
		free(pathbuf);
		return err;
	}

	VOP_LOCK(linknode);
	tmpfsnode_t *linktmpnode = (tmpfsnode_t *)linknode;
	linktmpnode->link = pathbuf;
	VOP_UNLOCK(linknode);
	VOP_RELEASE(linknode);

	return 0;
}

static int tmpfs_readlink(vnode_t *node, char **link, cred_t *cred) {
	if (node->type != V_TYPE_LINK)
		return EINVAL;

	tmpfsnode_t *tmpnode = (tmpfsnode_t *)node;

	VOP_LOCK(node);
	char *ret = alloc(strlen(tmpnode->link) + 1);
	if (ret == NULL) {
		VOP_UNLOCK(node);
		return ENOMEM;
	}

	strcpy(ret, tmpnode->link);
	*link = ret;

	VOP_UNLOCK(node);
	return 0;
}

#define MMAPTMPFLAGS (ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_NOEXEC | ARCH_MMU_FLAGS_WRITE)

static int tmpfs_mmap(vnode_t *node, void *addr, uintmax_t offset, int flags, cred_t *cred) {
	__assert(!"unreachable");
}

static int tmpfs_munmap(vnode_t *node, void *addr, uintmax_t offset, int flags, cred_t *cred) {
	__assert(!"unreachable");
}


static int tmpfs_getdents(vnode_t *node, dent_t *buffer, size_t count, uintmax_t offset, size_t *readcount) {
	if (node->type != V_TYPE_DIR)
		return ENOTDIR;

	VOP_LOCK(node);

	tmpfsnode_t *tmpnode = (tmpfsnode_t *)node;

	*readcount = 0;
	size_t current = 0;

	HASHTABLE_FOREACH(&tmpnode->children) {
		if (current < offset) {
			++current;
			continue;
		}

		if (*readcount == count)
			break;

		dent_t *ent = &buffer[*readcount];
		tmpfsnode_t *itmpnode = entry->value;

		ent->d_ino = itmpnode->attr.inode;
		ent->d_off = offset;
		ent->d_reclen = sizeof(dent_t);
		ent->d_type = vfs_getposixtype(itmpnode->vnode.type);
		memcpy(ent->d_name, entry->key, entry->keysize);

		*readcount += 1;
	}

	VOP_UNLOCK(node);
	return 0;
}

static int tmpfs_inactive(vnode_t *node) {
	if (node->type == V_TYPE_REGULAR) {
		vmmcache_truncate(node, 0);
	}
	freenode((tmpfsnode_t *)node);
	return 0;
}

static int tmpfs_resize(vnode_t *node, size_t size, cred_t *) {
	VOP_LOCK(node);

	tmpfsnode_t *tmpfsnode = (tmpfsnode_t *)node;
	if (size != tmpfsnode->attr.size && tmpfsnode->vnode.type == V_TYPE_REGULAR) {
		if (tmpfsnode->attr.size > size)
			vmmcache_truncate(node, size);

		tmpfsnode->attr.size = size;
	}

	VOP_UNLOCK(node);
	return 0;
}

static int tmpfs_getpage(vnode_t *node, uintmax_t offset, struct page_t *page) {
	int error = 0;

	VOP_LOCK(node);
	tmpfsnode_t *tmpfsnode = (tmpfsnode_t *)node;
	if (offset >= tmpfsnode->attr.size)
		error = ENXIO;

	VOP_UNLOCK(node);
	if (error)
		return error;

	// since tmpfs files now store their data on the vmmcache,
	// all getpage will do will be set the page to 0 and pin it in memory
	void *phy = pmm_getpageaddress(page);
	void *phyhhdm = MAKE_HHDM(phy);

	pmm_hold(phy);
	page->flags |= PAGE_FLAGS_PINNED;
	memset(phyhhdm, 0, PAGE_SIZE);
	return 0;
}

static int tmpfs_putpage(vnode_t *node, uintmax_t offset, struct page_t *page) {
	// putpage is a no-op on tmpfs
	return 0;
}

static int tmpfs_sync(vnode_t *node) {
	// sync is a no-op on tmpfs
	return 0;
}

static vfsops_t vfsops = {
	.mount = tmpfs_mount,
	.root = tmpfs_root
};

static vops_t vnops = {
	.create = tmpfs_create,
	.open = tmpfs_open,
	.close = tmpfs_close,
	.getattr = tmpfs_getattr,
	.setattr = tmpfs_setattr,
	.lookup = tmpfs_lookup,
	.poll = vfs_pollstub,
	.read = tmpfs_read,
	.write = tmpfs_write,
	.access = tmpfs_access,
	.unlink = tmpfs_unlink,
	.link = tmpfs_link,
	.symlink = tmpfs_symlink,
	.readlink = tmpfs_readlink,
	.inactive = tmpfs_inactive,
	.mmap = tmpfs_mmap,
	.munmap = tmpfs_munmap,
	.getdents = tmpfs_getdents,
	.resize = tmpfs_resize,
	.rename = tmpfs_rename,
	.getpage = tmpfs_getpage,
	.putpage = tmpfs_putpage,
	.sync = tmpfs_sync
};

static tmpfsnode_t *newnode(vfs_t *vfs, int type) {
	tmpfsnode_t *node = slab_allocate(nodecache);
	if (node == NULL)
		return NULL;

	memset(node, 0, sizeof(tmpfsnode_t));

	if (type == V_TYPE_DIR) {
		if (hashtable_init(&node->children, 32)) {
			slab_free(nodecache, node);
			return NULL;
		}

		if (hashtable_set(&node->children, node, ".", 1, true)) {
			hashtable_destroy(&node->children);
			slab_free(nodecache, node);
			return NULL;
		}
	}

	node->attr.type = type;
	node->attr.inode = ++((tmpfs_t *)vfs)->inodenumber;
	VOP_INIT(&node->vnode, &vnops, 0, type, vfs);
	return node;
}

static void freenode(tmpfsnode_t *node) {
	__assert(node->vnode.refcount == 0);
	switch (node->vnode.type) {
		case V_TYPE_LINK:
			if (node->link)
				free(node->link);
			break;
		case V_TYPE_DIR:
			__assert(node->children.entrycount == 2); // assert that it only has the dot entries
			// remove refcount of the .. dir
			void *r = NULL;
			__assert(hashtable_get(&node->children, &r, "..", 2) == 0);
			vnode_t *pnode = r;
			VOP_RELEASE(pnode);
			__assert(hashtable_destroy(&node->children) == 0);
			break;
		default:
			break;
	}
	slab_free(nodecache, node);
}

void tmpfs_init() {
	__assert(vfs_register(&vfsops, "tmpfs") == 0);
	nodecache = slab_newcache(sizeof(tmpfsnode_t), 0, NULL, NULL);
	__assert(nodecache);
}
