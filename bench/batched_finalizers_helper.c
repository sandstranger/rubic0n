#include "lua.h"
#include "lauxlib.h"

#include <stdio.h>

#define BATCHFIN_HELPER_MAX_METATABLES 4096
#define BATCHFIN_HELPER_MAX_FINALIZERS 8

typedef struct BatchFinPayload {
  int metatable_index;
  int sequence;
} BatchFinPayload;

static int finalizer_count;
static int upvalue_finalizer_count;
static int stack_error_count;
static int finalizer_counts[BATCHFIN_HELPER_MAX_FINALIZERS];

static void mt_name(char *buf, int index)
{
  sprintf(buf, "batchedfinalizers.udata.%d", index);
}

static void check_count(lua_State *L, int n, const char *what)
{
  luaL_argcheck(L, n >= 1, 1, what);
}

static int finalizer_body(lua_State *L, int index)
{
  int ok = lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TUSERDATA;
  lua_pushboolean(L, 1);
  ok = ok && lua_gettop(L) == 2;
  lua_pop(L, 1);
  ok = ok && lua_gettop(L) == 1;
  if (!ok) stack_error_count++;
  finalizer_count++;
  finalizer_counts[index]++;
  return 0;
}

#define DEFINE_FINALIZER(n) \
  static int ud_finalizer_##n(lua_State *L) { return finalizer_body(L, (n)); }

DEFINE_FINALIZER(0)
DEFINE_FINALIZER(1)
DEFINE_FINALIZER(2)
DEFINE_FINALIZER(3)
DEFINE_FINALIZER(4)
DEFINE_FINALIZER(5)
DEFINE_FINALIZER(6)
DEFINE_FINALIZER(7)

static lua_CFunction finalizers[BATCHFIN_HELPER_MAX_FINALIZERS] = {
  ud_finalizer_0, ud_finalizer_1, ud_finalizer_2, ud_finalizer_3,
  ud_finalizer_4, ud_finalizer_5, ud_finalizer_6, ud_finalizer_7
};

static int upvalue_finalizer(lua_State *L)
{
  int ok = lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TUSERDATA;
  luaL_checkany(L, lua_upvalueindex(1));
  if (!ok) stack_error_count++;
  upvalue_finalizer_count++;
  return 0;
}

static void ensure_metatable(lua_State *L, int index, int finalizer_index,
                             int ineligible)
{
  char name[64];
  mt_name(name, index);
  if (luaL_newmetatable(L, name)) {
    if (ineligible) {
      lua_pushliteral(L, "upvalue");
      lua_pushcclosure(L, upvalue_finalizer, 1);
    } else {
      lua_pushcfunction(L, finalizers[finalizer_index]);
    }
    lua_setfield(L, -2, "__gc");
  }
  lua_pop(L, 1);
}

static int is_ineligible_index(int index, int ineligible_every)
{
  return ineligible_every > 0 && index % ineligible_every == 0;
}

static void push_metatable(lua_State *L, int index)
{
  char name[64];
  mt_name(name, index);
  luaL_getmetatable(L, name);
}

static void new_udata(lua_State *L, int mt_index, int sequence)
{
  BatchFinPayload *payload = (BatchFinPayload *)lua_newuserdata(L, sizeof(*payload));
  payload->metatable_index = mt_index;
  payload->sequence = sequence;
  push_metatable(L, mt_index);
  lua_setmetatable(L, -2);
}

static int l_ensure_metatables(lua_State *L)
{
  int k = luaL_checkint(L, 1);
  int finalizer_n = luaL_checkint(L, 2);
  int ineligible_every = luaL_optint(L, 3, 0);
  int i;
  check_count(L, k, "metatable count must be positive");
  luaL_argcheck(L, k <= BATCHFIN_HELPER_MAX_METATABLES, 1,
                "metatable count is too large");
  luaL_argcheck(L, finalizer_n >= 1 && finalizer_n <= BATCHFIN_HELPER_MAX_FINALIZERS,
                2, "finalizer function count is out of range");
  luaL_argcheck(L, ineligible_every >= 0, 3, "ineligible interval must be non-negative");
  for (i = 1; i <= k; i++) {
    int fin = (i - 1) % finalizer_n;
    ensure_metatable(L, i, fin, is_ineligible_index(i, ineligible_every));
  }
  return 0;
}

static int l_alloc_homogeneous(lua_State *L)
{
  int n = luaL_checkint(L, 1);
  int i;
  check_count(L, n, "object count must be positive");
  for (i = 1; i <= n; i++) {
    new_udata(L, 1, i);
    lua_pop(L, 1);
  }
  return 0;
}

static int l_alloc_mixed(lua_State *L)
{
  int n = luaL_checkint(L, 1);
  int k = luaL_checkint(L, 2);
  int i;
  check_count(L, n, "object count must be positive");
  luaL_argcheck(L, k >= 1 && k <= BATCHFIN_HELPER_MAX_METATABLES, 2,
                "metatable count is out of range");
  for (i = 1; i <= n; i++) {
    new_udata(L, ((i - 1) % k) + 1, i);
    lua_pop(L, 1);
  }
  return 0;
}

static int l_counters(lua_State *L)
{
  lua_pushinteger(L, finalizer_count);
  lua_pushinteger(L, upvalue_finalizer_count);
  lua_pushinteger(L, stack_error_count);
  return 3;
}

static int l_finalizer_counts(lua_State *L)
{
  int i;
  lua_newtable(L);
  for (i = 0; i < BATCHFIN_HELPER_MAX_FINALIZERS; i++) {
    lua_pushinteger(L, finalizer_counts[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_reset(lua_State *L)
{
  int i;
  (void)L;
  finalizer_count = 0;
  upvalue_finalizer_count = 0;
  stack_error_count = 0;
  for (i = 0; i < BATCHFIN_HELPER_MAX_FINALIZERS; i++)
    finalizer_counts[i] = 0;
  return 0;
}

LUALIB_API int luaopen_batchedfinalizers(lua_State *L)
{
  lua_newtable(L);
  lua_pushinteger(L, BATCHFIN_HELPER_MAX_FINALIZERS);
  lua_setfield(L, -2, "max_finalizers");
  lua_pushcfunction(L, l_ensure_metatables);
  lua_setfield(L, -2, "ensure_metatables");
  lua_pushcfunction(L, l_alloc_homogeneous);
  lua_setfield(L, -2, "alloc_homogeneous");
  lua_pushcfunction(L, l_alloc_mixed);
  lua_setfield(L, -2, "alloc_mixed");
  lua_pushcfunction(L, l_counters);
  lua_setfield(L, -2, "counters");
  lua_pushcfunction(L, l_finalizer_counts);
  lua_setfield(L, -2, "finalizer_counts");
  lua_pushcfunction(L, l_reset);
  lua_setfield(L, -2, "reset");
  return 1;
}
