#include "thread_api/tx_init_thread.h"

#if TX_HAVE_THREAD_API

#include "tx_call.h"
#include "tx_log.h"
#include "tx_prototypes.h"
#include "tx_utils.h"
#include "thread_api/tx_stage0_mach_ports.h"
#include "thread_api/tx_stage1_shared_memory.h"

#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

// _pthread_set_self() initializes thread-local storage.
extern void _pthread_set_self(pthread_t);

// Perform straightforward initialization using a supplied thread port.
static bool
init_with_thread(threadexec_t threadexec) {
	DEBUG_TRACE(1, "Using thread 0x%x", threadexec->thread);
	assert(threadexec->thread != MACH_PORT_NULL);
	// Set up Mach ports to send messages between this task and the remote thread.
	bool ok = tx_stage0_init_mach_ports(threadexec);
	if (!ok) {
		goto fail;
	}
	// Set up the shared memory region.
	ok = tx_stage1_init_shared_memory(threadexec);
	if (!ok) {
		goto fail;
	}
	// Return success.
	return true;
fail:
	tx_deinit_with_thread_api(threadexec);
	return false;
}

// Pick a thread in the task to hijack.
static thread_t pick_hijack_thread(task_t task) {
	thread_t hijack = MACH_PORT_NULL;
	// Get all the threads in the task.
	thread_act_array_t threads;
	mach_msg_type_number_t thread_count;
	kern_return_t kr = task_threads(task, &threads, &thread_count);
	if (kr != KERN_SUCCESS) {
		ERROR_CALL(task_threads, "%u", kr);
		goto fail_0;
	}
	if (thread_count == 0) {
		ERROR("No threads in task 0x%x", task);
		goto fail_1;
	}
	// Find a candidate thread.
	thread_t thread = MACH_PORT_NULL;
	for (long i = thread_count - 1; thread == MACH_PORT_NULL && i >= 0; i--) {
		int suspend_count = thread_get_suspend_count(threads[i]);
		if (suspend_count == 0) {
			thread = threads[i];
			break;
		}
	}
	if (thread == MACH_PORT_NULL) {
		ERROR("No available candidate threads to hijack");
		goto fail_1;
	}
	// Success!
	hijack = thread;
	// Deallocate the thread ports and array.
fail_1:
	for (size_t i = 0; i < thread_count; i++) {
		DEBUG_TRACE(2, "Task 0x%x: thread 0x%x", task, threads[i]);
		if (threads[i] != hijack) {
			mach_port_deallocate(mach_task_self(), threads[i]);
		}
	}
	mach_vm_deallocate(mach_task_self(), (mach_vm_address_t) threads,
			thread_count * sizeof(*threads));
fail_0:
	return hijack;
}

// Hijack and use an existing thread. This is only safe if we'll kill the task anyway, since the
// thread will be totally consumed.
static bool
init_by_hijacking_thread(threadexec_t threadexec) {
	DEBUG_TRACE(1, "Performing thread hijacking");
	assert(threadexec->flags & TX_KILL_TASK);
	assert(threadexec->thread == MACH_PORT_NULL);
	assert((threadexec->flags & (TX_SUSPEND | TX_PRESERVE | TX_RESUME | TX_KILL_THREAD)) == 0);
	// First pick a thread to hijack. The thread is not suspended.
	thread_t hijack = pick_hijack_thread(threadexec->task);
	if (hijack == MACH_PORT_NULL) {
		ERROR("Could not hijack a thread in task 0x%x", threadexec->task);
		return false;
	}
	// Now initialize with that.
	threadexec->flags |= TX_SUSPEND;
	threadexec->thread = hijack;
	return tx_init_internal(threadexec);
}

// When we don't have a thread, we will perform thread hijacking to create one. First, we will
// choose a preexisting thread in the task to hijack, and create a threadexec with that using
// TX_PRESERVE semantics. Then we will use that thread to create a new thread. Once we have the new
// thread, we will replace the original thread in the threadexec struct with the new thread.
static bool
init_without_thread(threadexec_t threadexec) {
	DEBUG_TRACE(1, "Performing temporary thread hijacking");
	WARNING("NOT IMPLEMENTED");//TODO: This implementation is broken and I haven't debugged it.
	assert(threadexec->thread == MACH_PORT_NULL);
	assert((threadexec->flags & (TX_SUSPEND | TX_PRESERVE | TX_RESUME | TX_KILL_THREAD)) == 0);
	// First pick a thread to hijack. The thread is not suspended.
	thread_t hijack = pick_hijack_thread(threadexec->task);
	if (hijack == MACH_PORT_NULL) {
		ERROR("Could not hijack a thread in task 0x%x", threadexec->task);
		goto fail_0;
	}
	// Now set the TX_SUSPEND, TX_PRESERVE, and TX_RESUME flags and initialize. This will hit
	// init_with_thread(). If this fails then tx_deinit_with_thread_api() and
	// tx_preserve_restore() will be called, but the ports themselves will not be deallocated.
	// This means the only cleanup we need to do is deallocate the hijack thread.
	threadexec->thread = hijack;
	threadexec->flags |= TX_SUSPEND | TX_PRESERVE | TX_RESUME;
	bool ok = tx_init_internal(threadexec);
	if (!ok) {
		ERROR("Thread hijacking failed");
		goto fail_1;
	}
	DEBUG_TRACE(1, "Successfully initialized with existing thread 0x%x", hijack);
	// Alright, we successfully initialized! Now use the working threadexec to create a new
	// thread. We'll give ourselves 4 seconds to find it before it exits on its own.
	int ret;
	pthread_t pthread_r;
	ok = threadexec_call_cv(threadexec, &ret, sizeof(ret),
			pthread_create_suspended_np, 4,
			TX_CARG_PTR_LITERAL_OUT(pthread_t *,       &pthread_r),
			TX_CARG_LITERAL        (pthread_attr_t *,  NULL      ),
			TX_CARG_LITERAL        (void *(*)(void *), abort     ),
			TX_CARG_LITERAL        (void *,            0         ));
	if (!ok) {
		ERROR_REMOTE_CALL(pthread_create);
		goto fail_2;
	}
	if (ret != 0) {
		ERROR_REMOTE_CALL_FAIL(pthread_create, "%u", ret);
		goto fail_2;
	}
	// Detach the thread.
	threadexec_call_cv(threadexec, NULL, 0,
			pthread_detach, 1,
			TX_CARG_LITERAL(pthread_t, pthread_r));
	// Get the Mach port for that thread.
	mach_port_t thread_r;
	ok = threadexec_call_cv(threadexec, &thread_r, sizeof(thread_r),
			pthread_mach_thread_np, 1,
			TX_CARG_LITERAL(pthread_t, pthread_r));
	if (!ok) {
		ERROR_REMOTE_CALL(pthread_mach_thread_np);
		goto fail_2;
	}
	DEBUG_TRACE(2, "Created remote thread 0x%x", thread_r);
	// Copy the thread port into our IPC space.
	mach_port_t thread;
	ok = threadexec_mach_port_extract(threadexec, thread_r, &thread, MACH_MSG_TYPE_COPY_SEND);
	if (!ok) {
		ERROR("Could not copy remote thread port");
		goto fail_2;
	}
	DEBUG_TRACE(2, "Got local port to created thread: 0x%x", thread);
	// Restore the state of the original thread and resume it. The threadexec object cannot be
	// used for calls after this operation!
	tx_preserve_restore(threadexec);
	thread_resume_check(hijack);
	mach_port_deallocate(mach_task_self(), hijack);
	threadexec->flags &= ~(TX_SUSPEND | TX_PRESERVE | TX_RESUME);
	// Replace the original thread with the new one, once again making the threadexec valid for
	// calls. We will kill the thread in threadexec_deinit().
	threadexec->thread = thread;
	threadexec->thread_remote = thread_r;
	threadexec->flags |= TX_KILL_THREAD; // TODO: Maybe a call to pthread_exit() is better.
	// Now set the pthread context for the thread.
	ok = threadexec_call_cv(threadexec, NULL, 0,
			_pthread_set_self, 1,
			TX_CARG_LITERAL(pthread_t, pthread_r));
	if (!ok) {
		ERROR_REMOTE_CALL(_pthread_set_self);
		goto fail_2;
	}
	// Success!
	return true;
fail_2:
	tx_deinit_with_thread_api(threadexec);
fail_1:
	mach_port_deallocate(mach_task_self(), hijack);
	threadexec->thread = MACH_PORT_NULL;
	threadexec->flags &= ~(TX_SUSPEND | TX_PRESERVE | TX_RESUME);
fail_0:
	return false;
}

bool
tx_init_with_thread_api(threadexec_t threadexec) {
	DEBUG_TRACE(1, "Using thread API");
	// We use different initialization strategies for when we do and don't have a thread port.
	if (threadexec->thread != MACH_PORT_NULL) {
		return init_with_thread(threadexec);
	} else if (threadexec->flags & TX_KILL_TASK) {
		return init_by_hijacking_thread(threadexec);
	} else {
		return init_without_thread(threadexec);
	}
}

void
tx_deinit_with_thread_api(threadexec_t threadexec) {
	DEBUG_TRACE(2, "%s", __func__);
	if (threadexec->shmem_size) {
		// Don't bother deallocating the remote memory if we're killing the task.
		if (threadexec->shmem_remote != 0 && (threadexec->flags & TX_KILL_TASK) == 0) {
			DEBUG_TRACE(2, "Deallocating remote shared memory");
			WARNING("NOT IMPLEMENTED");//TODO
			// TODO: Here's a problem: We can't deallocate the memory that is our
			// stack or we'll crash! Therefore we need to save the old SP before we set
			// it (implicitly) with a call to thread_call_stack().
			word_t mvd_args[3] = {
				threadexec->task_remote, threadexec->shmem_remote,
				threadexec->shmem_size
			};
			tx_call_regs(threadexec, NULL, 0,
					(word_t) mach_vm_deallocate, 3, mvd_args);
			threadexec->shmem_remote = 0;
		}
		if (threadexec->shmem != NULL) {
			DEBUG_TRACE(2, "Deallocating local shared memory");
			mach_vm_deallocate(mach_task_self(), (mach_vm_address_t) threadexec->shmem,
					threadexec->shmem_size);
			threadexec->shmem = NULL;
		}
		threadexec->shmem_size = 0;
	}
	// TODO: Destroy these ports on the remote end.
	DEBUG_TRACE(2, "Destroying local Mach ports");
	if (threadexec->local_port != MACH_PORT_NULL) {
		mach_port_destroy(mach_task_self(), threadexec->local_port);
		threadexec->local_port = MACH_PORT_NULL;
	}
	if (threadexec->remote_port != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), threadexec->remote_port);
		threadexec->remote_port = MACH_PORT_NULL;
	}
}

#endif // TX_HAVE_THREAD_API
