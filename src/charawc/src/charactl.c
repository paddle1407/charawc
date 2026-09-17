/* charactl: send one command to a running charaWC and print the reply. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Must match chara_socket_path() in charawc.c. */
static const char *
socket_path(void)
{
	static char path[108];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	const char *override = getenv("CHARAWC_SOCKET");

	if (override && *override)
		return override;
	if (runtime && *runtime == '/')
		snprintf(path, sizeof(path), "%s/charawc-%s.sock", runtime,
		         display ? display : "0");
	else
		snprintf(path, sizeof(path), "/tmp/charawc-%d.sock", (int)getuid());
	return path;
}

static void
usage(FILE *out, const char *name)
{
	fprintf(out,
	    "usage: %s <command> [arguments]\n\n"
	    "Commands that act on a window take a selector first: a name from a\n"
	    "rule (term, term:2), an automatic id (#4), or 'focused'.\n\n"
	    "  %s focus term\n"
	    "  %s move #4 40 0\n"
	    "  %s maximize term\n"
	    "  %s list_windows\n\n"
	    "See CONFIG.md for the full command list.\n",
	    name, name, name, name, name);
}

int
main(int argc, char **argv)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	const char *path = socket_path();
	char request[4096];
	size_t used = 0;

	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(argc < 2 ? stderr : stdout, argv[0]);
		return argc < 2 ? 1 : 0;
	}
	for (int i = 1; i < argc; ++i) {
		int n = snprintf(request + used, sizeof(request) - used, "%s%s",
		                 i > 1 ? " " : "", argv[i]);
		if (n < 0 || (size_t)n >= sizeof(request) - used) {
			fprintf(stderr, "charactl: command is too long\n");
			return 1;
		}
		used += (size_t)n;
	}
	request[used++] = '\n';

	if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", path) >=
	    (int)sizeof(address.sun_path)) {
		fprintf(stderr, "charactl: socket path is too long: %s\n", path);
		return 1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "charactl: %s\n", strerror(errno));
		return 1;
	}
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		fprintf(stderr, "charactl: %s: %s\n", path, strerror(errno));
		close(fd);
		return 1;
	}
	for (size_t sent = 0; sent < used;) {
		ssize_t n = write(fd, request + sent, used - sent);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			fprintf(stderr, "charactl: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		sent += (size_t)n;
	}
	shutdown(fd, SHUT_WR);

	char reply[8192];
	size_t got = 0;
	while (got < sizeof(reply) - 1) {
		ssize_t n = read(fd, reply + got, sizeof(reply) - 1 - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	reply[got] = '\0';
	if (reply[got ? got - 1 : 0] == '\n')
		reply[got - 1] = '\0';

	if (!strncmp(reply, "ok", 2)) {
		if (reply[2])
			printf("%s\n", reply + 3);
		return 0;
	}
	if (!strncmp(reply, "error", 5)) {
		fprintf(stderr, "charactl: %s\n", reply[5] ? reply + 6 : "failed");
		return 1;
	}
	fprintf(stderr, "charactl: no reply from charaWC\n");
	return 1;
}
