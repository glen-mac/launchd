/*
 * Copyright (c) 1999-2008 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

static const char *const __rcs_file_version__ = "$Revision: 25122 $";

#include "config.h"
#include "launchd_runtime.h"

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/boolean.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/mig_errors.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/exception.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/proc.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/fcntl.h>
#include <sys/kdebug.h>
#include <bsm/libbsm.h>
#include <malloc/malloc.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <syslog.h>
#include <signal.h>
#include <dlfcn.h>

#include "launchd_internalServer.h"
#include "launchd_internal.h"
#include "notifyServer.h"
#include "mach_excServer.h"

/* We shouldn't be including these */
#include "launch.h"
#include "launchd.h"
#include "launchd_core_logic.h"
#include "vproc.h"
#include "vproc_priv.h"
#include "vproc_internal.h"
#include "protocol_vprocServer.h"
#include "protocol_job_reply.h"

#if !TARGET_OS_EMBEDDED
#include "domainServer.h"
#endif /* !TARGET_OS_EMBEDDED */
#include "eventsServer.h"

static mach_port_t ipc_port_set;
static mach_port_t demand_port_set;
static mach_port_t launchd_internal_port;
static int mainkq;

#define BULK_KEV_MAX 100
static struct kevent *bulk_kev;
static int bulk_kev_i;
static int bulk_kev_cnt;

static pthread_t kqueue_demand_thread;

static void mportset_callback(void);
static kq_callback kqmportset_callback = (kq_callback)mportset_callback;
static void *kqueue_demand_loop(void *arg);

boolean_t launchd_internal_demux(mach_msg_header_t *Request, mach_msg_header_t *Reply);
static void record_caller_creds(mach_msg_header_t *mh);
static void launchd_runtime2(mach_msg_size_t msg_size, mig_reply_error_t *bufRequest, mig_reply_error_t *bufReply);
static mach_msg_size_t max_msg_size;
static mig_callback *mig_cb_table;
static size_t mig_cb_table_sz;
static timeout_callback runtime_idle_callback;
static mach_msg_timeout_t runtime_idle_timeout;
static struct ldcred ldc;
static size_t runtime_standby_cnt;

static STAILQ_HEAD(, logmsg_s) logmsg_queue = STAILQ_HEAD_INITIALIZER(logmsg_queue);
static size_t logmsg_queue_sz;
static size_t logmsg_queue_cnt;
static mach_port_t drain_reply_port;
static void runtime_log_uncork_pending_drain(void);
static kern_return_t runtime_log_pack(vm_offset_t *outval, mach_msg_type_number_t *outvalCnt);

static bool logmsg_add(struct runtime_syslog_attr *attr, int err_num, const char *msg);
static void logmsg_remove(struct logmsg_s *lm);

static void do_file_init(void) __attribute__((constructor));
static mach_timebase_info_data_t tbi;
static uint64_t tbi_safe_math_max;
static uint64_t time_of_mach_msg_return;
static double tbi_float_val;

static const int sigigns[] = { SIGHUP, SIGINT, SIGPIPE, SIGALRM, SIGTERM,
	SIGURG, SIGTSTP, SIGTSTP, SIGCONT, SIGTTIN, SIGTTOU, SIGIO, SIGXCPU,
	SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGINFO, SIGUSR1, SIGUSR2
};
static sigset_t sigign_set;
static FILE *ourlogfile;
bool pid1_magic;
bool do_apple_internal_logging;
bool low_level_debug;
bool g_flat_mach_namespace = true;
bool g_simulate_pid1_crash = false;
bool g_malloc_log_stacks = false;
bool g_use_gmalloc = false;
bool g_log_per_user_shutdown = false;
#if !TARGET_OS_EMBEDDED
bool g_log_pid1_shutdown = true;
#else
bool g_log_pid1_shutdown = false;
#endif
bool g_log_strict_usage = false;
bool g_trap_sigkill_bugs = false;
pid_t g_wsp = 0;
size_t runtime_busy_cnt;

mach_port_t
runtime_get_kernel_port(void)
{
	return launchd_internal_port;
}

// static const char *__crashreporter_info__ = "";

static int internal_mask_pri = LOG_UPTO(LOG_NOTICE);


void
launchd_runtime_init(void)
{
	mach_msg_size_t mxmsgsz;
	pid_t p = getpid();

	launchd_assert((mainkq = kqueue()) != -1);

	launchd_assert((errno = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &demand_port_set)) == KERN_SUCCESS);
	launchd_assert((errno = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &ipc_port_set)) == KERN_SUCCESS);

	launchd_assert(kevent_mod(demand_port_set, EVFILT_MACHPORT, EV_ADD, 0, 0, &kqmportset_callback) != -1);

	launchd_assert(launchd_mport_create_recv(&launchd_internal_port) == KERN_SUCCESS);
	launchd_assert(launchd_mport_make_send(launchd_internal_port) == KERN_SUCCESS);

	/* Sigh... at the moment, MIG has maxsize == sizeof(reply union) */
	mxmsgsz = sizeof(union __RequestUnion__x_launchd_internal_subsystem);
	if (x_launchd_internal_subsystem.maxsize > mxmsgsz) {
		mxmsgsz = x_launchd_internal_subsystem.maxsize;
	}

	launchd_assert(runtime_add_mport(launchd_internal_port, launchd_internal_demux, mxmsgsz) == KERN_SUCCESS);
	launchd_assert(pthread_create(&kqueue_demand_thread, NULL, kqueue_demand_loop, NULL) == 0);
	launchd_assert(pthread_detach(kqueue_demand_thread) == 0);

	(void)launchd_assumes(sysctlbyname("vfs.generic.noremotehang", NULL, NULL, &p, sizeof(p)) != -1);
}

void
launchd_runtime_init2(void)
{
	size_t i;

	for (i = 0; i < (sizeof(sigigns) / sizeof(int)); i++) {
		sigaddset(&sigign_set, sigigns[i]);
		(void)launchd_assumes(signal(sigigns[i], SIG_IGN) != SIG_ERR);
	}
}

#define FLAGIF(f) if (flags & f) { flags_off += sprintf(flags_off, #f); flags &= ~f; }
const char *
reboot_flags_to_C_names(unsigned int flags)
{
#define MAX_RB_STR "RB_ASKNAME|RB_SINGLE|RB_NOSYNC|RB_HALT|RB_INITNAME|RB_DFLTROOT|RB_ALTBOOT|RB_UNIPROC|RB_SAFEBOOT|RB_UPSDELAY|0xdeadbeeffeedface"
	static char flags_buf[sizeof(MAX_RB_STR)];
	char *flags_off = NULL;

	if (flags == 0) {
		return "RB_AUTOBOOT";
	}

	while (flags) {
		if (flags_off) {
			*flags_off = '|';
			flags_off++;
			*flags_off = '\0';
		} else {
			flags_off = flags_buf;
		}

		FLAGIF(RB_ASKNAME)
		else FLAGIF(RB_SINGLE)
		else FLAGIF(RB_NOSYNC)
		else FLAGIF(RB_HALT)
		else FLAGIF(RB_INITNAME)
		else FLAGIF(RB_DFLTROOT)
		else FLAGIF(RB_ALTBOOT)
		else FLAGIF(RB_UNIPROC)
		else FLAGIF(RB_SAFEBOOT)
		else FLAGIF(RB_UPSDELAY)
		else {
			flags_off += sprintf(flags_off, "0x%x", flags);
			flags = 0;
		}
	}

	return flags_buf;
}

const char *
signal_to_C_name(unsigned int sig)
{
	static char unknown[25];

#define SIG2CASE(sg)	case sg: return #sg

	switch (sig) {
	SIG2CASE(SIGHUP);
	SIG2CASE(SIGINT);
	SIG2CASE(SIGQUIT);
	SIG2CASE(SIGILL);
	SIG2CASE(SIGTRAP);
	SIG2CASE(SIGABRT);
	SIG2CASE(SIGFPE);
	SIG2CASE(SIGKILL);
	SIG2CASE(SIGBUS);
	SIG2CASE(SIGSEGV);
	SIG2CASE(SIGSYS);
	SIG2CASE(SIGPIPE);
	SIG2CASE(SIGALRM);
	SIG2CASE(SIGTERM);
	SIG2CASE(SIGURG);
	SIG2CASE(SIGSTOP);
	SIG2CASE(SIGTSTP);
	SIG2CASE(SIGCONT);
	SIG2CASE(SIGCHLD);
	SIG2CASE(SIGTTIN);
	SIG2CASE(SIGTTOU);
	SIG2CASE(SIGIO);
	SIG2CASE(SIGXCPU);
	SIG2CASE(SIGXFSZ);
	SIG2CASE(SIGVTALRM);
	SIG2CASE(SIGPROF);
	SIG2CASE(SIGWINCH);
	SIG2CASE(SIGINFO);
	SIG2CASE(SIGUSR1);
	SIG2CASE(SIGUSR2);
	default:
		snprintf(unknown, sizeof(unknown), "%u", sig);
		return unknown;
	}
}

void
log_kevent_struct(int level, struct kevent *kev_base, int indx)
{
	struct kevent *kev = &kev_base[indx];
	const char *filter_str;
	char ident_buf[100];
	char filter_buf[100];
	char fflags_buf[1000];
	char flags_buf[1000] = "0x0";
	char *flags_off = NULL;
	char *fflags_off = NULL;
	unsigned short flags = kev->flags;
	unsigned int fflags = kev->fflags;

	if (likely(!(LOG_MASK(level) & internal_mask_pri))) {
		return;
	}

	if (flags) while (flags) {
		if (flags_off) {
			*flags_off = '|';
			flags_off++;
			*flags_off = '\0';
		} else {
			flags_off = flags_buf;
		}

		FLAGIF(EV_ADD)
		else FLAGIF(EV_RECEIPT)
		else FLAGIF(EV_DELETE)
		else FLAGIF(EV_ENABLE)
		else FLAGIF(EV_DISABLE)
		else FLAGIF(EV_CLEAR)
		else FLAGIF(EV_EOF)
		else FLAGIF(EV_ONESHOT)
		else FLAGIF(EV_ERROR)
		else {
			flags_off += sprintf(flags_off, "0x%hx", flags);
			flags = 0;
		}
	}

	snprintf(ident_buf, sizeof(ident_buf), "%ld", kev->ident);
	snprintf(fflags_buf, sizeof(fflags_buf), "0x%x", fflags);

	switch (kev->filter) {
	case EVFILT_READ:
		filter_str = "EVFILT_READ";
		break;
	case EVFILT_WRITE:
		filter_str = "EVFILT_WRITE";
		break;
	case EVFILT_AIO:
		filter_str = "EVFILT_AIO";
		break;
	case EVFILT_VNODE:
		filter_str = "EVFILT_VNODE";
		if (fflags) while (fflags) {
			if (fflags_off) {
				*fflags_off = '|';
				fflags_off++;
				*fflags_off = '\0';
			} else {
				fflags_off = fflags_buf;
			}

#define FFLAGIF(ff) if (fflags & ff) { fflags_off += sprintf(fflags_off, #ff); fflags &= ~ff; }

			FFLAGIF(NOTE_DELETE)
			else FFLAGIF(NOTE_WRITE)
			else FFLAGIF(NOTE_EXTEND)
			else FFLAGIF(NOTE_ATTRIB)
			else FFLAGIF(NOTE_LINK)
			else FFLAGIF(NOTE_RENAME)
			else FFLAGIF(NOTE_REVOKE)
			else {
				fflags_off += sprintf(fflags_off, "0x%x", fflags);
				fflags = 0;
			}
		}
		break;
	case EVFILT_PROC:
		filter_str = "EVFILT_PROC";
		if (fflags) while (fflags) {
			if (fflags_off) {
				*fflags_off = '|';
				fflags_off++;
				*fflags_off = '\0';
			} else {
				fflags_off = fflags_buf;
			}

			FFLAGIF(NOTE_EXIT)
			else FFLAGIF(NOTE_REAP)
			else FFLAGIF(NOTE_FORK)
			else FFLAGIF(NOTE_EXEC)
			else FFLAGIF(NOTE_SIGNAL)
			else FFLAGIF(NOTE_TRACK)
			else FFLAGIF(NOTE_TRACKERR)
			else FFLAGIF(NOTE_CHILD)
			else {
				fflags_off += sprintf(fflags_off, "0x%x", fflags);
				fflags = 0;
			}
		}
		break;
	case EVFILT_SIGNAL:
		filter_str = "EVFILT_SIGNAL";
		strcpy(ident_buf, signal_to_C_name(kev->ident));
		break;
	case EVFILT_TIMER:
		filter_str = "EVFILT_TIMER";
		snprintf(ident_buf, sizeof(ident_buf), "0x%lx", kev->ident);
		if (fflags) while (fflags) {
			if (fflags_off) {
				*fflags_off = '|';
				fflags_off++;
				*fflags_off = '\0';
			} else {
				fflags_off = fflags_buf;
			}

			FFLAGIF(NOTE_SECONDS)
			else FFLAGIF(NOTE_USECONDS)
			else FFLAGIF(NOTE_NSECONDS)
			else FFLAGIF(NOTE_ABSOLUTE)
			else {
				fflags_off += sprintf(fflags_off, "0x%x", fflags);
				fflags = 0;
			}
		}
		break;
	case EVFILT_MACHPORT:
		filter_str = "EVFILT_MACHPORT";
		snprintf(ident_buf, sizeof(ident_buf), "0x%lx", kev->ident);
		break;
	case EVFILT_FS:
		filter_str = "EVFILT_FS";
		snprintf(ident_buf, sizeof(ident_buf), "0x%lx", kev->ident);
		if (fflags) while (fflags) {
			if (fflags_off) {
				*fflags_off = '|';
				fflags_off++;
				*fflags_off = '\0';
			} else {
				fflags_off = fflags_buf;
			}

			FFLAGIF(VQ_NOTRESP)
			else FFLAGIF(VQ_NEEDAUTH)
			else FFLAGIF(VQ_LOWDISK)
			else FFLAGIF(VQ_MOUNT)
			else FFLAGIF(VQ_UNMOUNT)
			else FFLAGIF(VQ_DEAD)
			else FFLAGIF(VQ_ASSIST)
			else FFLAGIF(VQ_NOTRESPLOCK)
			else FFLAGIF(VQ_UPDATE)
			else {
				fflags_off += sprintf(fflags_off, "0x%x", fflags);
				fflags = 0;
			}
		}
		break;
	default:
		snprintf(filter_buf, sizeof(filter_buf), "%hd", kev->filter);
		filter_str = filter_buf;
		break;
	}

	runtime_syslog(level, "KEVENT[%d]: udata = %p data = 0x%lx ident = %s filter = %s flags = %s fflags = %s",
			indx, kev->udata, kev->data, ident_buf, filter_str, flags_buf, fflags_buf);
}

void
mportset_callback(void)
{
	mach_port_name_array_t members;
	mach_msg_type_number_t membersCnt;
	mach_port_status_t status;
	mach_msg_type_number_t statusCnt;
	struct kevent kev;
	unsigned int i;

	if (!launchd_assumes((errno = mach_port_get_set_status(mach_task_self(), demand_port_set, &members, &membersCnt)) == KERN_SUCCESS)) {
		return;
	}

	for (i = 0; i < membersCnt; i++) {
		statusCnt = MACH_PORT_RECEIVE_STATUS_COUNT;
		if (mach_port_get_attributes(mach_task_self(), members[i], MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&status,
					&statusCnt) != KERN_SUCCESS) {
			continue;
		}
		if (status.mps_msgcount) {
			EV_SET(&kev, members[i], EVFILT_MACHPORT, 0, 0, 0, job_find_by_service_port(members[i]));
#if 0
			if (launchd_assumes(kev.udata != NULL)) {
#endif
				log_kevent_struct(LOG_DEBUG, &kev, 0);
				(*((kq_callback *)kev.udata))(kev.udata, &kev);
#if 0
			} else {
				log_kevent_struct(LOG_ERR, &kev, 0);
			}
#endif
			/* the callback may have tainted our ability to continue this for loop */
			break;
		}
	}

	(void)launchd_assumes(vm_deallocate(mach_task_self(), (vm_address_t)members,
				(vm_size_t) membersCnt * sizeof(mach_port_name_t)) == KERN_SUCCESS);
}

void *
kqueue_demand_loop(void *arg __attribute__((unused)))
{
	fd_set rfds;

	/*
	 * Yes, at first glance, calling select() on a kqueue seems silly.
	 *
	 * This avoids a race condition between the main thread and this helper
	 * thread by ensuring that we drain kqueue events on the same thread
	 * that manipulates the kqueue.
	 */

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(mainkq, &rfds);
		if (launchd_assumes(select(mainkq + 1, &rfds, NULL, NULL, NULL) == 1)) {
			(void)launchd_assumes(handle_kqueue(launchd_internal_port, mainkq) == 0);
		}
	}

	return NULL;
}

kern_return_t
x_handle_kqueue(mach_port_t junk __attribute__((unused)), integer_t fd)
{
	struct timespec ts = { 0, 0 };
	struct kevent *kevi, kev[BULK_KEV_MAX];
	int i;

	bulk_kev = kev;

	if (launchd_assumes((bulk_kev_cnt = kevent(fd, NULL, 0, kev, BULK_KEV_MAX, &ts)) != -1)) {
#if 0	
		for (i = 0; i < bulk_kev_cnt; i++) {
			log_kevent_struct(LOG_DEBUG, &kev[0], i);
		}
#endif
		for (i = 0; i < bulk_kev_cnt; i++) {
			bulk_kev_i = i;
			kevi = &kev[i];

			if (kevi->filter) {
				runtime_syslog(LOG_DEBUG, "Dispatching kevent...");
				log_kevent_struct(LOG_DEBUG, kev, i);
#if 0
				/* Check if kevi->udata was either malloc(3)ed or is a valid function pointer. 
				 * If neither, it's probably an invalid pointer and we should log it. 
				 */
				Dl_info dli;
				if (launchd_assumes(malloc_size(kevi->udata) || dladdr(kevi->udata, &dli))) {
					runtime_ktrace(RTKT_LAUNCHD_BSD_KEVENT|DBG_FUNC_START, kevi->ident, kevi->filter, kevi->fflags);
					(*((kq_callback *)kevi->udata))(kevi->udata, kevi);
					runtime_ktrace0(RTKT_LAUNCHD_BSD_KEVENT|DBG_FUNC_END);
				} else {
					runtime_syslog(LOG_ERR, "The following kevent had invalid context data.");
					log_kevent_struct(LOG_EMERG, &kev[0], i);
				}
#else
				struct job_check_s {
					kq_callback kqc;
				};

				struct job_check_s *check = kevi->udata;
				if (check && check->kqc) {
					runtime_ktrace(RTKT_LAUNCHD_BSD_KEVENT|DBG_FUNC_START, kevi->ident, kevi->filter, kevi->fflags);
					(*((kq_callback *)kevi->udata))(kevi->udata, kevi);
					runtime_ktrace0(RTKT_LAUNCHD_BSD_KEVENT|DBG_FUNC_END);
				} else {
					runtime_syslog(LOG_ERR, "The following kevent had invalid context data. Please file a bug with the following information:");
					log_kevent_struct(LOG_EMERG, &kev[0], i);
				}
#endif
			}
		}
	}

	bulk_kev = NULL;

	return 0;
}

void
launchd_runtime(void)
{
	mig_reply_error_t *req = NULL, *resp = NULL;
	mach_msg_size_t mz = max_msg_size;
	int flags = VM_MAKE_TAG(VM_MEMORY_MACH_MSG)|TRUE;

	for (;;) {
		if (likely(req)) {
			(void)launchd_assumes(vm_deallocate(mach_task_self(), (vm_address_t)req, mz) == KERN_SUCCESS);
			req = NULL;
		}
		if (likely(resp)) {
			(void)launchd_assumes(vm_deallocate(mach_task_self(), (vm_address_t)resp, mz) == KERN_SUCCESS);
			resp = NULL;
		}

		mz = max_msg_size;

		if (!launchd_assumes(vm_allocate(mach_task_self(), (vm_address_t *)&req, mz, flags) == KERN_SUCCESS)) {
			continue;
		}
		if (!launchd_assumes(vm_allocate(mach_task_self(), (vm_address_t *)&resp, mz, flags) == KERN_SUCCESS)) {
			continue;
		}

		launchd_runtime2(mz, req, resp);

		/* If we get here, max_msg_size probably changed... */
	}
}

kern_return_t
launchd_set_bport(mach_port_t name)
{
	return errno = task_set_bootstrap_port(mach_task_self(), name);
}

kern_return_t
launchd_get_bport(mach_port_t *name)
{
	return errno = task_get_bootstrap_port(mach_task_self(), name);
}

kern_return_t
launchd_mport_notify_req(mach_port_t name, mach_msg_id_t which)
{
	mach_port_mscount_t msgc = (which == MACH_NOTIFY_PORT_DESTROYED) ? 0 : 1;
	mach_port_t previous, where = (which == MACH_NOTIFY_NO_SENDERS) ? name : launchd_internal_port;

	if (which == MACH_NOTIFY_NO_SENDERS) {
		/* Always make sure the send count is zero, in case a receive right is reused */
		errno = mach_port_set_mscount(mach_task_self(), name, 0);
		if (unlikely(errno != KERN_SUCCESS)) {
			return errno;
		}
	}

	errno = mach_port_request_notification(mach_task_self(), name, which, msgc, where,
			MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);

	if (likely(errno == 0) && previous != MACH_PORT_NULL) {
		(void)launchd_assumes(launchd_mport_deallocate(previous) == KERN_SUCCESS);
	}

	return errno;
}

pid_t
runtime_fork(mach_port_t bsport)
{
	sigset_t emptyset, oset;
	pid_t r = -1;
	int saved_errno;
	size_t i;

	sigemptyset(&emptyset);

	(void)launchd_assumes(launchd_mport_make_send(bsport) == KERN_SUCCESS);
	(void)launchd_assumes(launchd_set_bport(bsport) == KERN_SUCCESS);
	(void)launchd_assumes(launchd_mport_deallocate(bsport) == KERN_SUCCESS);

	(void)launchd_assumes(sigprocmask(SIG_BLOCK, &sigign_set, &oset) != -1);
	for (i = 0; i < (sizeof(sigigns) / sizeof(int)); i++) {
		(void)launchd_assumes(signal(sigigns[i], SIG_DFL) != SIG_ERR);
	}

	r = fork();
	saved_errno = errno;

	if (r != 0) {
		for (i = 0; i < (sizeof(sigigns) / sizeof(int)); i++) {
			(void)launchd_assumes(signal(sigigns[i], SIG_IGN) != SIG_ERR);
		}
		(void)launchd_assumes(sigprocmask(SIG_SETMASK, &oset, NULL) != -1);
		(void)launchd_assumes(launchd_set_bport(MACH_PORT_NULL) == KERN_SUCCESS);
	} else {
		pid_t p = -getpid();
		(void)launchd_assumes(sysctlbyname("vfs.generic.noremotehang", NULL, NULL, &p, sizeof(p)) != -1);

		(void)launchd_assumes(sigprocmask(SIG_SETMASK, &emptyset, NULL) != -1);
	}

	errno = saved_errno;

	return r;
}


void
runtime_set_timeout(timeout_callback to_cb, unsigned int sec)
{
	if (sec == 0 || to_cb == NULL) {
		runtime_idle_callback = NULL;
		runtime_idle_timeout = 0;
	}

	runtime_idle_callback = to_cb;
	runtime_idle_timeout = sec * 1000;
}

kern_return_t
runtime_add_mport(mach_port_t name, mig_callback demux, mach_msg_size_t msg_size)
{
	size_t needed_table_sz = (MACH_PORT_INDEX(name) + 1) * sizeof(mig_callback);
	mach_port_t target_set = demux ? ipc_port_set : demand_port_set;

	msg_size = round_page(msg_size + MAX_TRAILER_SIZE);

	if (unlikely(needed_table_sz > mig_cb_table_sz)) {
		needed_table_sz *= 2; /* Let's try and avoid realloc'ing for a while */
		mig_callback *new_table = malloc(needed_table_sz);

		if (!launchd_assumes(new_table != NULL)) {
			return KERN_RESOURCE_SHORTAGE;
		}

		if (likely(mig_cb_table)) {
			memcpy(new_table, mig_cb_table, mig_cb_table_sz);
			free(mig_cb_table);
		}

		mig_cb_table_sz = needed_table_sz;
		mig_cb_table = new_table;
	}

	mig_cb_table[MACH_PORT_INDEX(name)] = demux;

	if (msg_size > max_msg_size) {
		max_msg_size = msg_size;
	}

	return errno = mach_port_move_member(mach_task_self(), name, target_set);
}

kern_return_t
runtime_remove_mport(mach_port_t name)
{
	mig_cb_table[MACH_PORT_INDEX(name)] = NULL;

	return errno = mach_port_move_member(mach_task_self(), name, MACH_PORT_NULL);
}

kern_return_t
launchd_mport_make_send(mach_port_t name)
{
	return errno = mach_port_insert_right(mach_task_self(), name, name, MACH_MSG_TYPE_MAKE_SEND);
}

kern_return_t
launchd_mport_copy_send(mach_port_t name)
{
	return errno = mach_port_insert_right(mach_task_self(), name, name, MACH_MSG_TYPE_COPY_SEND);
}

kern_return_t
launchd_mport_make_send_once(mach_port_t name, mach_port_t *so)
{
	mach_msg_type_name_t right = 0;
	return errno = mach_port_extract_right(mach_task_self(), name, MACH_MSG_TYPE_MAKE_SEND_ONCE, so, &right);
}

kern_return_t
launchd_mport_close_recv(mach_port_t name)
{
	return errno = mach_port_mod_refs(mach_task_self(), name, MACH_PORT_RIGHT_RECEIVE, -1);
}

kern_return_t
launchd_mport_create_recv(mach_port_t *name)
{
	return errno = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, name);
}

kern_return_t
launchd_mport_deallocate(mach_port_t name)
{
	return errno = mach_port_deallocate(mach_task_self(), name);
}

int
kevent_bulk_mod(struct kevent *kev, size_t kev_cnt)
{
	size_t i;

	for (i = 0; i < kev_cnt; i++) {
		kev[i].flags |= EV_CLEAR|EV_RECEIPT;
	}

	return kevent(mainkq, kev, kev_cnt, kev, kev_cnt, NULL);
}

int
kevent_mod(uintptr_t ident, short filter, u_short flags, u_int fflags, intptr_t data, void *udata)
{
	struct kevent kev;
	int r;

	switch (filter) {
	case EVFILT_READ:
	case EVFILT_WRITE:
		break;
	case EVFILT_TIMER:
		/* Workaround 5225889 */
		if (flags & EV_ADD) {
			kevent_mod(ident, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
		}
		/* fall through */
	default:
		flags |= EV_CLEAR;
		break;
	}

	flags |= EV_RECEIPT;

	if (flags & EV_ADD && !launchd_assumes(udata != NULL)) {
		errno = EINVAL;
		return -1;
	} else if ((flags & EV_DELETE) && bulk_kev) {
		int i = 0;
		for (i = bulk_kev_i + 1; i < bulk_kev_cnt; i++) {
			if (bulk_kev[i].filter == filter && bulk_kev[i].ident == ident) {
				runtime_syslog(LOG_DEBUG, "Pruning the following kevent:");
				log_kevent_struct(LOG_DEBUG, &bulk_kev[0], i);
				bulk_kev[i].filter = (short)0;
			}
		}
	}

	EV_SET(&kev, ident, filter, flags, fflags, data, udata);

	r = kevent(mainkq, &kev, 1, &kev, 1, NULL);

	if (!launchd_assumes(r == 1)) {
		return -1;
	}

	if (launchd_assumes(kev.flags & EV_ERROR)) {
		if ((flags & EV_ADD) && kev.data) {
			runtime_syslog(LOG_DEBUG, "%s(): See the next line...", __func__);
			log_kevent_struct(LOG_DEBUG, &kev, 0);
			errno = kev.data;
			return -1;
		}
	}

	return r;
}

boolean_t
launchd_internal_demux(mach_msg_header_t *Request, mach_msg_header_t *Reply)
{
	if (launchd_internal_server_routine(Request)) {
		return launchd_internal_server(Request, Reply);
	} else if (notify_server_routine(Request)) {
		return notify_server(Request, Reply);
	} else {
		return mach_exc_server(Request, Reply);
	}
}

kern_return_t
do_mach_notify_port_destroyed(mach_port_t notify __attribute__((unused)), mach_port_t rights)
{
	/* This message is sent to us when a receive right is returned to us. */

	if (!launchd_assumes(job_ack_port_destruction(rights))) {
		(void)launchd_assumes(launchd_mport_close_recv(rights) == KERN_SUCCESS);
	}

	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_port_deleted(mach_port_t notify __attribute__((unused)), mach_port_name_t name __attribute__((unused)))
{
	/* If we deallocate/destroy/mod_ref away a port with a pending
	 * notification, the original notification message is replaced with
	 * this message. To quote a Mach kernel expert, "the kernel has a
	 * send-once right that has to be used somehow."
	 */
	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_no_senders(mach_port_t notify, mach_port_mscount_t mscount __attribute__((unused)))
{
	job_t j = job_mig_intran(notify);

	/* This message is sent to us when the last customer of one of our
	 * objects goes away.
	 */

	if (!launchd_assumes(j != NULL)) {
		return KERN_FAILURE;
	}

	job_ack_no_senders(j);

	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_send_once(mach_port_t notify __attribute__((unused)))
{
	/*
	 * This message is sent for each send-once right that is deallocated
	 * without being used.
	 */

	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_dead_name(mach_port_t notify __attribute__((unused)), mach_port_name_t name)
{
	/* This message is sent to us when one of our send rights no longer has
	 * a receiver somewhere else on the system.
	 */

	if (name == drain_reply_port) {
		(void)launchd_assumes(launchd_mport_deallocate(name) == KERN_SUCCESS);
		drain_reply_port = MACH_PORT_NULL;
	}

	if (launchd_assumes(root_jobmgr != NULL)) {
		root_jobmgr = jobmgr_delete_anything_with_port(root_jobmgr, name);
	}

	/* A dead-name notification about a port appears to increment the
	 * rights on said port. Let's deallocate it so that we don't leak
	 * dead-name ports.
	 */
	(void)launchd_assumes(launchd_mport_deallocate(name) == KERN_SUCCESS);

	return KERN_SUCCESS;
}

void
record_caller_creds(mach_msg_header_t *mh)
{
	mach_msg_max_trailer_t *tp;
	size_t trailer_size;

	tp = (mach_msg_max_trailer_t *)((vm_offset_t)mh + round_msg(mh->msgh_size));

	trailer_size = tp->msgh_trailer_size - (mach_msg_size_t)(sizeof(mach_msg_trailer_type_t) - sizeof(mach_msg_trailer_size_t));

	if (launchd_assumes(trailer_size >= (mach_msg_size_t)sizeof(audit_token_t))) {
		audit_token_to_au32(tp->msgh_audit, /* audit UID */ NULL, &ldc.euid,
				&ldc.egid, &ldc.uid, &ldc.gid, &ldc.pid,
				&ldc.asid, /* au_tid_t */ NULL);
	}

}

struct ldcred *
runtime_get_caller_creds(void)
{
	return &ldc;
}

mach_msg_return_t
launchd_exc_runtime_once(mach_port_t port, mach_msg_size_t rcv_msg_size, mach_msg_size_t send_msg_size, mig_reply_error_t *bufRequest, mig_reply_error_t *bufReply, mach_msg_timeout_t to)
{
	mach_msg_return_t mr = ~MACH_MSG_SUCCESS;
	mach_msg_option_t rcv_options =	MACH_RCV_MSG										|
									MACH_RCV_TIMEOUT									|
									MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT)	|
									MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)	;
				
	do {
		mr = mach_msg(&bufRequest->Head, rcv_options, 0, rcv_msg_size, port, to, MACH_PORT_NULL);
		switch (mr) {
			case MACH_RCV_TIMED_OUT	:
				runtime_syslog(LOG_DEBUG, "Message queue is empty.");
				break;
			case MACH_RCV_TOO_LARGE	:
				runtime_syslog(LOG_INFO, "Message is larger than %u bytes.", rcv_msg_size);
				break;
			default					:
				(void)launchd_assumes(mr == MACH_MSG_SUCCESS);
		}
		
		if (mr == MACH_MSG_SUCCESS) {
			if (!launchd_assumes(mach_exc_server(&bufRequest->Head, &bufReply->Head) == TRUE)) {
				runtime_syslog(LOG_WARNING, "Exception server routine failed.");
				break;
			}
			
			mach_msg_return_t smr = ~MACH_MSG_SUCCESS;
			mach_msg_option_t send_options =	MACH_SEND_MSG		|
												MACH_SEND_TIMEOUT	;
			
			(void)launchd_assumes(bufReply->Head.msgh_size <= send_msg_size);
			smr = mach_msg(&bufReply->Head, send_options, bufReply->Head.msgh_size, 0, MACH_PORT_NULL, to + 100, MACH_PORT_NULL);
			switch (smr) {
				case MACH_SEND_TIMED_OUT	:
					runtime_syslog(LOG_WARNING, "Timed out while trying to send reply to exception message.");
					break;
				case MACH_SEND_INVALID_DEST	:
					runtime_syslog(LOG_WARNING, "Tried sending a message to a port that we don't possess a send right to.");
					break;
				default						:
					if (!launchd_assumes(smr == MACH_MSG_SUCCESS)) {
						runtime_syslog(LOG_WARNING, "Couldn't deliver exception reply: 0x%x", smr);
					}
					break;
			}
		}
	} while (0);
	
	return mr;
}

void
launchd_runtime2(mach_msg_size_t msg_size, mig_reply_error_t *bufRequest, mig_reply_error_t *bufReply)
{
	mach_msg_options_t options, tmp_options;
	mig_reply_error_t *bufTemp;
	mig_callback the_demux;
	mach_msg_timeout_t to;
	mach_msg_return_t mr;
	size_t busy_cnt;

	options = MACH_RCV_MSG|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT) |
		MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0);

	tmp_options = options;

	for (;;) {
		busy_cnt = runtime_busy_cnt + runtime_standby_cnt;
		to = MACH_MSG_TIMEOUT_NONE;

		if (unlikely(msg_size != max_msg_size)) {
			/* The buffer isn't big enough to receive messages anymore... */
			tmp_options &= ~MACH_RCV_MSG;
			options &= ~MACH_RCV_MSG;
			if (!(tmp_options & MACH_SEND_MSG)) {
				return;
			}
		}

		if ((tmp_options & MACH_RCV_MSG) && (runtime_idle_callback || (busy_cnt == 0))) {
			tmp_options |= MACH_RCV_TIMEOUT;

			if (!(tmp_options & MACH_SEND_TIMEOUT)) {
#if !TARGET_OS_EMBEDDED
				to = busy_cnt ? runtime_idle_timeout : (_vproc_standby_timeout() * 1000);
#else
				to = runtime_idle_timeout;
#endif
			}
		}

		runtime_log_push();

		mr = mach_msg(&bufReply->Head, tmp_options, bufReply->Head.msgh_size,
				msg_size, ipc_port_set, to, MACH_PORT_NULL);

		time_of_mach_msg_return = runtime_get_opaque_time();

		tmp_options = options;

		/* It looks like the compiler doesn't optimize switch(unlikely(...)) See: 5691066 */
		if (unlikely(mr)) switch (mr) {
		case MACH_SEND_INVALID_DEST:
		case MACH_SEND_TIMED_OUT:
			/* We need to clean up and start over. */
			if (bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
				mach_msg_destroy(&bufReply->Head);
			}
			continue;
		case MACH_RCV_TIMED_OUT:
			if (to != MACH_MSG_TIMEOUT_NONE) {
				if (busy_cnt == 0) {
					runtime_syslog(LOG_INFO, "Idle exiting.");
					launchd_shutdown();
				} else if (runtime_idle_callback) {
					runtime_idle_callback();
				}
			}
			continue;
		default:
			if (!launchd_assumes(mr == MACH_MSG_SUCCESS)) {
				runtime_syslog(LOG_ERR, "mach_msg(): %u: %s", mr, mach_error_string(mr));
			}
			continue;
		}

		bufTemp = bufRequest;
		bufRequest = bufReply;
		bufReply = bufTemp;

		if (unlikely(!(tmp_options & MACH_RCV_MSG))) {
			continue;
		}

		/* we have another request message */
#if 0
		if (!launchd_assumes(mig_cb_table != NULL)) {
			break;
		}
#endif

		the_demux = mig_cb_table[MACH_PORT_INDEX(bufRequest->Head.msgh_local_port)];

#if 0
		if (!launchd_assumes(the_demux != NULL)) {
			break;
		}
#endif

		record_caller_creds(&bufRequest->Head);
		runtime_ktrace(RTKT_LAUNCHD_MACH_IPC|DBG_FUNC_START, bufRequest->Head.msgh_local_port, bufRequest->Head.msgh_id, (long)the_demux);

		if (the_demux(&bufRequest->Head, &bufReply->Head) == FALSE) {
			/* XXX - also gross */
			if (likely(bufRequest->Head.msgh_id == MACH_NOTIFY_NO_SENDERS)) {
				notify_server(&bufRequest->Head, &bufReply->Head);
			} else if (the_demux == protocol_vproc_server) {
				
#if !TARGET_OS_EMBEDDED
				/* Similarly gross. */
				if (xpc_domain_server(&bufRequest->Head, &bufReply->Head) == FALSE) {
					(void)xpc_events_server(&bufRequest->Head, &bufReply->Head);
				}
#else
				(void)xpc_events_server(&bufRequest->Head, &bufReply->Head);
#endif /* !TARGET_OS_EMBEDDED */
			}
		}

		runtime_ktrace(RTKT_LAUNCHD_MACH_IPC|DBG_FUNC_END, bufReply->Head.msgh_remote_port, bufReply->Head.msgh_bits, bufReply->RetCode);

		/* bufReply is a union. If MACH_MSGH_BITS_COMPLEX is set, then bufReply->RetCode is assumed to be zero. */
		if (!(bufReply->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
			if (unlikely(bufReply->RetCode != KERN_SUCCESS)) {
				if (likely(bufReply->RetCode == MIG_NO_REPLY)) {
					bufReply->Head.msgh_remote_port = MACH_PORT_NULL;
				} else if (bufRequest->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
					/* destroy the request - but not the reply port */
					bufRequest->Head.msgh_remote_port = MACH_PORT_NULL;
					mach_msg_destroy(&bufRequest->Head);
				}
			}
		}

		if (likely(bufReply->Head.msgh_remote_port != MACH_PORT_NULL)) {
			tmp_options |= MACH_SEND_MSG;

			if (unlikely(MACH_MSGH_BITS_REMOTE(bufReply->Head.msgh_bits) != MACH_MSG_TYPE_MOVE_SEND_ONCE)) {
				tmp_options |= MACH_SEND_TIMEOUT;
			}
		}
	}
}

int
runtime_close(int fd)
{
	int i;

	if (bulk_kev) for (i = bulk_kev_i + 1; i < bulk_kev_cnt; i++) {
		switch (bulk_kev[i].filter) {
		case EVFILT_VNODE:
		case EVFILT_WRITE:
		case EVFILT_READ:
			if (unlikely((int)bulk_kev[i].ident == fd)) {
				runtime_syslog(LOG_DEBUG, "Skipping kevent index: %d", i);
				bulk_kev[i].filter = 0;
			}
		default:
			break;
		}
	}

	return close(fd);
}

void
runtime_closelog(void)
{
	runtime_log_push();

	if (ourlogfile) {
		(void)launchd_assumes(fflush(ourlogfile) == 0);
		(void)launchd_assumes(runtime_fsync(fileno(ourlogfile)) != -1);
	}
}

int
runtime_fsync(int fd)
{
#if 0
	if (do_apple_internal_logging) {
		return fcntl(fd, F_FULLFSYNC, NULL);
	} else {
		return fsync(fd);
	}
#else
	return fsync(fd);
#endif
}

int
runtime_setlogmask(int maskpri)
{
	internal_mask_pri = maskpri;

	return internal_mask_pri;
}

void
runtime_syslog(int pri, const char *message, ...)
{
	struct runtime_syslog_attr attr = {
		g_my_label, 
		g_my_label,
		pid1_magic ? "System" : "Background",
		pri, 
		getuid(), 
		getpid(), 
		getpid()
	};
	va_list ap;

	va_start(ap, message);
	runtime_vsyslog(&attr, message, ap);

	va_end(ap);
}

void
runtime_vsyslog(struct runtime_syslog_attr *attr, const char *message, va_list args)
{
	int saved_errno = errno;
	char newmsg[10000];
	bool echo_to_console= false;

	if (attr->priority == LOG_APPLEONLY) {
		if (do_apple_internal_logging) {
			attr->priority = LOG_NOTICE;
		} else {
			return;
		}
	} else if (attr->priority == LOG_SCOLDING) {
		attr->priority = g_log_strict_usage ? LOG_NOTICE : LOG_DEBUG;
	}

	if (attr->priority & LOG_CONSOLE) {
		echo_to_console = true;
		attr->priority &= ~LOG_CONSOLE;
	}

	if (!(LOG_MASK(attr->priority) & internal_mask_pri)) {
		return;
	}

	vsnprintf(newmsg, sizeof(newmsg), message, args);

	if (g_console && (unlikely(low_level_debug) || echo_to_console)) {
		fprintf(g_console, "%s %u\t%s %u\t%s\n", attr->from_name, attr->from_pid, attr->about_name, attr->about_pid, newmsg);
	}

	logmsg_add(attr, saved_errno, newmsg);
}

bool
logmsg_add(struct runtime_syslog_attr *attr, int err_num, const char *msg)
{
	size_t lm_sz = sizeof(struct logmsg_s) + strlen(msg) + strlen(attr->from_name) + strlen(attr->about_name) + strlen(attr->session_name) + 4;
	char *data_off;
	struct logmsg_s *lm;

#define ROUND_TO_64BIT_WORD_SIZE(x)	((x + 7) & ~7)

	/* we do this to make the unpacking for the log_drain cause unalignment faults */
	lm_sz = ROUND_TO_64BIT_WORD_SIZE(lm_sz);

	if (unlikely((lm = calloc(1, lm_sz)) == NULL)) {
		return false;
	}

	data_off = lm->data;

	lm->when = runtime_get_wall_time();
	lm->from_pid = attr->from_pid;
	lm->about_pid = attr->about_pid;
	lm->err_num = err_num;
	lm->pri = attr->priority;
	lm->obj_sz = lm_sz;
	lm->msg = data_off;
	data_off += sprintf(data_off, "%s", msg) + 1;
	lm->from_name = data_off;
	data_off += sprintf(data_off, "%s", attr->from_name) + 1;
	lm->about_name = data_off;
	data_off += sprintf(data_off, "%s", attr->about_name) + 1;
	lm->session_name = data_off;
	data_off += sprintf(data_off, "%s", attr->session_name) + 1;

	STAILQ_INSERT_TAIL(&logmsg_queue, lm, sqe);
	logmsg_queue_sz += lm_sz;
	logmsg_queue_cnt++;

	return true;
}

void
logmsg_remove(struct logmsg_s *lm)
{
	STAILQ_REMOVE(&logmsg_queue, lm, logmsg_s, sqe);
	logmsg_queue_sz -= lm->obj_sz;
	logmsg_queue_cnt--;

	free(lm);
}
 
kern_return_t
runtime_log_pack(vm_offset_t *outval, mach_msg_type_number_t *outvalCnt)
{
	struct logmsg_s *lm;
	void *offset;

	*outvalCnt = logmsg_queue_sz;

	mig_allocate(outval, *outvalCnt);

	if (unlikely(*outval == 0)) {
		return 1;
	}

	offset = (void *)*outval;

	if (g_log_per_user_shutdown && !ourlogfile && !pid1_magic && shutdown_in_progress) {
		char logfile[NAME_MAX];
		snprintf(logfile, sizeof(logfile), "/var/tmp/launchd-%s.shutdown.log", g_username);
		
		char logfile1[NAME_MAX];
		snprintf(logfile1, sizeof(logfile1), "/var/tmp/launchd-%s.shutdown.log.1", g_username);
		
		rename(logfile, logfile1);
		ourlogfile = fopen(logfile, "a");
	}

	static int64_t shutdown_start = 0;
	if (shutdown_start == 0) {
		shutdown_start = runtime_get_wall_time();
	}

	while ((lm = STAILQ_FIRST(&logmsg_queue))) {
		int64_t log_delta = lm->when - shutdown_start;
		if (!pid1_magic && ourlogfile) {
			fprintf(ourlogfile, "%8lld%6u %-40s%6u %-40s %s\n", log_delta,
					lm->from_pid, lm->from_name, lm->about_pid, lm->about_name, lm->msg);
			fflush(ourlogfile);
		}

		lm->from_name_offset = lm->from_name - (char *)lm;
		lm->about_name_offset = lm->about_name - (char *)lm;
		lm->msg_offset = lm->msg - (char *)lm;
		lm->session_name_offset = lm->session_name - (char *)lm;

		memcpy(offset, lm, lm->obj_sz);
		
		offset += lm->obj_sz;

		logmsg_remove(lm);
	}
	
	if (ourlogfile) {
		fflush(ourlogfile);
	}

	return 0;
}

void
runtime_log_uncork_pending_drain(void)
{
	mach_msg_type_number_t outvalCnt;
	mach_port_t tmp_port;
	vm_offset_t outval;

	if (!drain_reply_port) {
		return;
	}

	if (logmsg_queue_cnt == 0) {
		return;
	}

	if (runtime_log_pack(&outval, &outvalCnt) != 0) {
		return;
	}

	tmp_port = drain_reply_port;
	drain_reply_port = MACH_PORT_NULL;

	if (unlikely(errno = job_mig_log_drain_reply(tmp_port, 0, outval, outvalCnt))) {
		(void)launchd_assumes(errno == MACH_SEND_INVALID_DEST);
		(void)launchd_assumes(launchd_mport_deallocate(tmp_port) == KERN_SUCCESS);
	}

	mig_deallocate(outval, outvalCnt);
}

void
runtime_log_push(void)
{
	static pthread_mutex_t ourlock = PTHREAD_MUTEX_INITIALIZER;
	static int64_t shutdown_start, log_delta;
	mach_msg_type_number_t outvalCnt;
	struct logmsg_s *lm;
	vm_offset_t outval;

	if (logmsg_queue_cnt == 0) {
		(void)launchd_assumes(STAILQ_EMPTY(&logmsg_queue));
		return;
	} else if (!pid1_magic) {
		if (runtime_log_pack(&outval, &outvalCnt) == 0) {
			(void)launchd_assumes(_vprocmgr_log_forward(inherited_bootstrap_port, (void *)outval, outvalCnt) == NULL);
			mig_deallocate(outval, outvalCnt);
		}
		return;
	}

	if (likely(!shutdown_in_progress && !fake_shutdown_in_progress)) {
		runtime_log_uncork_pending_drain();
		return;
	}

	if (unlikely(shutdown_start == 0)) {
		shutdown_start = runtime_get_wall_time();
		launchd_log_vm_stats();
	}

	pthread_mutex_lock(&ourlock);

	if (unlikely(ourlogfile == NULL) && g_log_pid1_shutdown) {
		rename("/var/log/launchd-shutdown.log", "/var/log/launchd-shutdown.log.1");
		ourlogfile = fopen("/var/log/launchd-shutdown.log", "a");
	}

	pthread_mutex_unlock(&ourlock);

	if (unlikely(!ourlogfile)) {
		return;
	}

	while ((lm = STAILQ_FIRST(&logmsg_queue))) {
		log_delta = lm->when - shutdown_start;

		fprintf(ourlogfile, "%8lld%6u %-40s%6u %-40s %s\n", log_delta,
				lm->from_pid, lm->from_name, lm->about_pid, lm->about_name, lm->msg);

		logmsg_remove(lm);
	}
	
	fflush(ourlogfile);
}

kern_return_t
runtime_log_forward(uid_t forward_uid, gid_t forward_gid, vm_offset_t inval, mach_msg_type_number_t invalCnt)
{
	struct logmsg_s *lm, *lm_walk;
	mach_msg_type_number_t data_left = invalCnt;

	if (inval == 0) {
		return 0;
	}

	for (lm_walk = (struct logmsg_s *)inval; (data_left > 0) && (lm_walk->obj_sz <= data_left); lm_walk = ((void *)lm_walk + lm_walk->obj_sz)) {
		/* malloc() does not return NULL if you ask it for an allocation of size 0.
		 * It will return a valid pointer that can be passed to free(). If we don't
		 * do this check, we'll wind up corrupting our heap in the subsequent 
		 * assignments.
		 *
		 * We break out if this check fails because, obj_sz is supposed to include
		 * the size of the logmsg_s struct. If it claims to be of zero size, we
		 * can't safely increment our counter because something obviously got screwed
		 * up along the way, since this should always be at least sizeof(struct logmsg_s).
		 */
		if (!launchd_assumes(lm_walk->obj_sz > 0)) {
			runtime_syslog(LOG_WARNING, "Encountered a log message of size 0 with %u bytes left in forwarded data. Ignoring remaining messages.", data_left);
			break;
		}
		
		/* If malloc() keeps failing, we shouldn't put additional pressure on the system
		 * by attempting to add more messages to the log until it returns success
		 * log a failure, hope pressure lets off, and move on.
		 */
		if (!launchd_assumes(lm = malloc(lm_walk->obj_sz))) {
			runtime_syslog(LOG_WARNING, "Failed to allocate %llu bytes for log message with %u bytes left in forwarded data. Ignoring remaining messages.", lm_walk->obj_sz, data_left);
			break;
		}

		memcpy(lm, lm_walk, lm_walk->obj_sz);
		lm->sender_uid = forward_uid;
		lm->sender_gid = forward_gid;

		lm->from_name += (size_t)lm;
		lm->about_name += (size_t)lm;
		lm->msg += (size_t)lm;
		lm->session_name += (size_t)lm;

		STAILQ_INSERT_TAIL(&logmsg_queue, lm, sqe);
		logmsg_queue_sz += lm->obj_sz;
		logmsg_queue_cnt++;

		data_left -= lm->obj_sz;
	}

	mig_deallocate(inval, invalCnt);

	return 0;
}

kern_return_t
runtime_log_drain(mach_port_t srp, vm_offset_t *outval, mach_msg_type_number_t *outvalCnt)
{
	(void)launchd_assumes(drain_reply_port == 0);

	if ((logmsg_queue_cnt == 0) || shutdown_in_progress || fake_shutdown_in_progress) {
		drain_reply_port = srp;
		(void)launchd_assumes(launchd_mport_notify_req(drain_reply_port, MACH_NOTIFY_DEAD_NAME) == KERN_SUCCESS);

		return MIG_NO_REPLY;
	}

	return runtime_log_pack(outval, outvalCnt);
}

/*
 * We should break this into two reference counts.
 *
 * One for hard references that would prevent exiting.
 * One for soft references that would only prevent idle exiting.
 *
 * In the long run, reference counting should completely automate when a
 * process can and should exit.
 */
void
runtime_add_ref(void)
{
	if (!pid1_magic) {
	#if !TARGET_OS_EMBEDDED
		_vproc_transaction_begin();
	#endif
	}
	
	runtime_busy_cnt++;
	runtime_remove_timer();
}

void
runtime_del_ref(void)
{
	if (!pid1_magic) {
	#if !TARGET_OS_EMBEDDED
		if (_vproc_transaction_count() == 0) {
			runtime_syslog(LOG_INFO, "Exiting cleanly.");
		}
		
		runtime_closelog();
		_vproc_transaction_end();
	#endif
	}
	
	runtime_busy_cnt--;
	runtime_install_timer();
}

void
runtime_add_weak_ref(void)
{
	if (!pid1_magic) {
	#if !TARGET_OS_EMBEDDED
		_vproc_standby_begin();
	#endif
	}
	runtime_standby_cnt++;
}

void
runtime_del_weak_ref(void)
{
	if (!pid1_magic) {
	#if !TARGET_OS_EMBEDDED
		_vproc_standby_end();
	#endif
	}
	runtime_standby_cnt--;
}

void
runtime_install_timer(void)
{
	if (!pid1_magic && runtime_busy_cnt == 0) {
		(void)launchd_assumes(kevent_mod((uintptr_t)&g_runtime_busy_time, EVFILT_TIMER, EV_ADD, NOTE_SECONDS, 30, root_jobmgr) != -1);
	}
}

void
runtime_remove_timer(void)
{
	if (!pid1_magic && runtime_busy_cnt > 0) {
		(void)launchd_assumes(kevent_mod((uintptr_t)&g_runtime_busy_time, EVFILT_TIMER, EV_DELETE, 0, 0, NULL) != -1);
	}
}

kern_return_t
catch_mach_exception_raise(mach_port_t exception_port __attribute__((unused)), mach_port_t thread, mach_port_t task,
		exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCnt)
{
	pid_t p4t = -1;

	(void)launchd_assumes(pid_for_task(task, &p4t) == 0);
	
	runtime_syslog(LOG_NOTICE, "%s(): PID: %u thread: 0x%x type: 0x%x code: %p codeCnt: 0x%x",
			__func__, p4t, thread, exception, code, codeCnt);
	
	(void)launchd_assumes(launchd_mport_deallocate(thread) == KERN_SUCCESS);
	(void)launchd_assumes(launchd_mport_deallocate(task) == KERN_SUCCESS);

	return KERN_SUCCESS;
}

kern_return_t
catch_mach_exception_raise_state(mach_port_t exception_port __attribute__((unused)),
		exception_type_t exception, const mach_exception_data_t code, mach_msg_type_number_t codeCnt,
		int *flavor, const thread_state_t old_state, mach_msg_type_number_t old_stateCnt,
		thread_state_t new_state, mach_msg_type_number_t *new_stateCnt)
{
	runtime_syslog(LOG_NOTICE, "%s(): type: 0x%x code: %p codeCnt: 0x%x flavor: %p old_state: %p old_stateCnt: 0x%x new_state: %p new_stateCnt: %p",
			__func__, exception, code, codeCnt, flavor, old_state, old_stateCnt, new_state, new_stateCnt);
	
	memcpy(new_state, old_state, old_stateCnt * sizeof(old_state[0]));
	*new_stateCnt = old_stateCnt;

	return KERN_SUCCESS;
}

kern_return_t
catch_mach_exception_raise_state_identity(mach_port_t exception_port __attribute__((unused)), mach_port_t thread, mach_port_t task,
		exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCnt,
		int *flavor, thread_state_t old_state, mach_msg_type_number_t old_stateCnt,
		thread_state_t new_state, mach_msg_type_number_t *new_stateCnt)
{
	pid_t p4t = -1;

	(void)launchd_assumes(pid_for_task(task, &p4t) == 0);

	runtime_syslog(LOG_NOTICE, "%s(): PID: %u thread: 0x%x type: 0x%x code: %p codeCnt: 0x%x flavor: %p old_state: %p old_stateCnt: 0x%x new_state: %p new_stateCnt: %p",
			__func__, p4t, thread, exception, code, codeCnt, flavor, old_state, old_stateCnt, new_state, new_stateCnt);
	
	memcpy(new_state, old_state, old_stateCnt * sizeof(old_state[0]));
	*new_stateCnt = old_stateCnt;

	(void)launchd_assumes(launchd_mport_deallocate(thread) == KERN_SUCCESS);
	(void)launchd_assumes(launchd_mport_deallocate(task) == KERN_SUCCESS);

	return KERN_SUCCESS;
}

void
launchd_log_vm_stats(void)
{
	static struct vm_statistics orig_stats;
	static bool did_first_pass;
	unsigned int count = HOST_VM_INFO_COUNT;
	struct vm_statistics stats, *statsp;
	mach_port_t mhs = mach_host_self();

	statsp = did_first_pass ? &stats : &orig_stats;

	if (!launchd_assumes(host_statistics(mhs, HOST_VM_INFO, (host_info_t)statsp, &count) == KERN_SUCCESS)) {
		return;
	}

	(void)launchd_assumes(count == HOST_VM_INFO_COUNT);

	if (did_first_pass) {
		runtime_syslog(LOG_DEBUG, "VM statistics (now - orig): Free: %d Active: %d Inactive: %d Reactivations: %d PageIns: %d PageOuts: %d Faults: %d COW-Faults: %d Purgeable: %d Purges: %d",
				stats.free_count - orig_stats.free_count,
				stats.active_count - orig_stats.active_count,
				stats.inactive_count - orig_stats.inactive_count,
				stats.reactivations - orig_stats.reactivations,
				stats.pageins - orig_stats.pageins,
				stats.pageouts - orig_stats.pageouts,
				stats.faults - orig_stats.faults,
				stats.cow_faults - orig_stats.cow_faults,
				stats.purgeable_count - orig_stats.purgeable_count,
				stats.purges - orig_stats.purges);
	} else {
		runtime_syslog(LOG_DEBUG, "VM statistics (now): Free: %d Active: %d Inactive: %d Reactivations: %d PageIns: %d PageOuts: %d Faults: %d COW-Faults: %d Purgeable: %d Purges: %d",
				orig_stats.free_count,
				orig_stats.active_count,
				orig_stats.inactive_count,
				orig_stats.reactivations,
				orig_stats.pageins,
				orig_stats.pageouts,
				orig_stats.faults,
				orig_stats.cow_faults,
				orig_stats.purgeable_count,
				orig_stats.purges);

		did_first_pass = true;
	}

	launchd_mport_deallocate(mhs);
}

int64_t
runtime_get_wall_time(void)
{
	struct timeval tv;
	int64_t r;

	(void)launchd_assumes(gettimeofday(&tv, NULL) != -1);

	r = tv.tv_sec;
	r *= USEC_PER_SEC;
	r += tv.tv_usec;

	return r;
}

uint64_t
runtime_get_opaque_time(void)
{
	return mach_absolute_time();
}

uint64_t
runtime_get_opaque_time_of_event(void)
{
	return time_of_mach_msg_return;
}

uint64_t
runtime_get_nanoseconds_since(uint64_t o)
{
	return runtime_opaque_time_to_nano(runtime_get_opaque_time_of_event() - o);
}

uint64_t
runtime_opaque_time_to_nano(uint64_t o)
{
#if defined(__i386__) || defined(__x86_64__)
	if (unlikely(tbi.numer != tbi.denom)) {
#elif defined(__ppc__) || defined(__ppc64__)
	if (likely(tbi.numer != tbi.denom)) {
#else
	if (tbi.numer != tbi.denom) {
#endif
#ifdef __LP64__
		__uint128_t tmp = o;
		tmp *= tbi.numer;
		tmp /= tbi.denom;
		o = tmp;
#else
		if (o <= tbi_safe_math_max) {
			o *= tbi.numer;
			o /= tbi.denom;
		} else {
			double d = o;
			d *= tbi_float_val;
			o = d;
		}
#endif
	}

	return o;
}

void
do_file_init(void)
{
	struct stat sb;

	launchd_assert(mach_timebase_info(&tbi) == 0);
	tbi_float_val = tbi.numer;
	tbi_float_val /= tbi.denom;
	tbi_safe_math_max = UINT64_MAX / tbi.numer;

	if (getpid() == 1) {
		pid1_magic = true;
	}

	if (stat("/AppleInternal", &sb) == 0 && stat("/var/db/disableAppleInternal", &sb) == -1) {
		do_apple_internal_logging = true;
	}

	if (stat("/var/db/.debug_launchd", &sb) == 0) {
		internal_mask_pri = LOG_UPTO(LOG_DEBUG);
		low_level_debug = true;
	}
	
	if (stat("/var/db/.launchd_log_per_user_shutdown", &sb) == 0) {
		g_log_per_user_shutdown = true;
	}
	
	if (stat("/var/db/.launchd_use_gmalloc", &sb) == 0) {
		g_use_gmalloc = true;
	}

	if (stat("/var/db/.launchd_malloc_log_stacks", &sb) == 0) {
		g_malloc_log_stacks = true;
		g_use_gmalloc = false;
	}
	
	if (pid1_magic && stat("/var/db/.launchd_log_pid1_shutdown", &sb) == 0) {
		g_log_pid1_shutdown = true;
	}
	
	char bootargs[128];
	size_t len = sizeof(bootargs) - 1;
	int r = pid1_magic ? sysctlbyname("kern.bootargs", bootargs, &len, NULL, 0) : -1;
	if (r == 0) {
		if (strnstr(bootargs, "-v", len)) {
			g_verbose_boot = true;
		}
		if (strnstr(bootargs, "launchd_trap_sigkill_bugs", len)) {
			g_trap_sigkill_bugs = true;
		}
	}
	
	if (pid1_magic && g_verbose_boot && stat("/var/db/.launchd_shutdown_debugging", &sb) == 0) {
		g_shutdown_debugging = true;
	}
	
	if (stat("/var/db/.launchd_log_strict_usage", &sb) == 0) {
		g_log_strict_usage = true;
	}
}
