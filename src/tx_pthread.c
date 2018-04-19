#include "tx_pthread.h"

#include "tx_call.h"
#include "tx_internal.h"
#include "tx_log.h"

#include <pthread.h>

// _pthread_set_self() initializes thread-local storage.
extern void _pthread_set_self(pthread_t);

bool
tx_pthread_init_bare_thread(threadexec_t threadexec) {
	bool success = false;
	// Set our pthread context to the main thread's pthread context. This will allow us to use
	// thread-local storage. Note that we may only have register calling at this point.
	word_t _pthread_set_self_args[1] = {
		(word_t) (pthread_t) NULL,
	};
	bool ok = tx_call_regs(threadexec, NULL, 0,
			(word_t) _pthread_set_self, 1, _pthread_set_self_args);
	if (!ok) {
		ERROR_REMOTE_CALL(_pthread_set_self);
		goto fail_0;
	}
	// TODO: Create a new pthread context and switch to that!
	success = true;
fail_0:
	return success;
}
