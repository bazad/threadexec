#ifndef THREAD_CALL__X86_64__THREAD_CALL_X86_64_H_
#define THREAD_CALL__X86_64__THREAD_CALL_X86_64_H_

#include "thread_call.h"

/*
 * thread_call_stack_x86_64
 *
 * Description:
 * 	The thread_call_stack implementation for x86-64.
 *
 * Notes:
 * 	There isn't a plain thread_call implementation for x86-64 because there is no way on x86
 * 	architectures to control what code runs after a function call when you can only set the
 * 	values of the registers. You need some way of also controlling the stack, or else the
 * 	function will return to who-knows-where.
 *
 * 	Theoretically, as long as you can get your desired stack contents at some known location in
 * 	the target address space, you could perform your function call, e.g. by invoking memcpy()
 * 	to copy the data onto the stack and then memcpy's return would execute the target function.
 *
 * 	However, this limitation isn't really an issue on x86_64 because macOS systems do not
 * 	restrict the use of task ports, meaning we aren't initially stuck in the registers-only
 * 	world like we are on iOS.
 */
bool thread_call_stack_x86_64(thread_act_t thread,
		void *local_stack_base, word_t remote_stack_base, size_t stack_size,
		void *result, size_t result_size,
		word_t function, unsigned argument_count,
		const struct threadexec_call_argument *arguments);

#endif
