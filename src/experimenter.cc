/* -*- mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

//#define DEBUGTAG "Experimenter"

#include <sys/syscall.h>

#include "experimenter.h"

#include "debugger_gdb.h"
#include "log.h"
#include "replayer.h"
#include "session.h"
#include "task.h"
#include "util.h"

// The global experiment session, of which there can only be one at a
// time currently.  See long comment at the top of experimenter.h.
static ReplaySession::shr_ptr session;

static void finish_emulated_syscall_with_ret(Task* t, long ret)
{
	Registers r = t->regs();
	r.eax = ret;
	t->set_regs(r);
	t->finish_emulated_syscall();
}

/**
 * Execute the syscall contained in |t|'s current register set.  The
 * return value of the syscall is set for |t|'s registers, to be
 * returned to the tracee task.
 */
static void execute_syscall(Task* t)
{
	t->finish_emulated_syscall();

	struct current_state_buffer state;
	prepare_remote_syscalls(t, &state);
	remote_syscall6(t, &state, state.regs.original_syscallno(),
			state.regs.arg1(), state.regs.arg2(),
			state.regs.arg3(), state.regs.arg4(),
			state.regs.arg5(), state.regs.arg6());
	state.regs.set_syscall_result(t->regs().syscall_result());
	finish_remote_syscalls(t, &state);
}

static void process_syscall(Task* t, int syscallno)
{
	LOG(debug) <<"Processing "<< t->syscallname(syscallno);

	switch (syscallno) {
	// The arm/disarm-desched ioctls are emulated as no-ops.
	// However, because the rr preload library expects these
	// syscalls to succeed and aborts if they don't, we fudge a
	// "0" return value.
	case SYS_ioctl:
		if (!t->is_desched_event_syscall()) {
			break;
		}
		finish_emulated_syscall_with_ret(t, 0);
		return;

	// Writes to stdio fds are emulated in this tracer process.
	case SYS_write: {
		int fd = t->regs().arg1();
		void* bufaddr = (void*)t->regs().arg2();
		size_t num_bytes = t->regs().arg3();
		if (STDOUT_FILENO != fd && STDERR_FILENO != fd) {
			break;
		}
		byte buf[num_bytes];
		ssize_t nread =
			t->read_bytes_fallible(bufaddr, num_bytes, buf);
		if (nread > 0) {
			write(fd, buf, nread);
		}
		finish_emulated_syscall_with_ret(t, nread);
		return;
	}
	// These syscalls are actually executed, based on the register
	// contents already present in the remote task.  To execute a
	// new syscall, simply add it to this list.
	case SYS_mmap2:
	case SYS_munmap:
		return execute_syscall(t);

	}

	// We "implement" unhandled syscalls by simply ignoring them.
	// Tracees enter the syscall through a SYSEMU request, but no
	// emulation or return value munging is done.  To tracees,
	// this will look like an -ENOSYS return from the kernel.
	//
	// TODO: it's not known whether this is sufficient for
	// interesting cases yet.
	fprintf(stderr,
"rr: Warning: Syscall `%s' not handled during experimental session.\n",
		t->syscallname(syscallno));
}

/**
 * Advance execution of |t| according to |req| until either a signal
 * is received (including a SIGTRAP generated by a single-step) or a
 * syscall is made.  Return false when "interrupted" by a signal, true
 * when a syscall is made.
 */
static bool advance(Task* t, const struct dbg_request& req)
{
	assert(!t->child_sig);

	switch (req.type) {
	case DREQ_CONTINUE:
		LOG(debug) <<"Continuing to next syscall";
		t->cont_sysemu();
		break;
	case DREQ_STEP:
		t->cont_sysemu_singlestep();
		LOG(debug) <<"Stepping to next insn/syscall";
		break;
	default:
		FATAL() <<"Illegal debug request "<< req.type;
	}
	if (t->pending_sig()) {
		return false;
	}
	process_syscall(t, t->regs().orig_eax);
	return true;
}

/**
 * Process debugger requests made through |dbg| until action needs to
 * be taken by the caller (a resume-execution request is received).
 * The returned Task* is the target of the resume-execution request.
 *
 * The received request is returned through |req|.
 */
static Task* process_debugger_requests(struct dbg_context* dbg, Task* t,
				       struct dbg_request* req)
{
	while (true) {
		*req = dbg_get_request(dbg);
		if (dbg_is_resume_request(req)) {
			if (session->dying()) {
				return nullptr;
			}
			return t;
		}
		switch (req->type) {
		case DREQ_RESTART:
			return nullptr;

		case DREQ_READ_SIGINFO: {
			session->revive();
			// TODO: maybe share with replayer.cc?
			byte si_bytes[req->mem.len];
			memset(si_bytes, 0, sizeof(si_bytes));
			dbg_reply_read_siginfo(dbg,
					       si_bytes, sizeof(si_bytes));
			continue;
		}
		case DREQ_SET_QUERY_THREAD: {
			Task* next_task =
				t->session().find_task(req->target.tid);
			t = next_task ? next_task : t;
			break;
		}
		case DREQ_WRITE_SIGINFO:
			LOG(debug) <<"Experimental session dying at next continue request ...";
			session->start_dying();
			dbg_reply_write_siginfo(dbg);
			continue;

		default:
			break;
		}
		dispatch_debugger_request(*session, dbg, t, *req);
	}
}

void experiment(ReplaySession& replay, struct dbg_context* dbg, pid_t task,
		struct dbg_request* req)
{
	LOG(debug) <<"Starting debugging experiment for "<< HEX(&replay);

	session = replay.clone_experiment();
	Task* t = session->find_task(task);
	while (true) {
		if (!(t = process_debugger_requests(dbg, t, req))) {
			break;
		}
		if (!advance(t, *req)) {
			dbg_threadid_t thread;
			thread.pid = t->tgid();
			thread.tid = t->rec_tid;

			int sig = t->pending_sig();
			LOG(debug) <<"Tracee raised "<< signalname(sig);
			if (SIGTRAP != sig &&
			    (TRAP_BKPT_USER ==
			     t->vm()->get_breakpoint_type_at_addr(t->ip()))) {
				// See comment in replayer.cc near
				// breakpoint-dispatch code.
				sig = SIGTRAP;
			}
			LOG(debug) <<"  notifying debugger of "<< signalname(sig);
			dbg_notify_stop(dbg, thread, sig);
		}
	}

	LOG(debug) <<"... ending debugging experiment";
	session->kill_all_tasks();
	session = nullptr;
}
