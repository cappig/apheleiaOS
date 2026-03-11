#pragma once

#include <base/types.h>
#include <gui/ws.h>
#include <poll.h>
#include <sys/vfs.h>

bool ws_init(void);

ssize_t ws_ctl_ioctl(vfs_node_t *node, u64 request, void *args);
short ws_ctl_poll(vfs_node_t *node, short events, u32 flags);

ssize_t
ws_mgr_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags);
short ws_mgr_poll(vfs_node_t *node, short events, u32 flags);

ssize_t ws_fb_read(u32 id, void *buf, size_t offset, size_t len, u32 flags);
ssize_t
ws_fb_write(u32 id, const void *buf, size_t offset, size_t len, u32 flags);
short ws_fb_poll(u32 id, short events, u32 flags);

ssize_t ws_ev_read(u32 id, void *buf, size_t offset, size_t len, u32 flags);
short ws_ev_poll(u32 id, short events, u32 flags);

bool ws_node_read(
	vfs_node_t *node,
	pid_t caller_pid,
	void *buf,
	size_t offset,
	size_t len,
	u32 flags,
	ssize_t *result_out
);
bool ws_node_write(
	vfs_node_t *node,
	pid_t caller_pid,
	const void *buf,
	size_t offset,
	size_t len,
	u32 flags,
	ssize_t *result_out
);
bool ws_node_poll(
	vfs_node_t *node,
	pid_t caller_pid,
	short events,
	u32 flags,
	short *result_out
);
bool ws_node_ioctl(
	vfs_node_t *node,
	pid_t caller_pid,
	u64 request,
	void *args,
	ssize_t *result_out
);

void ws_notify_screen_active(void);
