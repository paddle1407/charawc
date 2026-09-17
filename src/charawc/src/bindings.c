#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <xkbcommon/xkbcommon.h>

#include "config.h"

static struct wl_list bindings = { &bindings, &bindings };

void
chara_argv_free(char **argv)
{
	if (!argv)
		return;
	for (size_t i = 0; argv[i]; ++i)
		free(argv[i]);
	free(argv);
}

/* Bindings are freed from three places: a replaced binding, the installed
 * set at shutdown, and a configuration that was never installed. All three
 * own the same fields, so they release them the same way. */
void
chara_binding_free(struct binding *binding)
{
	if (!binding)
		return;
	chara_argv_free(binding->argv);
	free(binding->action.selector);
	free(binding);
}

char **
chara_argv_copy(char *const *argv)
{
	size_t n = 0;
	while (argv[n])
		++n;
	char **copy = calloc(n + 1, sizeof(*copy));
	if (!copy)
		return NULL;
	for (size_t i = 0; i < n; ++i) {
		if (!(copy[i] = strdup(argv[i]))) {
			chara_argv_free(copy);
			return NULL;
		}
	}
	return copy;
}

pid_t
chara_spawn_process(char *const *argv, bool stop_on_exit)
{
	pid_t parent = getpid();
	pid_t pid = fork();
	if (pid < 0) {
		_wrn("couldn't launch %s: %s", argv[0], strerror(errno));
		return -1;
	}
	if (pid != 0)
		return pid;
	if (setsid() < 0)
		_exit(126);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	sigset_t mask;
	sigemptyset(&mask);
	sigprocmask(SIG_SETMASK, &mask, NULL);
#ifdef __linux__
	/* Also terminate direct managed children if charaWC crashes. Normal logout
	 * explicitly stops their process groups and waits for them. */
	if (stop_on_exit &&
	    (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent))
		_exit(126);
#else
	(void)parent;
	(void)stop_on_exit;
#endif
	execvp(argv[0], argv);
	dprintf(STDERR_FILENO, "charawc: couldn't execute %s: %s\n", argv[0], strerror(errno));
	_exit(127);
}

void
chara_spawn(char *const *argv)
{
	(void)chara_spawn_process(argv, false);
}

bool
chara_parse_key(const char *text, uint32_t mod, uint32_t *mods, uint32_t *key,
               bool modifiers_only)
{
	char *copy = strdup(text), *save = NULL, *token;
	bool ok = false;
	*mods = *key = 0;
	if (!copy || !*copy || *copy == '+' || copy[strlen(copy) - 1] == '+' ||
	    strstr(copy, "++"))
		goto done;
	for (token = strtok_r(copy, "+", &save); token;
	     token = strtok_r(NULL, "+", &save)) {
		uint32_t bit = 0;
		if (!strcmp(token, "mod") && !modifiers_only) bit = mod;
		else if (!strcmp(token, "logo") || !strcmp(token, "win") || !strcmp(token, "super")) bit = SWC_MOD_LOGO;
		else if (!strcmp(token, "alt")) bit = SWC_MOD_ALT;
		else if (!strcmp(token, "ctrl")) bit = SWC_MOD_CTRL;
		else if (!strcmp(token, "shift")) bit = SWC_MOD_SHIFT;
		else if (!strcmp(token, "any") && !modifiers_only) bit = SWC_MOD_ANY;
		else {
			if (modifiers_only || *key ||
			    !(*key = xkb_keysym_from_name(token, XKB_KEYSYM_CASE_INSENSITIVE)))
				goto done;
			continue;
		}
		if ((*mods & bit) || (*mods == (uint32_t)SWC_MOD_ANY) ||
		    (bit == (uint32_t)SWC_MOD_ANY && *mods))
			goto done;
		*mods |= bit;
	}
	ok = modifiers_only ? *mods != 0 : *key != 0;
done:
	free(copy);
	return ok;
}

static void
binding_handler(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	struct binding *binding = data;
	if (binding->argv)
		chara_spawn(binding->argv);
	else
		chara_action_run(&binding->action);
}

bool
chara_binding_remove(uint32_t mods, uint32_t key)
{
	struct binding *binding;
	wl_list_for_each(binding, &bindings, link) {
		if (binding->modifiers != mods || binding->key != key)
			continue;
		swc_remove_binding(SWC_BINDING_KEY, mods, key);
		wl_list_remove(&binding->link);
		chara_binding_free(binding);
		return true;
	}
	return false;
}

bool
chara_binding_install(struct binding *binding)
{
	/* Allocate the swc entry first, preserving the old binding on failure. */
	if (swc_add_binding(SWC_BINDING_KEY, binding->modifiers, binding->key,
	                    binding_handler, binding) < 0)
		return false;
	chara_binding_remove(binding->modifiers, binding->key);
	wl_list_insert(bindings.prev, &binding->link);
	return true;
}

void
chara_bindings_finish(void)
{
	struct binding *binding, *tmp;
	wl_list_for_each_safe(binding, tmp, &bindings, link) {
		wl_list_remove(&binding->link);
		chara_binding_free(binding);
	}
}

bool
chara_bindings_prepare(struct config *cfg, struct swc_binding_batch *batch)
{
	struct binding *b;
	wl_list_for_each(b, &cfg->bindings, link)
		if (!swc_binding_batch_add(batch, SWC_BINDING_KEY, b->modifiers, b->key,
		                           binding_handler, b)) return false;
	return true;
}

void
chara_bindings_replace(struct config *cfg)
{
	struct binding *b, *tmp;
	wl_list_for_each_safe(b, tmp, &bindings, link)
		chara_binding_remove(b->modifiers, b->key);
	wl_list_for_each_safe(b, tmp, &cfg->bindings, link) {
		wl_list_remove(&b->link);
		wl_list_insert(bindings.prev, &b->link);
	}
}
