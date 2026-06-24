# vim: set ss=4 ft= sw=4 et sts=4 ts=4:

use v5.10.1;
use Test::More;
use File::Temp qw(tempdir);
use Cwd qw(abs_path cwd);
use FindBin qw($Bin);

my $luajit = abs_path("$Bin/../src/luajit");

plan skip_all => "src/luajit is not built" unless defined $luajit && -x $luajit;

system $luajit, '-e', 'if not (jit and jit.gcstats) then os.exit(77) end; local s = jit.gcstats(); if s.finalizer_direct_cfunc_batches == nil then os.exit(78) end';
plan skip_all => 'LuaJIT built without LUAJIT_ENABLE_GCSTATS'
    if (($? >> 8) == 77);
plan skip_all => 'GCStats build does not expose batched finalizer telemetry'
    if (($? >> 8) == 78);

my $cc = $ENV{CC} || "cc";
system("$cc --version >/dev/null 2>&1");
plan skip_all => "C compiler '$cc' is not available" if $? == -1 || ($? >> 8) == 127;

my $cwd = cwd;
my $dir = tempdir "testlj_finalizer_batched_cfunc_XXXXXXX", CLEANUP => 1;
chdir $dir or die "Cannot chdir to $dir: $!";

open my $c, '>', 'fibatch.c' or die "Cannot open fibatch.c: $!";
print $c <<'C';
#include "lua.h"
#include "lauxlib.h"

static int leaf_count;
static int closure_count;
static int nonzero_count;
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

static int closure_finalizer(lua_State *L)
{
  check_udata_arg(L);
  luaL_checkany(L, lua_upvalueindex(1));
  closure_count++;
  return 0;
}

static int nonzero_finalizer(lua_State *L)
{
  check_udata_arg(L);
  nonzero_count++;
  return 1;
}

static void push_eligible(lua_State *L)
{
  lua_newuserdata(L, 1);
  luaL_getmetatable(L, "fibatch.eligible");
  lua_setmetatable(L, -2);
}

static void push_ineligible(lua_State *L)
{
  lua_newuserdata(L, 1);
  luaL_getmetatable(L, "fibatch.ineligible");
  lua_setmetatable(L, -2);
}

static void push_nonzero(lua_State *L)
{
  lua_newuserdata(L, 1);
  luaL_getmetatable(L, "fibatch.nonzero");
  lua_setmetatable(L, -2);
}

static int alloc_eligible(lua_State *L)
{
  int i, n = luaL_checkint(L, 1);
  luaL_argcheck(L, n >= 0, 1, "non-negative count expected");
  for (i = 0; i < n; i++) {
    push_eligible(L);
    lua_pop(L, 1);
  }
  return 0;
}

static int alloc_mixed(lua_State *L)
{
  int i, eligible = luaL_checkint(L, 1);
  int ineligible = luaL_checkint(L, 2);
  int n = eligible > ineligible ? eligible : ineligible;
  luaL_argcheck(L, eligible >= 0, 1, "non-negative count expected");
  luaL_argcheck(L, ineligible >= 0, 2, "non-negative count expected");
  for (i = 0; i < n; i++) {
    if (i < eligible) {
      push_eligible(L);
      lua_pop(L, 1);
    }
    if (i < ineligible) {
      push_ineligible(L);
      lua_pop(L, 1);
    }
  }
  return 0;
}

static int alloc_nonzero(lua_State *L)
{
  int i, n = luaL_checkint(L, 1);
  luaL_argcheck(L, n >= 0, 1, "non-negative count expected");
  for (i = 0; i < n; i++) {
    push_nonzero(L);
    lua_pop(L, 1);
  }
  return 0;
}

static int counters(lua_State *L)
{
  lua_pushinteger(L, leaf_count);
  lua_pushinteger(L, closure_count);
  lua_pushinteger(L, stack_error_count);
  lua_pushinteger(L, nonzero_count);
  return 4;
}

static int reset(lua_State *L)
{
  leaf_count = 0;
  closure_count = 0;
  nonzero_count = 0;
  stack_error_count = 0;
  return 0;
}

LUALIB_API int luaopen_fibatch(lua_State *L)
{
  luaL_newmetatable(L, "fibatch.eligible");
  lua_pushcfunction(L, leaf_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, "fibatch.ineligible");
  lua_pushstring(L, "upvalue");
  lua_pushcclosure(L, closure_finalizer, 1);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, "fibatch.nonzero");
  lua_pushcfunction(L, nonzero_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  lua_newtable(L);
  lua_pushcfunction(L, alloc_eligible);
  lua_setfield(L, -2, "alloc_eligible");
  lua_pushcfunction(L, alloc_mixed);
  lua_setfield(L, -2, "alloc_mixed");
  lua_pushcfunction(L, alloc_nonzero);
  lua_setfield(L, -2, "alloc_nonzero");
  lua_pushcfunction(L, counters);
  lua_setfield(L, -2, "counters");
  lua_pushcfunction(L, reset);
  lua_setfield(L, -2, "reset");
  return 1;
}
C
close $c;

my @compile = ($cc, "-shared", "-fPIC", "-I$Bin/../src", "-o", "fibatch.so", "fibatch.c");
my $compile_out = `@compile 2>&1`;
my $compile_rc = $? >> 8;
if ($compile_rc != 0 && $compile_out =~ /not found|No such file|command not found/i) {
    plan skip_all => "C compiler '$cc' is not available";
}
die "Cannot compile fibatch.so with '@compile':\n$compile_out" if $compile_rc != 0;

plan tests => 1;

$ENV{LUA_CPATH} = "$dir/?.so;$Bin/../?.so;;";
$ENV{LUA_PATH} = "$Bin/../lua/?.lua;;";

open my $lua, '>', 'test.lua' or die "Cannot open test.lua: $!";
print $lua <<'LUA';
if jit then jit.off() end
local m = require "fibatch"

local DEFAULT_BATCH_CAP = 64
local FINALIZE_COST = 100
local expected_cap = tonumber(os.getenv("EXPECTED_BATCHED_FINALIZER_MAX") or "")

local function stats()
  return jit.gcstats()
end

local function stat(name)
  return stats()[name]
end

local function delta(before, after, name)
  return after[name] - before[name]
end

local function force_until(label, pred)
  for _ = 1, 32 do
    collectgarbage("collect")
    if pred() then return end
  end
  error(label)
end

local function force_stat(name, before, amount)
  force_until(name, function()
    return stat(name) >= before + amount
  end)
end

local function reset_all()
  m.reset()
  collectgarbage("restart")
  collectgarbage("collect")
  collectgarbage("collect")
  jit.gcstats(true)
end

local function check_counter_shape()
  local s = stats()
  local fields = {
    "finalizer_direct_cfunc_batches",
    "finalizer_direct_cfunc_batched_calls",
    "finalizer_direct_cfunc_batch_max",
  }
  for _, name in ipairs(fields) do
    assert(type(s[name]) == "number", name)
    assert(s[name] >= 0, name)
  end
end

local function check_cap(observed_max, batched_calls, batches)
  local cap = expected_cap or DEFAULT_BATCH_CAP
  assert(observed_max <= cap, "batch max " .. observed_max .. " > cap " .. cap)
  assert(batches >= math.ceil(batched_calls / cap),
         "too few batches for cap: " .. batches .. " calls=" .. batched_calls .. " cap=" .. cap)
  if cap > 1 and batched_calls > cap then
    assert(batches < batched_calls,
           "batched path degraded to singleton dispatches: batches=" .. batches ..
           " calls=" .. batched_calls)
    assert(observed_max == cap,
           "batch max " .. observed_max .. " did not reach cap " .. cap)
  end
end

local function check_step_pacing()
  local cap = expected_cap or DEFAULT_BATCH_CAP
  local n = math.min(cap * 4, 1000)
  local stepmul = 20
  local stepsize_kb = 1
  local budget = math.floor((stepsize_kb * 1024) / 100) * stepmul
  local step_cap = math.max(1, math.floor(budget / FINALIZE_COST))
  local expected_step_cap = math.min(cap, step_cap)
  if cap <= 1 or n <= cap then return end

  reset_all()
  local old_stepmul = collectgarbage("setstepmul", stepmul)
  local old_stepsize = collectgarbage("setstepsize", stepsize_kb)
  collectgarbage("stop")
  m.alloc_eligible(n)

  local function isolated_step_delta()
    collectgarbage("stop")
    local before_calls = stat("finalizer_direct_cfunc_batched_calls")
    collectgarbage("restart")
    collectgarbage("step", 0)
    collectgarbage("stop")
    return stat("finalizer_direct_cfunc_batched_calls") - before_calls
  end

  local first_step_calls = nil
  for _ = 1, 100000 do
    local step_calls = isolated_step_delta()
    if step_calls > 0 then
      first_step_calls = step_calls
      break
    end
  end
  assert(first_step_calls, "GC step loop never reached batched finalizers")
  assert(first_step_calls <= expected_step_cap,
         "first finalizer step exceeded budget-aware cap: " .. first_step_calls ..
         " > " .. expected_step_cap)

  local second_step_calls = isolated_step_delta()
  assert(second_step_calls > 0, "second finalizer step did not run remaining finalizers")
  assert(second_step_calls <= expected_step_cap,
         "second finalizer step exceeded budget-aware cap: " .. second_step_calls ..
         " > " .. expected_step_cap)

  collectgarbage("setstepmul", old_stepmul)
  collectgarbage("setstepsize", old_stepsize)
  collectgarbage("restart")
  force_until("paced finalizers did not drain", function()
    local leaf, closure, stack_errors = m.counters()
    return leaf == n and closure == 0 and stack_errors == 0
  end)
end

local function check_homogeneous_batch()
  local n = 200
  reset_all()
  local before = stats()
  collectgarbage("stop")
  m.alloc_eligible(n)
  collectgarbage("restart")
  force_until("eligible finalizers did not drain", function()
    local leaf, closure, stack_errors = m.counters()
    return leaf == n and closure == 0 and stack_errors == 0
  end)
  local after = stats()
  local batches = delta(before, after, "finalizer_direct_cfunc_batches")
  local direct = delta(before, after, "finalizer_direct_cfunc_calls")
  local batched = delta(before, after, "finalizer_direct_cfunc_batched_calls")
  local frees = delta(before, after, "finalizer_nonresurrecting_cfunc_frees")
  local calls = delta(before, after, "finalizer_calls")
  assert(batches > 0, "no batched finalizer dispatches")
  assert(direct == n, "direct calls " .. direct .. " != " .. n)
  assert(batched == direct, "batched calls " .. batched .. " != direct " .. direct)
  assert(frees == direct, "direct-free count " .. frees .. " != direct " .. direct)
  assert(calls == direct, "finalizer calls " .. calls .. " != direct " .. direct)
  check_cap(after.finalizer_direct_cfunc_batch_max, batched, batches)
end

local function check_mixed_queue_keeps_fallbacks()
  local eligible = 33
  local ineligible = 7
  reset_all()
  local before = stats()
  collectgarbage("stop")
  m.alloc_mixed(eligible, ineligible)
  collectgarbage("restart")
  force_until("mixed finalizers did not drain", function()
    local leaf, closure, stack_errors = m.counters()
    return leaf == eligible and closure == ineligible and stack_errors == 0
  end)
  local after = stats()
  local direct = delta(before, after, "finalizer_direct_cfunc_calls")
  local batched = delta(before, after, "finalizer_direct_cfunc_batched_calls")
  local frees = delta(before, after, "finalizer_nonresurrecting_cfunc_frees")
  local upvalue = delta(before, after, "finalizer_cfunc_upvalue_calls")
  local fallback = delta(before, after, "finalizer_nonresurrecting_cfunc_fallbacks")
  local calls = delta(before, after, "finalizer_calls")
  assert(direct == eligible, "direct calls " .. direct .. " != " .. eligible)
  assert(batched == eligible, "batched calls " .. batched .. " != " .. eligible)
  assert(frees == eligible, "direct-free count " .. frees .. " != " .. eligible)
  assert(upvalue == ineligible, "upvalue calls " .. upvalue .. " != " .. ineligible)
  assert(fallback == ineligible, "fallbacks " .. fallback .. " != " .. ineligible)
  assert(calls == eligible + ineligible,
         "finalizer calls " .. calls .. " != " .. (eligible + ineligible))
end

local function check_nonzero_result_stops_each_batch()
  local n = 4
  reset_all()
  local before = stats()
  collectgarbage("stop")
  m.alloc_nonzero(n)
  collectgarbage("restart")
  force_until("nonzero finalizers did not drain", function()
    local leaf, closure, stack_errors, nonzero = m.counters()
    return leaf == 0 and closure == 0 and stack_errors == 0 and nonzero == n
  end)
  local after = stats()
  local batches = delta(before, after, "finalizer_direct_cfunc_batches")
  local direct = delta(before, after, "finalizer_direct_cfunc_calls")
  local batched = delta(before, after, "finalizer_direct_cfunc_batched_calls")
  local nonzero = delta(before, after, "finalizer_direct_cfunc_nonzero_results")
  local frees = delta(before, after, "finalizer_nonresurrecting_cfunc_frees")
  assert(batches == n, "nonzero batches " .. batches .. " != " .. n)
  assert(direct == n, "nonzero direct calls " .. direct .. " != " .. n)
  assert(batched == n, "nonzero batched calls " .. batched .. " != " .. n)
  assert(nonzero == n, "nonzero results " .. nonzero .. " != " .. n)
  assert(frees == n, "nonzero frees " .. frees .. " != " .. n)
  assert(after.finalizer_direct_cfunc_batch_max == 1,
         "nonzero batch max " .. after.finalizer_direct_cfunc_batch_max .. " != 1")
end

local function check_unsupported_finalizers_do_not_batch()
  reset_all()
  local before = stats()
  do
    local u = newproxy(true)
    getmetatable(u).__gc = tostring
    u = nil
  end
  force_stat("finalizer_ffunc_calls", before.finalizer_ffunc_calls, 1)
  local after = stats()
  assert(delta(before, after, "finalizer_direct_cfunc_batches") == 0)
  assert(delta(before, after, "finalizer_direct_cfunc_batched_calls") == 0)
  assert(delta(before, after, "finalizer_direct_cfunc_calls") == 0)
  assert(delta(before, after, "finalizer_nonresurrecting_cfunc_frees") == 0)

  local ok, ffi = pcall(require, "ffi")
  if ok then
    local ran = 0
    reset_all()
    before = stats()
    do
      local p = ffi.gc(ffi.new("int[1]"), function() ran = ran + 1 end)
      p = nil
    end
    force_until("cdata finalizer did not run", function() return ran == 1 end)
    after = stats()
    assert(delta(before, after, "finalizer_direct_cfunc_batches") == 0)
    assert(delta(before, after, "finalizer_direct_cfunc_batched_calls") == 0)
    assert(delta(before, after, "finalizer_direct_cfunc_calls") == 0)
    assert(delta(before, after, "finalizer_nonresurrecting_cfunc_frees") == 0)
  end
end

check_counter_shape()
check_homogeneous_batch()
check_mixed_queue_keeps_fallbacks()
check_nonzero_result_stops_each_batch()
check_unsupported_finalizers_do_not_batch()
check_step_pacing()

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

is "$rc:$out$err", "0:ok\n", 'batched direct C finalizers are counted and capped';
