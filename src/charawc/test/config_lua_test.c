#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <lualib.h>
#include "config_lua.h"

static void run(const char *source, bool cancelled)
{
	struct config_lua_budget budget = {0};
	clock_gettime(CLOCK_MONOTONIC, &budget.start);
	lua_State *L = lua_newstate(config_lua_alloc, &budget);
	assert(L);
	luaL_requiref(L, "_G", luaopen_base, 1);
	lua_pop(L, 1);
	config_lua_protect_calls(L);
	lua_sethook(L, config_lua_hook, LUA_MASKCOUNT, 1000);
	/* A broken cancellation implementation must not hang the test suite. */
	alarm(2);
	int status = luaL_loadbufferx(L, source, strlen(source), "budget test", "t");
	if (status == LUA_OK) status = lua_pcall(L, 0, 0, 0);
	alarm(0);
	if (cancelled) {
		assert(status != LUA_OK && budget.exhausted);
		assert(strstr(lua_tostring(L, -1), "execution limit"));
	} else {
		if (status != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
		assert(status == LUA_OK && !budget.exhausted);
	}
	lua_close(L);
}

int main(void)
{
	run("local ok,a,b,c = pcall(function(x) return x,nil,3 end,2); "
	    "assert(ok and a==2 and b==nil and c==3); "
	    "local marker={}; local ok,e=pcall(function() error(marker) end); "
	    "assert(not ok and e==marker); "
	    "local ok,a,b=xpcall(function(x) return x,4 end,function(e) return e end,3); "
	    "assert(ok and a==3 and b==4); "
	    "local ok,e=xpcall(function() error(marker) end,function(e) "
	    "assert(e==marker); return 7 end); assert(not ok and e==7); "
	    "local ok,e=xpcall(function() error(marker) end,function() error('handler') end); "
	    "assert(not ok and e~=nil)", false);
	run("while true do end", true);
	run("while true do pcall(function() while true do end end) end", true);
	run("pcall(function() while true do pcall(function() while true do end end) end end)", true);
	run("xpcall(function() while true do end end,function() while true do end end)", true);
	run("pcall(function() xpcall(function() while true do end end, "
	    "function() return 'caught' end) end)", true);
	run("xpcall(function() error('ordinary') end,function() "
	    "while true do pcall(function() while true do end end) end end)", true);
	puts("Lua config: protected-call semantics and uncatchable cancellation passed");
	return 0;
}
