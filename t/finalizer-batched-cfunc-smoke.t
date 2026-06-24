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
my $dir = tempdir "testlj_finalizer_batched_cfunc_smoke_XXXXXXX", CLEANUP => 1;
chdir $dir or die "Cannot chdir to $dir: $!";

open my $c, '>', 'fibatchsmoke.c' or die "Cannot open fibatchsmoke.c: $!";
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

static void push_with_mt(lua_State *L, const char *mt)
{
  lua_newuserdata(L, 1);
  luaL_getmetatable(L, mt);
  lua_setmetatable(L, -2);
}

static int alloc_eligible(lua_State *L)
{
  int i, n = luaL_checkint(L, 1);
  luaL_argcheck(L, n >= 0, 1, "non-negative count expected");
  for (i = 0; i < n; i++) {
    push_with_mt(L, "fibatchsmoke.eligible");
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
      push_with_mt(L, "fibatchsmoke.eligible");
      lua_pop(L, 1);
    }
    if (i < ineligible) {
      push_with_mt(L, "fibatchsmoke.ineligible");
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
    push_with_mt(L, "fibatchsmoke.nonzero");
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

LUALIB_API int luaopen_fibatchsmoke(lua_State *L)
{
  luaL_newmetatable(L, "fibatchsmoke.eligible");
  lua_pushcfunction(L, leaf_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, "fibatchsmoke.ineligible");
  lua_pushstring(L, "upvalue");
  lua_pushcclosure(L, closure_finalizer, 1);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, "fibatchsmoke.nonzero");
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

my @compile = ($cc, "-shared", "-fPIC", "-I$Bin/../src", "-o",
               "fibatchsmoke.so", "fibatchsmoke.c");
my $compile_out = `@compile 2>&1`;
my $compile_rc = $? >> 8;
if ($compile_rc != 0 && $compile_out =~ /not found|No such file|command not found/i) {
    plan skip_all => "C compiler '$cc' is not available";
}
die "Cannot compile fibatchsmoke.so with '@compile':\n$compile_out" if $compile_rc != 0;

plan tests => 1;

$ENV{LUA_CPATH} = "$dir/?.so;$Bin/../?.so;;";
$ENV{LUA_PATH} = "$Bin/../lua/?.lua;;";

open my $lua, '>', 'test.lua' or die "Cannot open test.lua: $!";
print $lua <<'LUA';
if jit then jit.off() end
local m = require "fibatchsmoke"

local FINALIZE_COST = 100
local STEP_MUL = 20
local STEP_SIZE_KB = 1

local function counters()
  return m.counters()
end

local function force_until(label, pred)
  for _ = 1, 64 do
    collectgarbage("collect")
    if pred() then return end
  end
  error(label)
end

local function reset_all()
  m.reset()
  collectgarbage("restart")
  collectgarbage("collect")
  collectgarbage("collect")
end

local function isolated_step()
  collectgarbage("restart")
  collectgarbage("step", 0)
  collectgarbage("stop")
end

local function check_budget_pacing_without_gcstats()
  local n = 200
  local old_stepmul = collectgarbage("setstepmul", STEP_MUL)
  local old_stepsize = collectgarbage("setstepsize", STEP_SIZE_KB)
  local budget = math.floor((STEP_SIZE_KB * 1024) / 100) * STEP_MUL
  local budget_cap = math.max(1, math.floor(budget / FINALIZE_COST))
  local previous = 0
  local first_delta

  reset_all()
  collectgarbage("stop")
  m.alloc_eligible(n)

  for _ = 1, 100000 do
    local leaf
    isolated_step()
    leaf = counters()
    if leaf > previous then
      first_delta = leaf - previous
      previous = leaf
      break
    end
    previous = leaf
  end

  assert(first_delta, "GC step loop never reached batched finalizers")
  assert(first_delta <= budget_cap,
         "first finalizer step ran " .. first_delta ..
         " finalizers with budget cap " .. budget_cap)

  isolated_step()
  do
    local leaf = counters()
    local second_delta = leaf - previous
    assert(second_delta > 0, "second finalizer step did not run remaining finalizers")
    assert(second_delta <= budget_cap,
           "second finalizer step ran " .. second_delta ..
           " finalizers with budget cap " .. budget_cap)
  end

  collectgarbage("setstepmul", old_stepmul)
  collectgarbage("setstepsize", old_stepsize)
  collectgarbage("restart")
  force_until("eligible finalizers did not drain", function()
    local leaf, closure, stack_errors, nonzero = counters()
    return leaf == n and closure == 0 and nonzero == 0 and stack_errors == 0
  end)
end

local function check_mixed_fallbacks_without_gcstats()
  local eligible = 37
  local ineligible = 11
  reset_all()
  collectgarbage("stop")
  m.alloc_mixed(eligible, ineligible)
  collectgarbage("restart")
  force_until("mixed finalizers did not drain", function()
    local leaf, closure, stack_errors, nonzero = counters()
    return leaf == eligible and closure == ineligible and
           nonzero == 0 and stack_errors == 0
  end)
end

local function check_nonzero_contract_violation_without_gcstats()
  local old_stepmul = collectgarbage("setstepmul", STEP_MUL)
  local old_stepsize = collectgarbage("setstepsize", STEP_SIZE_KB)
  local previous = 0
  local first_delta

  reset_all()
  collectgarbage("stop")
  m.alloc_nonzero(4)
  for _ = 1, 100000 do
    local leaf, closure, stack_errors, nonzero
    isolated_step()
    leaf, closure, stack_errors, nonzero = counters()
    assert(leaf == 0 and closure == 0 and stack_errors == 0,
           leaf .. ":" .. closure .. ":" .. stack_errors)
    if nonzero > previous then
      first_delta = nonzero - previous
      break
    end
    previous = nonzero
  end
  assert(first_delta == 1,
         "nonzero direct C finalizer did not stop batch: " .. tostring(first_delta))

  collectgarbage("setstepmul", old_stepmul)
  collectgarbage("setstepsize", old_stepsize)
  collectgarbage("restart")
  force_until("nonzero finalizers did not drain", function()
    local leaf, closure, stack_errors, nonzero = counters()
    return leaf == 0 and closure == 0 and nonzero == 4 and stack_errors == 0
  end)
end

check_budget_pacing_without_gcstats()
check_mixed_fallbacks_without_gcstats()
check_nonzero_contract_violation_without_gcstats()

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

is "$rc:$out$err", "0:ok\n", 'batched C finalizers pace and fallback without GCStats';
