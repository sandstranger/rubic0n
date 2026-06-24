/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmevent.h"

#define GCSWEEPMAX	40
#define GCSWEEPUDATAMAX	GCSWEEPMAX
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

#ifndef LUAJIT_BATCHED_FINALIZER_MAX
#define LUAJIT_BATCHED_FINALIZER_MAX 64
#elif LUAJIT_BATCHED_FINALIZER_MAX < 1
#error "LUAJIT_BATCHED_FINALIZER_MAX must be >= 1"
#endif

typedef struct GCFinalizerLookup {
  cTValue *mo;
  int resolved;
  int direct;
} GCFinalizerLookup;

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct), \
	       "TValue and GC type mismatch"); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lj_assertG(iswhite(o), "mark of non-white object");
  lj_assertG(!isdead(g, o), "mark of dead object");
  white2gray(o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    gc_markobj(g, tabref(gco2ud(o)->env));
    if (LJ_HASBUFFER && gco2ud(o)->udtype == UDTYPE_BUFFER) {
      SBufExt *sbx = (SBufExt *)uddata(gco2ud(o));
      if (sbufiscow(sbx) && gcref(sbx->cowref))
	gc_markobj(g, gcref(sbx->cowref));
      if (gcref(sbx->dict_str))
	gc_markobj(g, gcref(sbx->dict_str));
      if (gcref(sbx->dict_mt))
	gc_markobj(g, gcref(sbx->dict_mt));
    }
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lj_assertG(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE,
	       "bad GC type %d", gct);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_markobj(g, vmthread(g));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lj_assertG(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv,
	       "broken upvalue chain");
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

/* Queue an already unlinked userdata/cdata object for finalization.
** g->gc.mmudata points to the tail of this circular queue.
*/
void lj_gc_queuefinalizer(global_State *g, GCobj *o)
{
  GCobj *root = gcref(g->gc.mmudata);
  lj_assertG(o->gch.gct == ~LJ_TUDATA || o->gch.gct == ~LJ_TCDATA,
	     "bad finalizer queue object");
  lj_gc_stats_inc(g, finalizer_queued);
  if (root) {  /* Link after tail and make the new object the tail. */
    setgcrefr(o->gch.nextgc, root->gch.nextgc);
    setgcref(root->gch.nextgc, o);
    setgcref(g->gc.mmudata, o);
  } else {  /* Create circular list. */
    setgcref(o->gch.nextgc, o);
    setgcref(g->gc.mmudata, o);
  }
}

static void gc_unqueuefinalizer(global_State *g, GCobj *o)
{
  GCobj *tail = gcref(g->gc.mmudata);
  lj_assertG(tail != NULL && gcnext(tail) == o, "bad finalizer queue unlink");
  if (o == tail)
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(tail->gch.nextgc, o->gch.nextgc);
}

/*
** Separate userdata objects to be finalized to mmudata list. Userdata without
** a __gc metamethod are parked back on the normal root list and swept there.
** This runs only during atomic or lua_close, not while the root sweep cursor is
** active, so moving objects to g->gc.root cannot invalidate an in-progress
** root-list sweep.
*/
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  GCRef *p = &mainthread(g)->nextgc;
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    lj_gc_stats_inc(g, finalizer_scan_steps);
    if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
      setgcrefr(*p, o->gch.nextgc);
      setgcrefr(o->gch.nextgc, g->gc.root);
      setgcref(g->gc.root, o);
    } else if (!(iswhite(o) || all) || isfinalized(gco2ud(o))) {
      p = &o->gch.nextgc;  /* Nothing to do. */
    } else {  /* Otherwise move userdata to be finalized to mmudata list. */
      m += sizeudata(gco2ud(o));
      markfinalized(o);
      setgcrefr(*p, o->gch.nextgc);
      lj_gc_queuefinalizer(g, o);
    }
  }
  return m;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_markobj(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      if (gcref(g->gcroot[GCROOT_FFI_FIN]) == obj2gco(t)) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
	lj_gc_stats_inc(g, weak_tables);
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
  }
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lj_assertG(!tvisnil(&n->key), "mark of nil key in non-empty slot");
	if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n->key);
	if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lj_assertG(fn->l.nupvalues <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lj_assertG(traceno != G2J(g)->cur.traceno, "active trace escaped");
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  size_t m;
  lj_assertG(isgray(o), "propagation of non-gray object");
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    m = sizeof(GCtab) + sizeof(TValue) * t->asize +
	(t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    m = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
	sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    m = pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    m = sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    m = ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
      T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lj_assertG(0, "bad GC type %d", gct);
    m = 0;
#endif
  }
  lj_gc_stats_inc(g, propagate_calls);
  lj_gc_stats_add(g, propagate_bytes, m);
  return m;
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead object");
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive object");
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return p;
}

/* Sweep one string interning table chain. Preserves hashalg bit. */
static void gc_sweepstr(global_State *g, GCRef *chain)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  uintptr_t u = gcrefu(*chain);
  GCRef q;
  GCRef *p = &q;
  GCobj *o;
  setgcrefp(q, (u & ~(uintptr_t)1));
  while ((o = gcref(*p)) != NULL) {
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead string");
      makewhite(g, o);  /* String is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise string is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive string");
      setgcrefr(*p, o->gch.nextgc);
      lj_str_free(g, gco2str(o));
    }
  }
  setgcrefp(*chain, (gcrefu(q) | (u & 1)));
}

/* Preserve an immediate object needed by a sweep-discovered finalizer. */
static void gc_preserve_now(global_State *g, GCobj *o)
{
  if (o) {
    makewhite(g, o);
    lj_gc_stats_inc(g, sweep_udata_preserved);
  }
}

/*
** Partial sweep of the userdata candidate segment after atomic marking.
**
** This mode deliberately does not implement stock Lua/LuaJIT atomic finalizer
** discovery. It is for static, leaf/native userdata finalizers: late mutation
** of metatables or __gc is unsupported after the candidate is scanned, and Lua
** closure dependency graphs are not recursively preserved here. For a dead
** finalizable userdata, the userdata is always made current-white. Its metatable
** and immediate __gc callable are made current-white only if they are still dead
** in this post-atomic sweep-udata window, so root/table sweeping cannot free
** required dead objects before GCSfinalize resolves the finalizer.
*/
static GCRef *gc_sweepudata(global_State *g, GCRef *p, uint32_t lim,
				    GCSize *queued)
{
  int ow = otherwhite(g);
  GCobj *o;
  *queued = 0;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    GCudata *ud = gco2ud(o);
    GCtab *mt = tabref(ud->metatable);
    cTValue *mo = lj_meta_fastg(g, mt, MM_gc);
    int alive = ((o->gch.marked ^ LJ_GC_WHITES) & ow);
    lj_gc_stats_inc(g, sweep_udata_steps);
    if (alive) {  /* Black or current white: keep or park the live userdata. */
      lj_assertG(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED),
		 "sweep of undead userdata");
      makewhite(g, o);
      if (mo) {
	p = &o->gch.nextgc;
      } else {
	setgcrefr(*p, o->gch.nextgc);
	setgcrefr(o->gch.nextgc, g->gc.root);
	setgcref(g->gc.root, o);
	lj_gc_stats_inc(g, sweep_udata_parked);
      }
    } else {  /* Otherwise the userdata is dead. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive userdata");
      setgcrefr(*p, o->gch.nextgc);
      if (mo) {
	GCobj *mto;
	*queued += sizeudata(ud);
	markfinalized(o);
	gc_preserve_now(g, o);
	lj_gc_stats_inc(g, sweep_udata_preserve_udata);
	mto = obj2gco(mt);
	if (isdead(g, mto)) {
	  gc_preserve_now(g, mto);
	  lj_gc_stats_inc(g, sweep_udata_preserve_mt_dead);
	} else {
	  lj_gc_stats_inc(g, sweep_udata_preserve_mt_alive_skip);
	}
	if (tvisgcv(mo)) {
	  GCobj *mmo = gcV(mo);
	  if (isdead(g, mmo)) {
	    gc_preserve_now(g, mmo);
	    lj_gc_stats_inc(g, sweep_udata_preserve_callable_dead);
	  } else {
	    lj_gc_stats_inc(g, sweep_udata_preserve_callable_alive_skip);
	  }
	} else {
	  lj_gc_stats_inc(g, sweep_udata_preserve_callable_nongc);
	}
	lj_gc_queuefinalizer(g, o);
	lj_gc_stats_inc(g, sweep_udata_queued);
      } else {
	lj_udata_free(g, ud);
	lj_gc_stats_inc(g, sweep_udata_freed);
      }
    }
  }
  return p;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
static void gc_clearweak(global_State *g, GCobj *o)
{
  UNUSED(g);
  while (o) {
    GCtab *t = gco2tab(o);
    lj_assertG((t->marked & LJ_GC_WEAK), "clear of non-weak table");
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1)) {
	  setnilV(tv);
	  lj_gc_stats_inc(g, weak_slots_cleared);
	}
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1))) {
	  setnilV(&n->val);
	  lj_gc_stats_inc(g, weak_slots_cleared);
	}
      }
    }
    o = gcref(t->gclist);
  }
}

#ifdef LUAJIT_ENABLE_GCSTATS
static void gc_count_finalizer_kind(global_State *g, cTValue *mo)
{
  if (tvisfunc(mo)) {
    GCfunc *fn = funcV(mo);
    if (iscfunc(fn)) {
      lj_gc_stats_inc(g, finalizer_cfunc_calls);
      if (fn->c.nupvalues == 0)
        lj_gc_stats_inc(g, finalizer_cfunc_nup0_calls);
      else
        lj_gc_stats_inc(g, finalizer_cfunc_upvalue_calls);
    } else if (isluafunc(fn)) {
      lj_gc_stats_inc(g, finalizer_lfunc_calls);
    } else {
      lj_assertG(isffunc(fn), "bad finalizer function type");
      lj_gc_stats_inc(g, finalizer_ffunc_calls);
    }
  } else {
    lj_gc_stats_inc(g, finalizer_other_calls);
  }
}
#else
#define gc_count_finalizer_kind(g, mo) \
  ((void)0)
#endif

static GCfunc *gc_unprotected_c_finalizer_func(cTValue *mo, GCobj *o)
{
  GCfunc *fn;
  if (o->gch.gct != ~LJ_TUDATA || !tvisfunc(mo))
    return NULL;
  fn = funcV(mo);
  if (!iscfunc(fn) || fn->c.nupvalues != 0)
    return NULL;
  return fn;
}

static int gc_unprotected_c_finalizer(global_State *g, lua_State *VL,
				     cTValue *mo, GCobj *o, int *nresp)
{
  GCfunc *fn = gc_unprotected_c_finalizer_func(mo, o);
  TValue *oldbase, *oldtop, *top;
  int nres;
  if (fn == NULL)
    return 0;
  /*
  ** Experimental raw destructor ABI:
  ** Handles zero-upvalue C finalizers that accept a full userdata at positive
  ** index 1, return 0, and never throw, yield, or call Lua. It provides
  ** curr_func() metadata for basic C API checks plus normal stack headroom, but
  ** no protected error or yield handling and no general lua_CFunction
  ** call-frame semantics.
  */
  oldbase = VL->base;
  oldtop = VL->top;
  top = oldtop;
  copyTV(VL, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(VL, top, o, ~o->gch.gct);
  VL->base = top;
  VL->top = top+1;
  nres = fn->c.f(VL);
  if (nresp)
    *nresp = nres;
  if (nres != 0)
    lj_gc_stats_inc(g, finalizer_direct_cfunc_nonzero_results);
  lj_gc_stats_inc(g, finalizer_direct_cfunc_calls);
  VL->base = oldbase;
  VL->top = oldtop;
  return 1;
}

static void gc_call_nonresurrecting_c_finalizer(global_State *g, lua_State *L,
				     cTValue *mo, GCobj *o)
{
  lua_State *VL = vmthread(g);
  uint8_t oldh = hook_save(g);
  GCSize oldt = g->gc.threshold;
  int ok;

  lj_gc_stats_inc(g, finalizer_calls);
  gc_count_finalizer_kind(g, mo);
  lj_trace_abort(g);
  hook_entergc(g);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = LJ_MAX_MEM;
  ok = gc_unprotected_c_finalizer(g, VL, mo, o, NULL);
  lj_assertG(ok, "bad non-resurrecting finalizer eligibility");
  UNUSED(ok);
  setgcref(g->cur_L, obj2gco(L));
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = oldt;
}

static void gc_resolve_direct_udata_finalizer(global_State *g, GCobj *o,
					     GCFinalizerLookup *lookup)
{
  lookup->mo = NULL;
  lookup->resolved = 0;
  lookup->direct = 0;
  if (o->gch.gct != ~LJ_TUDATA)
    return;
  lookup->resolved = 1;
  lookup->mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  lookup->direct = lookup->mo &&
    gc_unprotected_c_finalizer_func(lookup->mo, o) != NULL;
}

static MSize gc_finalizer_batch_limit(GCSize lim)
{
  MSize cap = LUAJIT_BATCHED_FINALIZER_MAX;
  GCSize budget_cap = lim / GCFINALIZECOST;
  if (budget_cap == 0)
    return 1;
  if (budget_cap < cap)
    cap = (MSize)budget_cap;
  return cap;
}

static MSize gc_finalize_direct_cfunc_batch(lua_State *L, GCSize lim,
					   GCFinalizerLookup *fallback,
					   int *contract_violationp)
{
  global_State *g = G(L);
  lua_State *VL = vmthread(g);
  GCRef oldcur_L;
  uint8_t oldh;
  GCSize oldt, freed_bytes = 0;
  MSize cap = gc_finalizer_batch_limit(lim);
  MSize n = 0;
  GCobj *o;
  cTValue *mo;
  GCFinalizerLookup lookup;

  lj_assertG(g->gc.state == GCSfinalize,
	     "batched finalizer outside finalizer phase");
  *contract_violationp = 0;
  o = gcnext(gcref(g->gc.mmudata));
  gc_resolve_direct_udata_finalizer(g, o, &lookup);
  if (!lookup.direct) {
    *fallback = lookup;
    return 0;
  }
  mo = lookup.mo;

  /* May throw while the queue is still untouched. One frame is reused. */
  lj_state_checkstack(VL, 1+LJ_FR2+LUA_MINSTACK);
  oldcur_L = g->cur_L;
  oldh = hook_save(g);
  oldt = g->gc.threshold;
  lj_trace_abort(g);
  hook_entergc(g);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = LJ_MAX_MEM;

  do {
    GCudata *ud = gco2ud(o);
    GCSize sz = (GCSize)sizeudata(ud);
    int ok;
    int nres = 0;
    lj_gc_stats_inc(g, finalizer_calls);
    gc_count_finalizer_kind(g, mo);
    gc_unqueuefinalizer(g, o);
    ok = gc_unprotected_c_finalizer(g, VL, mo, o, &nres);
    lj_assertG(ok, "bad batched finalizer eligibility");
    UNUSED(ok);
    freed_bytes += sz;
    lj_udata_free(g, ud);
    lj_gc_stats_inc(g, finalizer_nonresurrecting_cfunc_frees);
    n++;
    if (nres != 0)
      *contract_violationp = 1;
    if (n >= cap || *contract_violationp || gcref(g->gc.mmudata) == NULL)
      break;
    o = gcnext(gcref(g->gc.mmudata));
    gc_resolve_direct_udata_finalizer(g, o, &lookup);
    mo = lookup.direct ? lookup.mo : NULL;
  } while (mo != NULL);

  setgcrefr(g->cur_L, oldcur_L);
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = oldt;
  g->gc.estimate += freed_bytes;  /* Queued finalizable size was pre-subtracted. */
  lj_gc_stats_inc(g, finalizer_direct_cfunc_batches);
  lj_gc_stats_add(g, finalizer_direct_cfunc_batched_calls, n);
  lj_gc_stats_max(g, finalizer_direct_cfunc_batch_max, n);
  return n;
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o, int try_direct)
{
  /* Save and restore lots of state around the __gc callback. */
  lua_State *VL = vmthread(g);
  uint8_t oldh;
  GCSize oldt;
  int errcode;
  TValue *top;
  /* May throw before entering direct-finalizer context. */
  lj_state_checkstack(VL, 1+LJ_FR2+LUA_MINSTACK);
  oldh = hook_save(g);
  oldt = g->gc.threshold;
  lj_gc_stats_inc(g, finalizer_calls);
  gc_count_finalizer_kind(g, mo);
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  if (try_direct && gc_unprotected_c_finalizer(g, VL, mo, o, NULL)) {
    setgcref(g->cur_L, obj2gco(L));
    hook_restore(g, oldh);
    if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
    g->gc.threshold = oldt;  /* Restore GC threshold. */
    return;
  } else if (try_direct && tvisfunc(mo)) {
    GCfunc *fn = funcV(mo);
    if (iscfunc(fn) && fn->c.nupvalues == 0)
      lj_gc_stats_inc(g, finalizer_direct_cfunc_fallbacks);
  }
  top = VL->top;
  copyTV(VL, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(VL, top, o, ~o->gch.gct);
  VL->top = top+1;
  errcode = lj_vm_pcall(VL, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  setgcref(g->cur_L, obj2gco(L));
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode) {
    TValue tmp;
    lj_gc_stats_inc(g, finalizer_error_calls);
    copyTV(VL, &tmp, VL->top-1);
    VL->top--;
    lj_vmevent_send(g, ERRFIN,
      copyTV(V, V->top++, &tmp);
    );
  }
}

/* Finalize one userdata or cdata object from the mmudata list. */
static void gc_finalize(lua_State *L, int allow_nores,
			GCFinalizerLookup *lookup)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo = NULL;
  int resolved = lookup && lookup->resolved && o->gch.gct == ~LJ_TUDATA;
  int nores_fallback = 0;
  lj_assertG(tvref(g->jit_base) == NULL, "finalizer called on trace");
  if (allow_nores && o->gch.gct == ~LJ_TUDATA) {
    int direct;
    if (resolved) {
      mo = lookup->mo;
      direct = lookup->direct;
    } else {
      mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
      direct = mo && gc_unprotected_c_finalizer_func(mo, o) != NULL;
    }
    if (direct) {
      GCudata *ud = gco2ud(o);
      GCSize sz = (GCSize)sizeudata(ud);
      /* May throw while the queued userdata is still linked and reachable. */
      lj_state_checkstack(vmthread(g), 1+LJ_FR2+LUA_MINSTACK);
      gc_unqueuefinalizer(g, o);
      gc_call_nonresurrecting_c_finalizer(g, L, mo, o);
      if (g->gc.state == GCSfinalize)
	g->gc.estimate += sz;  /* Queued finalizable size was pre-subtracted. */
      lj_udata_free(g, ud);
      lj_gc_stats_inc(g, finalizer_nonresurrecting_cfunc_frees);
      return;
    } else if (mo) {
      nores_fallback = 1;
    }
  }
  /* Unchain from list of userdata to be finalized. */
  gc_unqueuefinalizer(g, o);
  if (nores_fallback)
    lj_gc_stats_inc(g, finalizer_nonresurrecting_cfunc_fallbacks);
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    setgcrefr(o->gch.nextgc, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, tabref(g->gcroot[GCROOT_FFI_FIN]), &tmp);
    if (!tvisnil(tv)) {
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o, 1);
    }
    return;
  }
#endif
  /* Add finalized userdata back to the GC list and make it white. */
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  if (!resolved)
    mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o, !resolved);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L, 0, NULL);  /* lua_close() drain: keep unbatched resurrection-capable path. */
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  GCtab *t = tabref(g->gcroot[GCROOT_FFI_FIN]);
  Node *node = noderef(t->node);
  ptrdiff_t i;
  setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
  for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
    if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
      GCobj *o = gcV(&node[i].key);
      TValue tmp;
      makewhite(g, o);
      o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
      copyTV(L, &tmp, &node[i].val);
      setnilV(&node[i].val);
      gc_call_finalizer(g, L, &tmp, o, 1);
    }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i;
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.root);
  for (i = g->str.mask; i != ~(MSize)0; i--)  /* Free all string hash chains. */
    gc_sweepstr(g, &g->str.tab[i]);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;
  lj_gc_stats_inc(g, atomic_calls);

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lj_assertG(!iswhite(obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  udsize = 0;

  /* All marking done, clear weak tables. */
  gc_clearweak(g, gcref(g->gc.weak));

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  setmref(g->gc.sweepudata, &mainthread(g)->nextgc);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L, GCSize lim)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.state = GCSsweepudata;
    setmref(g->gc.sweepudata, &mainthread(g)->nextgc);
    return 0;
  case GCSsweepudata:
    {
      GCSize old = g->gc.total, queued;
      setmref(g->gc.sweepudata,
	      gc_sweepudata(g, mref(g->gc.sweepudata, GCRef),
			    GCSWEEPUDATAMAX, &queued));
      lj_assertG(old >= g->gc.total, "sweep increased memory");
      g->gc.estimate -= old - g->gc.total;
      g->gc.estimate -= queued;
      if (gcref(*mref(g->gc.sweepudata, GCRef)) != NULL)
	return GCSWEEPUDATAMAX*GCSWEEPCOST;
    }
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    lj_gc_stats_inc(g, sweep_string_steps);
    gc_sweepstr(g, &g->str.tab[g->gc.sweepstr++]);  /* Sweep one chain. */
    if (g->gc.sweepstr > g->str.mask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = g->gc.total;
    lj_gc_stats_inc(g, sweep_root_steps);
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      if (g->str.num <= (g->str.mask >> 2) && g->str.mask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->str.mask >> 1);  /* Shrink string table. */
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
	g->gc.state = GCSfinalize;
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	g->gc.debt = 0;
	lj_gc_stats_inc(g, cycle_count);
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      GCSize old = g->gc.total;
      MSize finalized;
      GCSize cost;
      GCFinalizerLookup lookup;
      int contract_violation;
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      finalized = gc_finalize_direct_cfunc_batch(L, lim, &lookup,
						  &contract_violation);
      if (finalized == 0) {
	gc_finalize(L, 1, &lookup);  /* Normal GC queue after sweep/weak clearing. */
	finalized = 1;
      }
      cost = (GCSize)GCFINALIZECOST * finalized;
      if (old >= g->gc.total && g->gc.estimate > old - g->gc.total)
	g->gc.estimate -= old - g->gc.total;
      if (g->gc.estimate > cost)
	g->gc.estimate -= cost;
      return contract_violation ? LJ_MAX_MEM : cost;
    }
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    lj_gc_stats_inc(g, cycle_count);
    return 0;
  default:
    lj_assertG(0, "bad GC state");
    return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = g->vmstate;
  lj_gc_stats_inc(g, step_calls);
  setvmstate(g, GC);
  lim = g->gc.stepsize/100;
  if (g->gc.stepmul != 0 && lim > LJ_MAX_MEM/g->gc.stepmul)
    lim = LJ_MAX_MEM;
  else
    lim *= g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    lim -= (GCSize)gc_onestep(L, lim);
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < g->gc.stepsize) {
    g->gc.threshold = g->gc.total <= LJ_GC_MAXTHRESHOLD - g->gc.stepsize ?
      g->gc.total + g->gc.stepsize : LJ_GC_MAXTHRESHOLD;
    g->vmstate = ostate;
    return -1;
  } else {
    g->gc.debt -= g->gc.stepsize;
    g->gc.threshold = g->gc.total;
    g->vmstate = ostate;
    return 0;
  }
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->cur_L));
  int force;
  L->base = tvref(G(L)->jit_base);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  force = (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
  if (force)
    lj_gc_stats_inc(g, jit_forced_exits);
  return force;
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  lj_gc_stats_inc(g, fullgc_calls);
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
    setmref(g->gc.sweepudata, &g->gc.mmudata);
  }
  while (
      g->gc.state == GCSsweepudata ||
      g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L, LJ_MAX_MEM);  /* Finish sweep. */
  lj_assertG(g->gc.state == GCSfinalize || g->gc.state == GCSpause,
	     "bad GC state");
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L, LJ_MAX_MEM); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lj_assertG(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o),
	     "bad object states for forward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  lj_assertG(o->gch.gct != ~LJ_TTAB, "barrier object is not a table");
  lj_gc_stats_inc(g, barrier_forward);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  lj_gc_stats_inc(g, barrier_upvalue);
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(&uv->tv))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
		 "bad GC state");
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  lj_gc_stats_inc(g, barrier_trace);
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
}
#endif

#ifdef LUAJIT_ENABLE_GCSTATS
void lj_gc_stats_reset(global_State *g)
{
  GCStats stats = { 0 };
  g->gc.stats = stats;
}
#endif

/* -- Allocator ----------------------------------------------------------- */

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lj_assertG((osz == 0) == (p == NULL), "realloc API violation");
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lj_assertG((nsz == 0) == (p == NULL), "allocf API violation");
  lj_assertG(checkptrGC(p),
	     "allocated memory address %p outside required range", p);
  if (osz == 0) {
    lj_gc_stats_inc(g, alloc_calls);
    lj_gc_stats_add(g, alloc_bytes, nsz);
  } else if (nsz == 0) {
    lj_gc_stats_inc(g, free_calls);
    lj_gc_stats_add(g, free_bytes, osz);
  } else {
    lj_gc_stats_inc(g, realloc_calls);
    lj_gc_stats_add(g, realloc_bytes, nsz);
  }
  g->gc.total = (g->gc.total - osz) + nsz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  lj_gc_stats_inc(g, alloc_calls);
  lj_gc_stats_add(g, alloc_bytes, size);
  lj_gc_stats_inc(g, new_gcobj_calls);
  g->gc.total += size;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  return o;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}
