#include "tx_internal.h"

#include "tx_log.h"

#include <unistd.h>

// Prototypes for working with fileports.
extern int fileport_makeport(int fd, mach_port_t *port);
extern int fileport_makefd(mach_port_t port);

bool
threadexec_file_insert(threadexec_t threadexec, int local_fd, int *remote_fd) {
	ERROR("NOT IMPLEMENTED");
	return false;
}

bool
threadexec_file_extract(threadexec_t threadexec, int remote_fd, int *local_fd) {
	// Create a fileport in the remote task representing the file descriptor.
	mach_port_t fileport_r;
	int ret;
	bool ok = threadexec_call_cv(threadexec, &ret, sizeof(ret),
			fileport_makeport, 2,
			TX_CARG_LITERAL(int, remote_fd),
			TX_CARG_PTR_LITERAL_OUT(mach_port_t *, &fileport_r));
	if (!ok) {
		ERROR_REMOTE_CALL(fileport_makeport);
		return false;
	}
	if (ret != 0) {
		ERROR_REMOTE_CALL_FAIL(fileport_makeport, "%u", ret);
		return false;
	}
	// Transfer the fileport (which is a send right) to us.
	mach_port_t fileport;
	ok = threadexec_mach_port_extract(threadexec, fileport_r, &fileport,
			MACH_MSG_TYPE_MOVE_SEND);
	if (!ok) {
		ERROR("Could not move fileport to local task");
		threadexec_mach_port_deallocate(threadexec, fileport_r);
		return false;
	}
	// Create a file descriptor from the fileport.
	int fd = fileport_makefd(fileport);
	mach_port_deallocate(mach_task_self(), fileport);
	if (fd < 0) {
		ERROR("Could not create file descriptor from fileport");
		return false;
	}
	// Success!
	*local_fd = fd;
	return true;
}

bool
threadexec_file_open(threadexec_t threadexec, const char *path, int oflags, mode_t mode,
		int *remote_fd, int *local_fd) {
	// First open the file in the threadexec.
	int fd_r, fd_l;
	bool ok = threadexec_call_cv(threadexec, &fd_r, sizeof(fd_r),
			open, 3,
			TX_CARG_CSTRING(const char *, path),
			TX_CARG_LITERAL(int, oflags),
			TX_CARG_LITERAL(mode_t, mode));
	if (!ok) {
		ERROR_REMOTE_CALL(open);
		goto fail_0;
	}
	// If the open failed, return that.
	if (fd_r < 0) {
		fd_l = fd_r;
		goto return_fds;
	}
	// Only copy the file over if we want the local file descriptor.
	if (local_fd != NULL) {
		ok = threadexec_file_extract(threadexec, fd_r, &fd_l);
		if (!ok) {
			goto fail_1;
		}
	}
	// Return the fds to our caller.
return_fds:
	if (remote_fd != NULL) {
		*remote_fd = fd_r;
	}
	if (local_fd != NULL) {
		*local_fd = fd_l;
	}
	// If we don't want the remote file or if we encountered an error, close the remote file.
	if (remote_fd == NULL) {
fail_1:
		threadexec_call_cv(threadexec, NULL, 0,
				close, 1,
				TX_CARG_LITERAL(int, fd_r));
	}
fail_0:
	return ok;
}
