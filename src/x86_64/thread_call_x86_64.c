#include "x86_64/thread_call_x86_64.h"

#include "tx_log.h"
#include "tx_utils.h"

#include <assert.h>
#include <stdlib.h>

static bool
thread_get_state_x86_64(mach_port_t thread, x86_thread_state64_t *state) {
	mach_msg_type_number_t thread_state_count = x86_THREAD_STATE64_COUNT;
	kern_return_t kr = thread_get_state(thread, x86_THREAD_STATE64,
			(thread_state_t) state, &thread_state_count);
	return (kr == KERN_SUCCESS);
}

static bool
thread_set_state_x86_64(mach_port_t thread, x86_thread_state64_t *state) {
	kern_return_t kr = thread_set_state(thread, x86_THREAD_STATE64,
			(thread_state_t) state, x86_THREAD_STATE64_COUNT);
	return (kr == KERN_SUCCESS);
}

// Find the address of a 'jmp rbx' gadget in the dyld shared cache.
// HACK: This heuristic is terrible.
static uint64_t
find_jmp_rbx() {
	static uint64_t jmp_rbx = 1;
	if (jmp_rbx == 1) {
		uint8_t jmp_rbx_ins[2] = { 0xff, 0xe3 };
		void *start = &malloc;
		if ((void *) &abort < start) {
			start = &abort;
		}
		size_t size = 0x4000 * 128;
		void *found = memmem(start, size, &jmp_rbx_ins, sizeof(jmp_rbx_ins));
		jmp_rbx = (uint64_t) found;
	}
	return jmp_rbx;
}

// Some code common to both thread_call_x86_64 routines.
static bool
set_state_run_thread_wait_and_stop_thread(const char *_func,
		thread_act_t thread, x86_thread_state64_t *state) {
	// Our caller has set up the thread state to have the thread infinite loop on a 'jmp rbx'
	// gadget once the function returns.
	// NOTE: We could also make the thread crash on completion and set ourselves up as the
	// exception handler, which would eliminate the need for the gadget, but this seems
	// simpler.
	uint64_t jmp_rbx = find_jmp_rbx();
	// Set the new state in the thread.
	bool success = thread_set_state_x86_64(thread, state);
	if (!success) {
		ERROR("%s: Failed to set thread state for thread %x", _func, thread);
		return false;
	}
	// Run the thread.
	success = thread_resume_check(thread);
	if (!success) {
		ERROR("%s: Failed to resume thread %x", _func, thread);
		return false;
	}
	// Wait until the thread is in the expected state.
	for (;;) {
		success = thread_get_state_x86_64(thread, state);
		if (!success) {
			// Possibly the thread crashed.
			thread_suspend_check(thread);
			ERROR("%s: Failed to get thread state for thread %x", _func, thread);
			return false;
		}
		if (state->__rip == jmp_rbx && state->__rbx == jmp_rbx) {
			break;
		}
	}
	// Suspend the thread.
	success = thread_suspend_check(thread);
	if (!success) {
		WARNING("%s: Failed to suspend thread %x", _func, thread);
	}
	return true;
}

#define REGISTER_ARGUMENT_COUNT 6

// Try to lay out the arguments in registers and on the stack.
//
// Currently only integral arguments are supported.
//
// We actually have an even easier time of it on x86-64 than on arm64 since stack arguments consume
// space in 8-byte multiples. But we do have to handle promotion of values smaller than 4 bytes.
//
// How do we do that when we don't know whether the type is signed or unsigned? C sign-extension to
// the rescue! The threadexec_call_argument struct stores the value as a word_t, or a uint64_t on
// this platform. Thus, as long as the data was a signed type when the user supplied it to the
// TX_ARG() macro, C performed sign extension on it already. Thus we just need to read out the
// value as a uint32_t and we're all set. :)
//
// Reference:
//   - https://github.com/hjl-tools/x86-psABI/wiki/X86-psABI
static bool
lay_out_arguments(uint64_t *registers, void *stack, size_t stack_size,
		unsigned argument_count, const struct threadexec_call_argument *arguments) {
	size_t i = 0;
	// Register arguments go directly in registers; no translation needed.
	for (; i < argument_count && i < REGISTER_ARGUMENT_COUNT; i++) {
		registers[i] = arguments[i].value;
	}
	// Stack arguments get packed and aligned.
	size_t stack_position = 0;
	for (; i < argument_count; i++) {
		assert(arguments[i].size <= sizeof(uint64_t));
		// Check that the argument fits in the available stack space.
		size_t next_position = stack_position + sizeof(uint64_t);
		if (next_position > stack_size) {
			return false;
		}
		// Add the argument to the stack.
		*(uint64_t *)((uint8_t *)stack + stack_position) = arguments[i].value;
		next_position = stack_position;
	}
	return (i == argument_count);
}

bool
thread_call_stack_x86_64(thread_act_t thread,
		void *local_stack_base, word_t remote_stack_base, size_t stack_size,
		void *result, size_t result_size,
		word_t function, unsigned argument_count,
		const struct threadexec_call_argument *arguments) {
	// Get the jmp rbx gadget we'll need for later.
	uint64_t jmp_rbx = find_jmp_rbx();
	// Process the arguments and lay out the stack. Note that the top of the arguments on the
	// stack must be 16-byte aligned, so stack_args_size must always be a multiple of 16. (We
	// are assuming the top of the stack allocation will always be page-aligned.)
	uint64_t registers[REGISTER_ARGUMENT_COUNT];
	size_t stack_args_size = 32 * sizeof(uint64_t);
	uint8_t *stack = (uint8_t *)local_stack_base - stack_args_size;
	uint64_t remote_stack = remote_stack_base - stack_args_size;
	bool args_ok = lay_out_arguments(registers, stack, stack_args_size,
			argument_count, arguments);
	// If the caller is just asking for whether we can perform this call, tell them.
	if (function == 0) {
		return (jmp_rbx != 0 && args_ok);
	}
	// Now make sure we have the gadget.
	if (jmp_rbx == 0) {
		ERROR("%s: Could not locate 'jmp rbx' gadget!", __func__);
		return false;
	}
	// And now make sure the arguments will work.
	if (!args_ok) {
		ERROR("%s: Unsupported number of arguments: %zu", __func__, argument_count);
		return false;
	}
	// Set the values of the registers to execute our function call. We set registers rdi, ...,
	// r9 to the first 6 arguments, rip to the function to call, and rbx to the 'jmp rbx'
	// gadget so that the thread infinite loops when done.
	x86_thread_state64_t state = {};
	uint64_t *state_argument_registers[REGISTER_ARGUMENT_COUNT] = {
		&state.__rdi, &state.__rsi, &state.__rdx,
		&state.__rcx, &state.__r8,  &state.__r9,
	};
	for (unsigned i = 0; i < REGISTER_ARGUMENT_COUNT; i++) {
		*state_argument_registers[i] = registers[i];
	}
	state.__rip = function;
	state.__rbx = jmp_rbx;
	// Push the return address onto the stack and set rsp to the top of the remote stack.
	remote_stack -= sizeof(uint64_t);
	stack        -= sizeof(uint64_t);
	*(uint64_t *)stack = jmp_rbx;
	state.__rsp = remote_stack;
	// Alright, now do the actual execution.
	bool success = set_state_run_thread_wait_and_stop_thread(__func__, thread, &state);
	if (!success) {
		return false;
	}
	// OK, everything looks good! Let's store the result.
	if (result_size > 0) {
		pack_uint(result, state.__rax, result_size);
	}
	return true;
}
