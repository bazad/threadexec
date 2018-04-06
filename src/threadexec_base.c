#include "tx_internal.h"

mach_port_t
threadexec_task(threadexec_t threadexec) {
	return threadexec->task;
}

mach_port_t
threadexec_task_remote(threadexec_t threadexec) {
	return threadexec->task_remote;
}

mach_port_t
threadexec_thread(threadexec_t threadexec) {
	return threadexec->thread;
}

mach_port_t
threadexec_thread_remote(threadexec_t threadexec) {
	return threadexec->thread_remote;
}
