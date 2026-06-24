# vim: set ss=4 ft= sw=4 et sts=4 ts=4:

use v5.10.1;
use Test::More;
use File::Temp qw(tempdir);
use Cwd qw(abs_path cwd);
use FindBin qw($Bin);

my $luajit = abs_path("$Bin/../src/luajit");

plan skip_all => "src/luajit is not built" unless defined $luajit && -x $luajit;

my $cc = $ENV{CC} || "cc";
system("$cc --version >/dev/null 2>&1");
plan skip_all => "C compiler '$cc' is not available" if $? == -1 || ($? >> 8) == 127;

my $cwd = cwd;
my $dir = tempdir "testlj_finalizer_nonres_cfunc_XXXXXXX", CLEANUP => 1;
chdir $dir or die "Cannot chdir to $dir: $!";

open my $c, '>', 'finonres.c' or die "Cannot open finonres.c: $!";
print $c <<'C';
#include "lua.h"
#include "lauxlib.h"

static int leaf_count;
static int nonzero_count;
static int closure_count;
static int resurrect_count;
static int stack_error_count;

static void check_udata_arg(lua_State *L)
{
  if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TUSERDATA)
    stack_error_count++;
  luaL_argcheck(L, lua_touserdata(L, 1) != NULL, 1, "userdata expected");
}

static int leaf_finalizer(lua_State *L)
{
  check_udata_arg(L);
  leaf_count++;
  return 0;
}

static int nonzero_finalizer(lua_State *L)
{
  check_udata_arg(L);
  nonzero_count++;
  return 1;
}

static int closure_finalizer(lua_State *L)
{
  check_udata_arg(L);
  luaL_checkany(L, lua_upvalueindex(1));
  closure_count++;
  return 0;
}

static int resurrect_finalizer(lua_State *L)
{
  check_udata_arg(L);
  lua_pushvalue(L, 1);
  lua_setfield(L, LUA_REGISTRYINDEX, "finonres.resurrected");
  resurrect_count++;
  return 0;
}

static int newproxy_with_gc(lua_State *L)
{
  luaL_checkany(L, 1);
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  return 1;
}

static int new_sized_with_gc(lua_State *L)
{
  size_t sz = (size_t)luaL_checkinteger(L, 1);
  luaL_checkany(L, 2);
  lua_newuserdata(L, sz);
  lua_newtable(L);
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  return 1;
}

static int has_resurrected(lua_State *L)
{
  int isud;
  lua_getfield(L, LUA_REGISTRYINDEX, "finonres.resurrected");
  isud = lua_type(L, -1) == LUA_TUSERDATA;
  lua_pop(L, 1);
  lua_pushboolean(L, isud);
  return 1;
}

static int clear_resurrected(lua_State *L)
{
  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "finonres.resurrected");
  return 0;
}

static int counters(lua_State *L)
{
  lua_pushinteger(L, leaf_count);
  lua_pushinteger(L, nonzero_count);
  lua_pushinteger(L, closure_count);
  lua_pushinteger(L, resurrect_count);
  lua_pushinteger(L, stack_error_count);
  return 5;
}

static int reset(lua_State *L)
{
  leaf_count = 0;
  nonzero_count = 0;
  closure_count = 0;
  resurrect_count = 0;
  stack_error_count = 0;
  clear_resurrected(L);
  return 0;
}

LUALIB_API int luaopen_finonres(lua_State *L)
{
  lua_newtable(L);
  lua_pushcfunction(L, leaf_finalizer);
  lua_setfield(L, -2, "leaf_finalizer");
  lua_pushcfunction(L, nonzero_finalizer);
  lua_setfield(L, -2, "nonzero_finalizer");
  lua_pushstring(L, "upvalue");
  lua_pushcclosure(L, closure_finalizer, 1);
  lua_setfield(L, -2, "closure_finalizer");
  lua_pushcfunction(L, resurrect_finalizer);
  lua_setfield(L, -2, "resurrect_finalizer");
  lua_pushcfunction(L, newproxy_with_gc);
  lua_setfield(L, -2, "newproxy_with_gc");
  lua_pushcfunction(L, new_sized_with_gc);
  lua_setfield(L, -2, "new_sized_with_gc");
  lua_pushcfunction(L, has_resurrected);
  lua_setfield(L, -2, "has_resurrected");
  lua_pushcfunction(L, clear_resurrected);
  lua_setfield(L, -2, "clear_resurrected");
  lua_pushcfunction(L, counters);
  lua_setfield(L, -2, "counters");
  lua_pushcfunction(L, reset);
  lua_setfield(L, -2, "reset");
  return 1;
}
C
close $c;

my @compile = ($cc, "-shared", "-fPIC", "-I$Bin/../src", "-o", "finonres.so", "finonres.c");
my $compile_out = `@compile 2>&1`;
my $compile_rc = $? >> 8;
if ($compile_rc != 0 && $compile_out =~ /not found|No such file|command not found/i) {
    plan skip_all => "C compiler '$cc' is not available";
}
die "Cannot compile finonres.so with '@compile':\n$compile_out" if $compile_rc != 0;

open my $probe, '>', 'closeprobe.c' or die "Cannot open closeprobe.c: $!";
print $probe <<'C';
#define LUA_CORE
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"

static int leaf_count;

static int leaf_finalizer(lua_State *L)
{
  luaL_argcheck(L, lua_touserdata(L, 1) != NULL, 1, "userdata expected");
  leaf_count++;
  return 0;
}

static int push_gcstats(lua_State *L, int reset)
{
  lua_getglobal(L, "jit");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  lua_getfield(L, -1, "gcstats");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return 0;
  }
  if (reset) {
    lua_pushboolean(L, 1);
    lua_call(L, 1, 1);
  } else {
    lua_call(L, 0, 1);
  }
  lua_remove(L, -2);  /* Remove jit table, leave stats table. */
  return 1;
}

static double gcstat(lua_State *L, const char *name)
{
  double n;
  if (!push_gcstats(L, 0))
    return -1.0;
  lua_getfield(L, -1, name);
  n = lua_tonumber(L, -1);
  lua_pop(L, 2);
  return n;
}

static int reset_gcstats(lua_State *L)
{
  if (!push_gcstats(L, 1))
    return 0;
  lua_pop(L, 1);
  return 1;
}

int main(void)
{
  lua_State *T = luaL_newstate();
  double before_nores, after_nores, before_direct, after_direct;
  double before_batches, after_batches;
  if (T == NULL)
    return 2;
  luaL_openlibs(T);
  if (!reset_gcstats(T)) {
    lua_close(T);
    return 77;
  }

  before_nores = gcstat(T, "finalizer_nonresurrecting_cfunc_frees");
  before_direct = gcstat(T, "finalizer_direct_cfunc_calls");
  before_batches = gcstat(T, "finalizer_direct_cfunc_batches");
  lua_newuserdata(T, 1);
  lua_newtable(T);
  lua_pushcfunction(T, leaf_finalizer);
  lua_setfield(T, -2, "__gc");
  lua_setmetatable(T, -2);
  lua_pop(T, 1);

  lj_gc_separateudata(G(T), 1);
  lj_gc_finalize_udata(T);

  after_nores = gcstat(T, "finalizer_nonresurrecting_cfunc_frees");
  after_direct = gcstat(T, "finalizer_direct_cfunc_calls");
  after_batches = gcstat(T, "finalizer_direct_cfunc_batches");
  lua_close(T);

  if (leaf_count != 1) {
    fprintf(stderr, "leaf_count=%d\n", leaf_count);
    return 3;
  }
  if (after_nores != before_nores) {
    fprintf(stderr, "nores %.0f -> %.0f\n", before_nores, after_nores);
    return 4;
  }
  if (after_direct < before_direct + 1) {
    fprintf(stderr, "direct %.0f -> %.0f\n", before_direct, after_direct);
    return 5;
  }
  if (after_batches != before_batches) {
    fprintf(stderr, "batches %.0f -> %.0f\n", before_batches, after_batches);
    return 6;
  }
  puts("ok");
  return 0;
}
C
close $probe;

my @probe_compile = ($cc, "-I$Bin/../src", "-o", "closeprobe",
                     "closeprobe.c", "$Bin/../src/libluajit.a", "-lm", "-ldl");
my $probe_compile_out = `@probe_compile 2>&1`;
my $probe_compile_rc = $? >> 8;
die "Cannot compile closeprobe with '@probe_compile':\n$probe_compile_out"
    if $probe_compile_rc != 0;

plan tests => 1;

$ENV{LUA_CPATH} = "$dir/?.so;$Bin/../?.so;;";
$ENV{LUA_PATH} = "$Bin/../lua/?.lua;;";

open my $lua, '>', 'test.lua' or die "Cannot open test.lua: $!";
print $lua <<'LUA';
if jit then jit.off() end
local m = require "finonres"

local stats = jit and jit.gcstats and jit.gcstats()
local has_nores_stats = stats and stats.finalizer_nonresurrecting_cfunc_frees ~= nil
local has_direct = stats and stats.finalizer_direct_cfunc_calls ~= nil

local function stat(name)
  return jit.gcstats()[name]
end

local function force_until(label, pred)
  for _ = 1, 32 do
    collectgarbage("collect")
    if pred() then return end
  end
  error(label)
end

local function force_stat(name, before, delta)
  force_until(name, function()
    return stat(name) >= before + delta
  end)
end

local function check_resurrecting_c_finalizer_uses_generic_path()
  m.reset()
  collectgarbage("collect")
  if stats then jit.gcstats(true) end
  local before_direct = has_direct and stat("finalizer_direct_cfunc_calls") or nil
  do
    local u = m.newproxy_with_gc(m.resurrect_finalizer)
    u = nil
  end
  force_until("C finalizer did not resurrect", function() return m.has_resurrected() end)
  local leaf, nonzero, closure, resurrect, stack_errors = m.counters()
  assert(leaf == 0 and nonzero == 0 and closure == 0, leaf .. ":" .. nonzero .. ":" .. closure)
  assert(resurrect == 1, resurrect)
  assert(stack_errors == 0, stack_errors)
  if before_direct then
    assert(stat("finalizer_direct_cfunc_calls") >= before_direct + 1)
  end
  m.clear_resurrected()
  for _ = 1, 4 do collectgarbage("collect") end
  leaf, nonzero, closure, resurrect, stack_errors = m.counters()
  assert(resurrect == 1, resurrect)
  assert(stack_errors == 0, stack_errors)
end

local function check_leaf_lifetime_without_stats_dependency()
  m.reset()
  collectgarbage("collect")
  local before = collectgarbage("count")
  local payload = 4 * 1024 * 1024
  do
    local u = m.new_sized_with_gc(payload, m.leaf_finalizer)
    u = nil
  end
  local allocated = collectgarbage("count") - before
  assert(allocated > (payload / 1024) * 0.75, allocated)
  collectgarbage("collect")
  local after_first = collectgarbage("count")
  local leaf, nonzero, closure, resurrect, stack_errors = m.counters()
  assert(leaf == 1 and nonzero == 0 and closure == 0 and resurrect == 0)
  assert(stack_errors == 0, stack_errors)
  collectgarbage("collect")
  local after_second = collectgarbage("count")
  local retained_limit = before + allocated * 0.50
  local immediate_free = after_first < retained_limit
  assert(after_second < retained_limit, after_second .. " >= " .. retained_limit)
  return immediate_free
end

local function check_nores_direct_free_and_fallbacks()
  assert(has_nores_stats)
  m.reset()
  collectgarbage("collect")
  jit.gcstats(true)

  do
    local before_free = stat("finalizer_nonresurrecting_cfunc_frees")
    local before_direct = stat("finalizer_direct_cfunc_calls")
    local before_calls = stat("finalizer_calls")
    local u = m.newproxy_with_gc(m.leaf_finalizer)
    u = nil
    force_stat("finalizer_nonresurrecting_cfunc_frees", before_free, 1)
    assert(stat("finalizer_direct_cfunc_calls") >= before_direct + 1)
    assert(stat("finalizer_calls") >= before_calls + 1)
    local leaf, nonzero, closure, resurrect, stack_errors = m.counters()
    assert(leaf == 1 and nonzero == 0 and closure == 0 and resurrect == 0)
    assert(stack_errors == 0, stack_errors)
  end

  do
    local before_free = stat("finalizer_nonresurrecting_cfunc_frees")
    local before_nonzero = stat("finalizer_direct_cfunc_nonzero_results")
    local u = m.newproxy_with_gc(m.nonzero_finalizer)
    u = nil
    force_stat("finalizer_nonresurrecting_cfunc_frees", before_free, 1)
    assert(stat("finalizer_direct_cfunc_nonzero_results") >= before_nonzero + 1)
    local leaf, nonzero, closure, resurrect, stack_errors = m.counters()
    assert(leaf == 1 and nonzero == 1 and closure == 0 and resurrect == 0)
    assert(stack_errors == 0, stack_errors)
  end

  do
    local before_free = stat("finalizer_nonresurrecting_cfunc_frees")
    local before_fallback = stat("finalizer_nonresurrecting_cfunc_fallbacks")
    local before_upvalue = stat("finalizer_cfunc_upvalue_calls")
    local u = m.newproxy_with_gc(m.closure_finalizer)
    u = nil
    force_stat("finalizer_cfunc_upvalue_calls", before_upvalue, 1)
    assert(stat("finalizer_nonresurrecting_cfunc_frees") == before_free)
    assert(stat("finalizer_nonresurrecting_cfunc_fallbacks") >= before_fallback + 1)
    local leaf, nonzero, closure, resurrect, stack_errors = m.counters()
    assert(leaf == 1 and nonzero == 1 and closure == 1 and resurrect == 0)
    assert(stack_errors == 0, stack_errors)
  end
end

local function check_close_drain_uses_generic_path()
  local a, b, c = os.execute("./closeprobe >closeprobe.out 2>closeprobe.err")
  if not (a == 0 or a == true) then
    local err = io.open("closeprobe.err", "r"):read("*a")
    error("closeprobe failed: " .. tostring(a) .. ":" .. tostring(b) .. ":" ..
          tostring(c) .. ":" .. err)
  end
  local out = io.open("closeprobe.out", "r"):read("*a")
  assert(out == "ok\n", out)
end

local immediate_leaf_free = check_leaf_lifetime_without_stats_dependency()
assert(immediate_leaf_free, "non-resurrecting C finalizer did not free after first finalizer cycle")
if has_nores_stats then
  check_close_drain_uses_generic_path()
  check_nores_direct_free_and_fallbacks()
end
check_resurrecting_c_finalizer_uses_generic_path()

print("ok")
LUA
close $lua;

system qq{"$luajit" test.lua >stdout.txt 2>stderr.txt};
my $rc = $? >> 8;

open my $outfh, '<', 'stdout.txt' or die "Cannot open stdout.txt: $!";
my $out = do { local $/; <$outfh> };
close $outfh;

open my $errfh, '<', 'stderr.txt' or die "Cannot open stderr.txt: $!";
my $err = do { local $/; <$errfh> };
close $errfh;

chdir $cwd or die $!;

is "$rc:$out$err", "0:ok\n", 'non-resurrecting direct-free C finalizer contract is guarded';
