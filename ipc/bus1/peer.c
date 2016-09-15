/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pid_namespace.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <uapi/linux/bus1.h>
#include "main.h"
#include "message.h"
#include "peer.h"
#include "transaction.h"
#include "user.h"
#include "util.h"
#include "util/active.h"
#include "util/pool.h"
#include "util/queue.h"

static void bus1_peer_info_reset(struct bus1_peer_info *peer_info, bool final)
{
	struct bus1_queue_node *node;
	struct bus1_message *message;
	size_t n_slices, n_handles;
	LIST_HEAD(list);

	bus1_handle_flush_all(peer_info, &n_handles, final);
	bus1_queue_flush(&peer_info->queue, &list);

	mutex_lock(&peer_info->lock);
	if (peer_info->seed && final) {
		list_add(&peer_info->seed->qnode.link, &list);
		peer_info->seed = NULL;
	}
	bus1_pool_flush(&peer_info->pool, &n_slices);
	mutex_unlock(&peer_info->lock);

	atomic_add(n_slices, &peer_info->user->n_slices);
	atomic_add(n_handles, &peer_info->user->n_handles);

	while ((node = list_first_entry_or_null(&list, struct bus1_queue_node,
						link))) {
		list_del(&node->link);
		RB_CLEAR_NODE(&node->rb); /* reset the link/rb union */

		switch (bus1_queue_node_get_type(node)) {
		case BUS1_QUEUE_NODE_MESSAGE:
			message = bus1_message_from_node(node);
			bus1_message_unpin(message, peer_info);
			bus1_message_unref(message);
			break;
		case BUS1_QUEUE_NODE_HANDLE_DESTRUCTION:
		case BUS1_QUEUE_NODE_HANDLE_RELEASE:
			bus1_handle_unref_queued(node);
			break;
		default:
			WARN(1, "Invalid queue-node type");
			break;
		}
	}
}

static struct bus1_peer_info *
bus1_peer_info_free(struct bus1_peer_info *peer_info)
{
	if (!peer_info)
		return NULL;

	bus1_peer_info_reset(peer_info, true);

	bus1_queue_destroy(&peer_info->queue);
	bus1_pool_destroy(&peer_info->pool);
	bus1_user_quota_destroy(&peer_info->quota);

	peer_info->user = bus1_user_unref(peer_info->user);
	put_pid_ns(peer_info->pid_ns);
	put_cred(peer_info->cred);

	WARN_ON(!RB_EMPTY_ROOT(&peer_info->map_handles_by_node));
	WARN_ON(!RB_EMPTY_ROOT(&peer_info->map_handles_by_id));
	WARN_ON(peer_info->seed);

	kfree_rcu(peer_info, rcu);

	return NULL;
}

static struct bus1_peer_info *bus1_peer_info_new(wait_queue_head_t *waitq)
{
	struct bus1_peer_info *peer_info;
	int r;

	peer_info = kmalloc(sizeof(*peer_info), GFP_KERNEL);
	if (!peer_info)
		return ERR_PTR(-ENOMEM);

	mutex_init(&peer_info->lock);
	peer_info->cred = get_cred(current_cred());
	peer_info->pid_ns = get_pid_ns(task_active_pid_ns(current));
	peer_info->user = NULL;
	bus1_user_quota_init(&peer_info->quota);
	peer_info->pool = BUS1_POOL_NULL;
	bus1_queue_init(&peer_info->queue, waitq);
	peer_info->seed = NULL;
	peer_info->map_handles_by_id = RB_ROOT;
	peer_info->map_handles_by_node = RB_ROOT;
	seqcount_init(&peer_info->seqcount);
	peer_info->handle_ids = 0;

	peer_info->user = bus1_user_ref_by_uid(peer_info->cred->uid);
	if (IS_ERR(peer_info->user)) {
		r = PTR_ERR(peer_info->user);
		peer_info->user = NULL;
		goto error;
	}

	r = bus1_pool_create(&peer_info->pool);
	if (r < 0)
		goto error;

	return peer_info;

error:
	bus1_peer_info_free(peer_info);
	return ERR_PTR(r);
}

/**
 * bus1_peer_new() - allocate new peer
 *
 * Allocate a new peer. The peer is *not* activated, nor linked into any
 * context. The caller owns the only pointer to the new peer.
 *
 * Return: Pointer to peer, ERR_PTR on failure.
 */
struct bus1_peer *bus1_peer_new(void)
{
	static atomic64_t bus1_peer_ids = ATOMIC64_INIT(0);
	struct bus1_peer *peer;

	peer = kmalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	init_waitqueue_head(&peer->waitq);
	bus1_active_init(&peer->active);
	rcu_assign_pointer(peer->info, NULL);
	peer->id = atomic64_inc_return(&bus1_peer_ids);
	peer->debugdir = NULL;

	if (!IS_ERR_OR_NULL(bus1_debugdir)) {
		char idstr[22];

		snprintf(idstr, sizeof(idstr), "peer-%llx", peer->id);

		peer->debugdir = debugfs_create_dir(idstr, bus1_debugdir);
		if (!peer->debugdir) {
			pr_err("cannot create debugfs dir for peer %llx\n",
			       peer->id);
		} else if (!IS_ERR_OR_NULL(peer->debugdir)) {
			bus1_debugfs_create_atomic_x("active", S_IRUGO,
						     peer->debugdir,
						     &peer->active.count);
		}
	}

	return peer;
}

/**
 * bus1_peer_free() - destroy peer
 * @peer:	peer to destroy, or NULL
 *
 * Destroy a peer object that was previously allocated via bus1_peer_new(). If
 * the peer object was activated, then the caller must make sure it was
 * properly torn down before destroying it.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_peer *bus1_peer_free(struct bus1_peer *peer)
{
	if (!peer)
		return NULL;

	WARN_ON(rcu_access_pointer(peer->info));
	debugfs_remove_recursive(peer->debugdir);
	bus1_active_destroy(&peer->active);
	kfree_rcu(peer, rcu);

	return NULL;
}

/**
 * bus1_peer_connect() - connect peer
 * @peer:		peer to operate on
 *
 * This connects a peer. It first creates the linked peer_info object and then
 * marks the peer as active.
 *
 * The caller must make sure this function is called only once. Furthermore,
 * this call must not race bus1_peer_disconnect(). Better call it on
 * initialization.
 *
 * Return: 0 on success, negative error code if already torn down.
 */
int bus1_peer_connect(struct bus1_peer *peer)
{
	struct bus1_peer_info *peer_info;

	if (WARN_ON(!bus1_active_is_new(&peer->active)))
		return -ENOTRECOVERABLE;

	peer_info = bus1_peer_info_new(&peer->waitq);
	if (IS_ERR(peer_info))
		return PTR_ERR(peer_info);

	rcu_assign_pointer(peer->info, peer_info);
	bus1_active_activate(&peer->active);

	return 0;
}

static void bus1_peer_cleanup(struct bus1_active *active, void *userdata)
{
	struct bus1_peer *peer = container_of(active, struct bus1_peer, active);
	struct bus1_peer_info *peer_info;

	/*
	 * bus1_active guarantees that this function is called exactly once,
	 * and that it cannot race bus1_peer_connect(). It is therefore safe
	 * to access the info object without any locking.
	 */
	peer_info = rcu_dereference_raw(peer->info);
	rcu_assign_pointer(peer->info, NULL);

	bus1_peer_info_free(peer_info);
}

/**
 * bus1_peer_disconnect() - disconnect peer
 * @peer:		peer to operate on
 *
 * This tears down a peer synchronously. It first marks the peer as deactivated,
 * waits for all outstanding operations to finish, and eventually releases the
 * linked peer_info object.
 *
 * It is perfectly safe to call this function multiple times, even in parallel.
 * It is guaranteed to block *until* the peer is fully torn down, regardless
 * whether this was the call to tear it down, or not.
 *
 * Return: 0 on success, negative error code if already torn down.
 */
int bus1_peer_disconnect(struct bus1_peer *peer)
{
	bus1_active_deactivate(&peer->active);
	bus1_active_drain(&peer->active, &peer->waitq);

	if (!bus1_active_cleanup(&peer->active, &peer->waitq,
				 bus1_peer_cleanup, NULL))
		return -ESHUTDOWN;

	return 0;
}

static int bus1_peer_ioctl_query(struct bus1_peer *peer, unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	struct bus1_cmd_peer_reset __user *uparam = (void __user *)arg;
	struct bus1_cmd_peer_reset param;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_PEER_RESET) != sizeof(param));

	if (copy_from_user(&param, uparam, sizeof(param)))
		return -EFAULT;
	if (unlikely(param.flags))
		return -EINVAL;

	param.peer_flags = peer_info->flags & BUS1_PEER_FLAG_WANT_SECCTX;

	/* XXX: add support for peer limits */
	param.max_slices = -1;
	param.max_handles = -1;
	param.max_inflight_bytes = -1;
	param.max_inflight_fds = -1;

	return put_user(param.peer_flags, &uparam->peer_flags) ? -EFAULT : 0;
}

static int bus1_peer_ioctl_reset(struct bus1_peer *peer, unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	bool destroy_nodes, protect_persistent, release_handles;
	struct bus1_cmd_peer_reset __user *uparam = (void __user *)arg;
	struct bus1_cmd_peer_reset param;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_PEER_RESET) != sizeof(param));

	if (copy_from_user(&param, uparam, sizeof(param)))
		return -EFAULT;
	if (unlikely(param.flags & ~(BUS1_PEER_RESET_FLAG_DESTROY_NODES |
				     BUS1_PEER_RESET_FLAG_PROTECT_PERSISTENT |
				     BUS1_PEER_RESET_FLAG_RELEASE_HANDLES)))
		return -EINVAL;

	/* XXX: add support for peer limits */
	if (unlikely(param.max_slices != -1 ||
		     param.max_handles != -1 ||
		     param.max_inflight_bytes != -1 ||
		     param.max_inflight_fds != -1))
		return -EINVAL;

	destroy_nodes = param.flags & BUS1_PEER_RESET_FLAG_DESTROY_NODES;
	protect_persistent = param.flags &
			     BUS1_PEER_RESET_FLAG_PROTECT_PERSISTENT;
	release_handles = param.flags & BUS1_PEER_RESET_FLAG_RELEASE_HANDLES;

	/* PROTECT_PERSISTENT requires DESTROY_NODES */
	if (unlikely(protect_persistent && !destroy_nodes))
		return -EINVAL;
	/* DESTROY_NODES and RELEASE_HANDLES must be combined so far */
	if (unlikely(destroy_nodes != release_handles))
		return -EINVAL;
	/* refuse invalid flags, unless cleared to -1 */
	if (unlikely(param.peer_flags != -1 &&
		     (param.peer_flags & ~BUS1_PEER_FLAG_WANT_SECCTX)))
		return -EINVAL;

	if (destroy_nodes || release_handles)
		bus1_peer_info_reset(peer_info, !protect_persistent);

	if (param.peer_flags != -1) {
		mutex_lock(&peer_info->lock);
		peer_info->flags = param.peer_flags |
				   (peer_info->flags &
				    ~BUS1_PEER_FLAG_WANT_SECCTX);
		mutex_unlock(&peer_info->lock);
	}

	return 0;
}

static int bus1_peer_ioctl_handle_release(struct bus1_peer *peer,
					  unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	size_t n_handles = 0;
	u64 id;
	int r;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_HANDLE_RELEASE) != sizeof(id));

	if (get_user(id, (const u64 __user *)arg))
		return -EFAULT;

	r = bus1_handle_release_by_id(peer, id, &n_handles);
	atomic_add(n_handles, &peer_info->user->n_handles);

	return r;
}

static int bus1_peer_ioctl_handle_transfer(struct bus1_peer *src,
					   unsigned long arg)
{
	struct bus1_cmd_handle_transfer __user *uparam = (void __user *) arg;
	struct bus1_cmd_handle_transfer param;
	struct bus1_peer *dst;
	struct fd dst_f;
	int r = 0;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_HANDLE_TRANSFER) != sizeof(param));

	if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
		return -EFAULT;
	if (unlikely(param.flags))
		return -EINVAL;

	if (param.dst_fd == -1) {
		dst = bus1_peer_acquire(src);
	} else {
		dst_f = fdget(param.dst_fd);
		if (!dst_f.file)
			return -EBADF;
		if (dst_f.file->f_op != &bus1_fops) {
			fdput(dst_f);
			return -EOPNOTSUPP;
		}

		dst = bus1_peer_acquire(dst_f.file->private_data);
		fdput(dst_f);
	}

	if (!dst)
		return -ESHUTDOWN;

	/* pass handle from src to dst, allocating node if necessary */
	r = bus1_handle_pair(src, dst, &param.src_handle, &param.dst_handle);
	if (r < 0)
		goto out;

	if (put_user(param.src_handle, &uparam->src_handle) ||
	    put_user(param.dst_handle, &uparam->dst_handle))
		r = -EFAULT; /* We don't care, keep what we did */

out:
	bus1_peer_release(dst);
	return r;
}

static int bus1_peer_ioctl_node_destroy(struct bus1_peer *peer,
					unsigned long arg)
{
	struct bus1_cmd_node_destroy param;
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	u64 __user *ptr_nodes;
	size_t n_handles = 0;
	unsigned int i;
	int r, res = 0;
	u64 id;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_NODE_DESTROY) != sizeof(param));

	if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
		return -EFAULT;
	if (unlikely(param.flags))
		return -EINVAL;
	if (unlikely(param.ptr_nodes != (u64)(unsigned long)param.ptr_nodes))
		return -EFAULT;

	ptr_nodes = (u64 __user *)(unsigned long)param.ptr_nodes;

	/* XXX: make atomic and disallow partial failures */
	for (i = 0; i < param.n_nodes; ++i) {
		if (get_user(id, ptr_nodes + i)) {
			r = -EFAULT;
		} else {
			/* >= 0 on success, >0 in case @id was modified */
			r = bus1_node_destroy_by_id(peer, &id, &n_handles);
			if (r > 0 && put_user(id, ptr_nodes + i))
				r = -EFAULT;
		}

		if (unlikely(r < 0 && res >= 0))
			res = r;
	}

	atomic_add(n_handles, &peer_info->user->n_handles);

	return res;
}

static int bus1_peer_ioctl_slice_release(struct bus1_peer *peer,
					 unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	size_t n_slices = 0;
	u64 offset;
	int r;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_SLICE_RELEASE) != sizeof(offset));

	if (get_user(offset, (const u64 __user *)arg))
		return -EFAULT;

	mutex_lock(&peer_info->lock);
	r = bus1_pool_release_user(&peer_info->pool, offset, &n_slices);
	mutex_unlock(&peer_info->lock);

	atomic_add(n_slices, &peer_info->user->n_slices);
	return r;
}

static int bus1_peer_ioctl_send(struct bus1_peer *peer, unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	struct bus1_transaction *transaction = NULL;
	/* Use a stack-allocated buffer for the transaction object if it fits */
	u8 buf[512];
	struct bus1_cmd_send param;
	u64 __user *ptr_dest, *ptr_err;
	size_t i;
	int r;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_SEND) != sizeof(param));

	if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
		return -EFAULT;
	if (unlikely(param.flags & ~(BUS1_SEND_FLAG_CONTINUE |
				     BUS1_SEND_FLAG_SEED)))
		return -EINVAL;

	/* check basic limits; avoids integer-overflows later on */
	if (unlikely(param.n_vecs > BUS1_VEC_MAX) ||
	    unlikely(param.n_fds > BUS1_FD_MAX))
		return -EMSGSIZE;

	/* 32bit pointer validity checks */
	if (unlikely(param.ptr_destinations !=
		     (u64)(unsigned long)param.ptr_destinations) ||
	    unlikely(param.ptr_errors !=
		     (u64)(unsigned long)param.ptr_errors) ||
	    unlikely(param.ptr_vecs !=
		     (u64)(unsigned long)param.ptr_vecs) ||
	    unlikely(param.ptr_handles !=
		     (u64)(unsigned long)param.ptr_handles) ||
	    unlikely(param.ptr_fds !=
		     (u64)(unsigned long)param.ptr_fds))
		return -EFAULT;

	ptr_dest = (u64 __user *)(unsigned long)param.ptr_destinations;
	ptr_err = (u64 __user *)(unsigned long)param.ptr_errors;

	transaction = bus1_transaction_new_from_user(buf, sizeof(buf), peer,
						     &param);
	if (IS_ERR(transaction))
		return PTR_ERR(transaction);

	if (param.flags & BUS1_SEND_FLAG_SEED) {
		if (unlikely((param.flags & BUS1_SEND_FLAG_CONTINUE) ||
			     param.n_destinations)) {
			r = -EINVAL;
			goto exit;
		}

		r = bus1_transaction_commit_seed(transaction);
		if (r < 0)
			goto exit;
	} else {
		if (unlikely(param.n_destinations >
			     atomic_read(&peer_info->user->max_handles))) {
			r = -EMSGSIZE;
			goto exit;
		}

		for (i = 0; i < param.n_destinations; ++i) {
			r = bus1_transaction_instantiate_for_id(transaction,
								ptr_dest + i,
								ptr_err ?
								ptr_err + i :
								NULL);
			if (r < 0)
				goto exit;
		}

		r = bus1_transaction_commit(transaction);
		if (r < 0)
			goto exit;
	}

	r = 0;

exit:
	bus1_transaction_free(transaction, buf);
	return r;
}

static int bus1_peer_ioctl_recv(struct bus1_peer *peer, unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	struct bus1_queue_node *node = NULL;
	struct bus1_message *msg = NULL;
	unsigned int type = _BUS1_QUEUE_NODE_N;
	struct bus1_cmd_recv param;
	bool has_continue = false;
	int r;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_RECV) != sizeof(param));

	if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
		return -EFAULT;
	if (unlikely(param.flags & ~(BUS1_RECV_FLAG_PEEK |
				     BUS1_RECV_FLAG_SEED |
				     BUS1_RECV_FLAG_INSTALL_FDS)))
		return -EINVAL;

	/*
	 * Hold the peer lock over peek() -> remove() to make sure we never
	 * dequeue a message twice. We need the peer locked for handle-install
	 * anyway.
	 * We ignore racing RESETs. It does not matter which call drops the
	 * message, since the dequeue is the only visible effect.
	 */
	mutex_lock(&peer_info->lock);

	/*
	 * We need to pin the message while it is still queued, otherwise a
	 * racing RESET might deallocate the resources. In case of seeds the
	 * lock is redundant, but it is no fast-path, anyway.
	 */
	mutex_lock(&peer_info->queue.lock);
	if (!(param.flags & BUS1_RECV_FLAG_SEED))
		node = bus1_queue_peek_locked(&peer_info->queue, &has_continue);
	else if (peer_info->seed)
		node = &bus1_message_ref(peer_info->seed)->qnode;
	if (node) {
		type = bus1_queue_node_get_type(node);
		if (type == BUS1_QUEUE_NODE_MESSAGE)
			msg = bus1_message_pin(bus1_message_from_node(node));
	}
	mutex_unlock(&peer_info->queue.lock);

	/*
	 * Verify the message to return. In case of user messages we need to
	 * install the payload before dequeueing the message. If that fails, we
	 * must skip the dequeue and instead return an error to user-space.
	 */
	if (!node) {
		r = -EAGAIN;
	} else if (!msg) {
		r = 0;
	} else if (param.max_offset < msg->slice->offset + msg->slice->size) {
		r = -ERANGE;
	} else {
		r = bus1_message_install(msg, peer_info, &param);
	}

	if (r >= 0 && !(param.flags & BUS1_RECV_FLAG_PEEK)) {
		/* unpin() cannot be the last, so safe under peer lock */
		if (param.flags & BUS1_RECV_FLAG_SEED) {
			peer_info->seed = bus1_message_unref(msg);
			bus1_message_unpin(msg, peer_info);
		} else if (bus1_queue_remove(&peer_info->queue, node)) {
			bus1_message_unpin(msg, peer_info);
		}
	}

	/*
	 * The message was pinned, installed, and possibly dequeued. We can
	 * unlock the peer now. In case of an error, simply bail out. In case
	 * of success, we can copy the data to user-space. No further
	 * operations required.
	 */
	mutex_unlock(&peer_info->lock);

	if (r < 0)
		return r;

	if (msg) {
		param.msg.type = BUS1_MSG_DATA;
		param.msg.flags = msg->flags;
		param.msg.uid = msg->uid;
		param.msg.gid = msg->gid;
		param.msg.pid = msg->pid;
		param.msg.tid = msg->tid;
		param.msg.offset = msg->slice->offset;
		param.msg.n_bytes = msg->n_bytes;
		param.msg.n_handles = msg->handles.batch.n_entries;
		param.msg.n_fds = msg->n_files;
		param.msg.n_secctx = msg->n_secctx;

		param.msg.destination = msg->destination;

		bus1_message_unpin(msg, peer_info);
		bus1_message_unref(msg);
	} else {
		param.msg.type = (type == BUS1_QUEUE_NODE_HANDLE_DESTRUCTION)
				 ? BUS1_MSG_NODE_DESTROY
				 : BUS1_MSG_NODE_RELEASE;
		param.msg.flags = 0;
		param.msg.uid = -1;
		param.msg.gid = -1;
		param.msg.pid = 0;
		param.msg.tid = 0;
		param.msg.offset = BUS1_OFFSET_INVALID;
		param.msg.n_bytes = 0;
		param.msg.n_handles = 0;
		param.msg.n_fds = 0;
		param.msg.n_secctx = 0;

		param.msg.destination = bus1_handle_unref_queued(node);
	}

	if (has_continue)
		param.msg.flags |= BUS1_MSG_FLAG_CONTINUE;

	return copy_to_user((void __user *)arg,
			    &param, sizeof(param)) ? -EFAULT : 0;
}

/**
 * bus1_peer_ioctl() - handle peer runtime ioctls
 * @peer:		peer to work on
 * @cmd:		ioctl command
 * @arg:		ioctl argument
 *
 * This handles the given ioctl (cmd+arg) on the passed peer. The caller must
 * hold an active reference to @peer.
 *
 * This only handles the runtime ioctls. Setup and teardown must be called
 * directly.
 *
 * Multiple ioctls can be called in parallel just fine. No locking is needed.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_peer_ioctl(struct bus1_peer *peer,
		    unsigned int cmd,
		    unsigned long arg)
{
	lockdep_assert_held(&peer->active);

	switch (cmd) {
	case BUS1_CMD_PEER_QUERY:
		return bus1_peer_ioctl_query(peer, arg);
	case BUS1_CMD_PEER_RESET:
		return bus1_peer_ioctl_reset(peer, arg);
	case BUS1_CMD_HANDLE_RELEASE:
		return bus1_peer_ioctl_handle_release(peer, arg);
	case BUS1_CMD_HANDLE_TRANSFER:
		return bus1_peer_ioctl_handle_transfer(peer, arg);
	case BUS1_CMD_NODE_DESTROY:
		return bus1_peer_ioctl_node_destroy(peer, arg);
	case BUS1_CMD_SLICE_RELEASE:
		return bus1_peer_ioctl_slice_release(peer, arg);
	case BUS1_CMD_SEND:
		return bus1_peer_ioctl_send(peer, arg);
	case BUS1_CMD_RECV:
		return bus1_peer_ioctl_recv(peer, arg);
	}

	return -ENOTTY;
}
