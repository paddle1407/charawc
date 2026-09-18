#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

/* Only startup commands are owned here. Ordinary keybinding children are
 * reaped, but are never treated as session services. */
struct process {
	struct wl_list link;
	struct startup_command *command;
	pid_t pid;
	uint64_t deadline;
};

static struct wl_list pending, processes;
static struct process *waiting;
static struct wl_event_source *timer, *child_signal;
static bool initialized, stopping;

static uint64_t
now_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void
discard_pending(void)
{
	struct startup_command *c, *tmp;
	wl_list_for_each_safe(c, tmp, &pending, link) {
		wl_list_remove(&c->link);
		chara_startup_command_free(c);
	}
}

static void
signal_process(struct process *p, int sig)
{
	/* The direct child is still ours (possibly a zombie), so its PID cannot
	 * be reused here. Include descendants in its session/process group. */
	if (kill(-p->pid, sig) < 0 && errno == ESRCH)
		kill(p->pid, sig); /* Child may not have reached setsid() yet. */
}

static bool
owned(struct process *p)
{
	return p->command->stop_on_exit || p->command->wait;
}

static void
fail_startup(const char *reason)
{
	_wrn("startup: %s; remaining startup commands skipped", reason);
	if (waiting)
		signal_process(waiting, SIGTERM);
	waiting = NULL;
	discard_pending();
}

static int
reap_children(int signal_number, void *data)
{
	for (;;) {
		siginfo_t info = {0};
		/* Keep the zombie until after signalling its group: this pins the
		 * leader's PID and prevents signalling a reused, unrelated group. */
		if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (!info.si_pid) break;
		struct process *p, *found = NULL;
		wl_list_for_each(p, &processes, link) {
			if (p->pid == info.si_pid) { found = p; break; }
		}
		/* Successful one-shot commands may intentionally launch applications;
		 * only service ownership, failure, or cancellation owns descendants. */
		if (found && (found->command->stop_on_exit ||
		    (found->command->wait && (stopping || info.si_code != CLD_EXITED || info.si_status != 0))))
			signal_process(found, SIGKILL);
		int exit_status;
		pid_t result;
		do result = waitpid(info.si_pid, &exit_status, 0); while (result < 0 && errno == EINTR);
		if (result < 0) break;
		chara_child_exited(result);
		if (!found) continue;
		if (waiting == found) {
			waiting = NULL;
			if (found->command->ready_socket || !WIFEXITED(exit_status) || WEXITSTATUS(exit_status)) {
				_wrn("startup: %s exited before successful readiness/completion (status %d)",
				     found->command->argv[0], WIFEXITED(exit_status) ? WEXITSTATUS(exit_status) : 128 + WTERMSIG(exit_status));
				fail_startup("prerequisite failed");
			}
		} else if (!stopping && found->command->stop_on_exit) {
			_wrn("startup: service %s exited (status %d)", found->command->argv[0],
			     WIFEXITED(exit_status) ? WEXITSTATUS(exit_status) : 128 + WTERMSIG(exit_status));
		}
		wl_list_remove(&found->link);
		chara_startup_command_free(found->command);
		free(found);
	}
	if (!stopping && !waiting && !wl_list_empty(&pending))
		wl_event_source_timer_update(timer, 1);
	return 0;
}

static bool
socket_ready(const char *path)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (*path != '/' && (!runtime || !*runtime)) return false;
	int length = *path == '/' ? snprintf(address.sun_path, sizeof(address.sun_path), "%s", path)
	    : snprintf(address.sun_path, sizeof(address.sun_path), "%s/%s", runtime, path);
	if (length < 0 || (size_t)length >= sizeof(address.sun_path)) return false;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return false;
	bool ready = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
	close(fd);
	return ready;
}

static int
advance(void *data)
{
	reap_children(0, NULL);
	if (waiting) {
		if (waiting->command->ready_socket && socket_ready(waiting->command->ready_socket)) {
			_inf("startup: %s ready", waiting->command->argv[0]);
			waiting = NULL;
		} else if (now_ms() >= waiting->deadline) {
			_wrn("startup: timed out waiting for %s", waiting->command->argv[0]);
			/* A timed-out prerequisite must not leak if it ignores TERM. */
			signal_process(waiting, SIGKILL);
			fail_startup("prerequisite timeout");
			return 0;
		}
	}
	while (!waiting && !wl_list_empty(&pending)) {
		struct startup_command *c = wl_container_of(pending.next, c, link);
		struct process *p = calloc(1, sizeof(*p));
		if (!p) { fail_startup("out of memory"); return 0; }
		/* Do not adopt another login's service/socket. Starting a second
		 * PipeWire against that socket would otherwise look ready briefly. */
		if (c->ready_socket && socket_ready(c->ready_socket)) {
			_wrn("startup: socket %s is already served; refusing to start %s", c->ready_socket, c->argv[0]);
			free(p);
			fail_startup("service already running outside this startup");
			return 0;
		}
		p->pid = chara_spawn_process(c->argv, c->stop_on_exit || c->wait);
		if (p->pid < 0) { free(p); fail_startup("could not fork"); return 0; }
		wl_list_remove(&c->link);
		p->command = c;
		p->deadline = now_ms() + c->timeout_ms;
		wl_list_insert(processes.prev, &p->link);
		if (c->wait || c->ready_socket) waiting = p;
	}
	if (waiting) wl_event_source_timer_update(timer, 25);
	return 0;
}

bool
chara_startup_init(struct wl_event_loop *loop)
{
	wl_list_init(&pending);
	wl_list_init(&processes);
	stopping = false;
	child_signal = wl_event_loop_add_signal(loop, SIGCHLD, reap_children, NULL);
	timer = wl_event_loop_add_timer(loop, advance, NULL);
	if (!child_signal || !timer) {
		if (child_signal) wl_event_source_remove(child_signal);
		if (timer) wl_event_source_remove(timer);
		child_signal = timer = NULL;
		return false;
	}
	initialized = true;
	/* Account for children that exited before SIGCHLD was registered. */
	reap_children(0, NULL);
	return true;
}

void
chara_startup_run(struct config *cfg)
{
	/* Transfer ownership: a reload can free its own config independently of
	 * startup readiness and the lifetime of session services. */
	struct wl_list *lists[] = { &cfg->exec_once, &cfg->exec };
	for (size_t i = 0; i < 2; ++i) {
		while (!wl_list_empty(lists[i])) {
			struct startup_command *c = wl_container_of(lists[i]->next, c, link);
			wl_list_remove(&c->link);
			wl_list_insert(pending.prev, &c->link);
		}
	}
	wl_event_source_timer_update(timer, 1);
}

void
chara_startup_finish(void)
{
	if (!initialized) return;
	stopping = true;
	waiting = NULL;
	discard_pending();
	wl_event_source_remove(timer);
	wl_event_source_remove(child_signal);
	timer = child_signal = NULL;
	struct process *p, *tmp;
	wl_list_for_each(p, &processes, link) {
		if (owned(p)) signal_process(p, SIGTERM);
	}
	uint64_t deadline = now_ms() + 2000;
	for (;;) {
		reap_children(0, NULL);
		bool remaining = false;
		wl_list_for_each(p, &processes, link) {
			if (owned(p)) remaining = true;
		}
		if (!remaining || now_ms() >= deadline) break;
		struct timespec delay = { .tv_nsec = 10000000 };
		nanosleep(&delay, NULL);
	}
	wl_list_for_each_safe(p, tmp, &processes, link) {
		if (owned(p)) signal_process(p, SIGKILL);
		wl_list_remove(&p->link);
		chara_startup_command_free(p->command);
		free(p);
	}
	initialized = false;
}
