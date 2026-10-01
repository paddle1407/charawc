#ifndef CHARA_CONFIG_LUA_H
#define CHARA_CONFIG_LUA_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <lua.h>
#include <lauxlib.h>

/* Shared by the compositor and standalone bar configuration readers. */
struct config_lua_budget {
	size_t bytes;
	unsigned instructions;
	struct timespec start;
	bool exhausted;
};

static void *config_lua_alloc(void *data, void *ptr, size_t old_size, size_t size)
{
	struct config_lua_budget *budget = data;
	if (!ptr) old_size = 0; /* Lua uses old_size as a type tag for new objects. */
	if (!size) {
		free(ptr);
		budget->bytes -= old_size;
		return NULL;
	}
	if (size > 64 * 1024 * 1024 - (budget->bytes - old_size)) return NULL;
	void *next = realloc(ptr, size);
	if (next) budget->bytes = budget->bytes - old_size + size;
	return next;
}

static void config_lua_check_budget(lua_State *L)
{
	void *data;
	struct timespec now;
	lua_getallocf(L, &data);
	struct config_lua_budget *budget = data;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t elapsed = (now.tv_sec - budget->start.tv_sec) * INT64_C(1000000000) +
	                  now.tv_nsec - budget->start.tv_nsec;
	if (budget->exhausted || budget->instructions >= 1000000 || elapsed > 250000000) {
		budget->exhausted = true;
		luaL_error(L, "configuration exceeded its execution limit");
	}
}

static void config_lua_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	void *data;
	lua_getallocf(L, &data);
	struct config_lua_budget *budget = data;
	if (!budget->exhausted) budget->instructions += 1000;
	config_lua_check_budget(L);
}

/* Protected calls may catch ordinary Lua errors, but must propagate budget
 * cancellation through every enclosing protected call. Do not enter a user
 * error handler after cancellation: Lua can disable hooks while handling an
 * error from a hook, so that handler could otherwise loop indefinitely. */
static int config_lua_error_handler(lua_State *L)
{
	config_lua_check_budget(L);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, 1, 1);
	config_lua_check_budget(L);
	return 1;
}

static int config_lua_pcall(lua_State *L)
{
	config_lua_check_budget(L);
	luaL_checkany(L, 1);
	int result = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
	config_lua_check_budget(L);
	lua_pushboolean(L, result == LUA_OK);
	lua_insert(L, 1);
	return lua_gettop(L);
}

static int config_lua_xpcall(lua_State *L)
{
	config_lua_check_budget(L);
	luaL_checkany(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	int nargs = lua_gettop(L) - 2;
	lua_pushvalue(L, 2);
	lua_pushcclosure(L, config_lua_error_handler, 1);
	lua_remove(L, 2);
	lua_insert(L, 1);
	int result = lua_pcall(L, nargs, LUA_MULTRET, 1);
	lua_remove(L, 1);
	config_lua_check_budget(L);
	lua_pushboolean(L, result == LUA_OK);
	lua_insert(L, 1);
	return lua_gettop(L);
}

/* Call after opening the base library, before evaluating any user code. */
static void config_lua_protect_calls(lua_State *L)
{
	lua_pushcfunction(L, config_lua_pcall);
	lua_setglobal(L, "pcall");
	lua_pushcfunction(L, config_lua_xpcall);
	lua_setglobal(L, "xpcall");
}

#endif
