/*
 * write.c - writer
 *
 *   Copyright (c) 2000-2004 Shiro Kawai, All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  $Id: write.c,v 1.49 2004-11-22 23:16:05 shirok Exp $
 */

#include <stdio.h>
#include <ctype.h>
#define LIBGAUCHE_BODY
#include "gauche.h"
#include "gauche/port.h"
#include "gauche/builtin-syms.h"

static void write_walk(ScmObj obj, ScmPort *port, ScmWriteContext *ctx);
static void write_ss(ScmObj obj, ScmPort *port, ScmWriteContext *ctx);
static void write_ss_rec(ScmObj obj, ScmPort *port, ScmWriteContext *ctx);
static void write_object(ScmObj obj, ScmPort *out, ScmWriteContext *ctx);
static ScmObj write_object_fallback(ScmObj *args, int nargs, ScmGeneric *gf);
SCM_DEFINE_GENERIC(Scm_GenericWriteObject, write_object_fallback, NULL);

/*============================================================
 * Writers
 */

/* Note: all internal routine (static functions) assumes the output
   port is properly locked. */

/* Note: the current internal structure is in the transient state.
   handling of writer mode and context should be much better.
   Do not count on these APIs! */

/* Note: in order to support write/ss, we need to pass down the context
   along the call tree.  We can think of a few strategies:
   
  (a) Use separate context argument : this is logically the most natural way.
      The problem is that the legacy code didn't take the context into
      account (especially in the printer of user-defined objects).
    
  (b) Attach context information to the port : this isn't "right", because
      theoretically a user program may want to mix output of write/ss and
      other writes into a single port.  However, it isn't likely a problem,
      since (1) the outmost write() call locks the port, hence only one
      thread can write to the port during a single write/ss call, and
      (2) the purpose of write/ss is to produce an output which can be
      read back, so you don't want to mix up other output.

      Another possible drawback is the overhead of dynamic wind in the
      toplevel write() call (since we need to remove the context information
      from the port when write() exits non-locally).  If the port hasn't
      been locked, we need a C-level unwind-protect anyway, so it's not
      a problem.   If the port is already locked, extra dynamic wind may
      impact performance.

      Furthermore, I feel it isn't "right" to modify longer-living data
      (port) for the sake of local, dynamically-scoped information (context).
      
      The advantage of this method is that legacy code will work unchanged.

  (c) A variation of (b) is to "wrap" the port by a transient procedural
      port, which passes through output data to the original port, _and_
      keeps the context info.  This is clean in the sense that it doesn't
      contaminate the longer-living data (original port) by the transient
      info.  We don't need to worry about dynamic winding as well (we can
      leave the transient port to be GCed).

      The concern is the overhead of forwarding output via procedural
      port interface.

   I'm not sure which is the best way in long run; so, as a temporary
   solution, I use the strategy (b), since it is compatible to the current
   version.  Let's see how it works.
 */

#define SPBUFSIZ   50

/* Two bitmask used internally to indicate extra write mode */
#define WRITE_LIMITED   0x10    /* we're limiting the length of output. */
#define WRITE_CIRCULAR  0x20    /* circular-safe write.  info->table
                                   is set up to look up for circular
                                   objects. */

/* VM-default case mode */
#define DEFAULT_CASE \
   (SCM_VM_RUNTIME_FLAG_IS_SET(Scm_VM(), SCM_CASE_FOLD)? \
    SCM_WRITE_CASE_FOLD:SCM_WRITE_CASE_NOFOLD)

static inline int outlen(ScmPort *out)
{
    SCM_ASSERT(SCM_PORT_TYPE(out) == SCM_PORT_OSTR);
    if (out->src.ostr.length < 0) {
        return Scm_DStringSize(&out->src.ostr);
    } else {
        return out->src.ostr.length;
    }
}

/*
 * Scm_Write - Standard Write.
 */
void Scm_Write(ScmObj obj, ScmObj p, int mode)
{
    ScmWriteContext ctx;
    ScmVM *vm;
    ScmPort *port;
    
    if (!SCM_OPORTP(p)) {
        Scm_Error("output port required, but got %S", p);
    }
    port = SCM_PORT(p);
    ctx.mode = mode;
    ctx.flags = 0;

    /* if this is a "walk" pass of write/ss, dispatch to the walker */
    if (port->flags & SCM_PORT_WALKING) {
        SCM_ASSERT(SCM_PAIRP(port->data)&&SCM_HASHTABLEP(SCM_CDR(port->data)));
        write_walk(obj, port, &ctx);
        return;
    }
    /* if this is a "output" pass of write/ss, call the recursive routine */
    if (port->flags & SCM_PORT_WRITESS) {
        SCM_ASSERT(SCM_PAIRP(port->data)&&SCM_HASHTABLEP(SCM_CDR(port->data)));
        write_ss_rec(obj, port, &ctx);
        return;
    }
    
    /* if case mode is not specified, use default taken from VM default */
    if (SCM_WRITE_CASE(&ctx) == 0) ctx.mode |= DEFAULT_CASE;

    vm = Scm_VM();
    PORT_LOCK(port, vm);
    if (SCM_WRITE_MODE(&ctx) == SCM_WRITE_SHARED) {
        PORT_SAFE_CALL(port, write_ss(obj, port, &ctx));
    } else {
        PORT_SAFE_CALL(port, write_ss_rec(obj, port, &ctx));
    }
    PORT_UNLOCK(port);
}

/* 
 * Scm_WriteLimited - Write to limited length.
 *
 *  Characters exceeding WIDTH are truncated.
 *  If the output fits within WIDTH, # of characters actually written
 *  is returned.  Othewise, -1 is returned.
 * 
 *  Current implementation is sloppy, potentially wasting time to write
 *  objects which will be just discarded.
 */
int Scm_WriteLimited(ScmObj obj, ScmObj port, int mode, int width)
{
    ScmWriteContext ctx;
    ScmObj out;
    int nc;
    
    if (!SCM_OPORTP(port))
        Scm_Error("output port required, but got %S", port);
    out = Scm_MakeOutputStringPort(TRUE);
    ctx.mode = mode;
    ctx.flags = WRITE_LIMITED;
    ctx.limit = width;
    /* if case mode is not specified, use default taken from VM default */
    if (SCM_WRITE_CASE(&ctx) == 0) ctx.mode |= DEFAULT_CASE;
    /* we don't need to lock out, for it is private. */
    write_ss_rec(obj, SCM_PORT(out), &ctx);
    nc = outlen(SCM_PORT(out));
    if (nc > width) {
        ScmObj sub = Scm_Substring(SCM_STRING(Scm_GetOutputString(SCM_PORT(out))),
                                   0, width);
        SCM_PUTS(sub, port);    /* this locks port */
        return -1;
    } else {
        SCM_PUTS(Scm_GetOutputString(SCM_PORT(out)), port); /* this locks port */
        return nc;
    }
}

/*
 * Scm_WriteCircular - circular-safe writer
 */

int Scm_WriteCircular(ScmObj obj, ScmObj port, int mode, int width)
{
    ScmWriteContext ctx;
    int nc;

    if (!SCM_OPORTP(port)) {
        Scm_Error("output port required, but got %S", port);
    }
    ctx.mode = mode;
    ctx.flags = WRITE_CIRCULAR;
    if (SCM_WRITE_CASE(&ctx) == 0) ctx.mode |= DEFAULT_CASE;
    if (width > 0) {
        ctx.flags |= WRITE_LIMITED;
        ctx.limit = width;
    }
    ctx.ncirc = 0;
    ctx.table = SCM_HASHTABLE(Scm_MakeHashTable(SCM_HASH_ADDRESS, NULL, 8));

    if (width > 0) {
        ScmObj out = Scm_MakeOutputStringPort(TRUE);
        /* no need to lock out, for it is private */
        write_ss(obj, SCM_PORT(out), &ctx);
        nc = outlen(SCM_PORT(out));
        if (nc > width) {
            ScmObj sub = Scm_Substring(SCM_STRING(Scm_GetOutputString(SCM_PORT(out))),
                                       0, width);
            SCM_PUTS(sub, port); /* this locks port */
            return -1;
        } else {
            SCM_PUTS(Scm_GetOutputString(SCM_PORT(out)), port); /* this locks port */
            return nc;
        }
    } else {
        ScmVM *vm = Scm_VM();
        PORT_LOCK(SCM_PORT(port), vm);
        PORT_SAFE_CALL(SCM_PORT(port),
                       write_ss(obj, SCM_PORT(port), &ctx));
        PORT_UNLOCK(SCM_PORT(port));
    }
    return 0;
}

/*===================================================================
 * Internal writer
 */

/* character name table (first 33 chars of ASCII)*/
static const char *char_names[] = {
    "null",   "x01",   "x02",    "x03",   "x04",   "x05",   "x06",   "x07",
    "x08",    "tab",   "newline","x0b",   "x0c",   "return","x0e",   "x0f",
    "x10",    "x11",   "x12",    "x13",   "x14",   "x15",   "x16",   "x17",
    "x18",    "x19",   "x1a",    "escape","x1c",   "x1d",   "x1e",   "x1f",
    "space"
};

#define CASE_ITAG(obj, str) \
    case SCM_ITAG(obj): Scm_PutzUnsafe(str, -1, port); break;

/* Obj is PTR, except pair and vector */
static void write_general(ScmObj obj, ScmPort *out, ScmWriteContext *ctx)
{
    ScmClass *c = Scm_ClassOf(obj);
    if (c->print) c->print(obj, out, ctx); 
    else          write_object(obj, out, ctx);
}

/* Default object printer delegates print action to generic function
   write-object.   We can't use VMApply here since this function can be
   called deep in the recursive stack of Scm_Write, so the control
   may not return to VM immediately. */
static void write_object(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    Scm_Apply(SCM_OBJ(&Scm_GenericWriteObject), SCM_LIST2(obj, SCM_OBJ(port)));
}

/* Default method for write-object */
static ScmObj write_object_fallback(ScmObj *args, int nargs, ScmGeneric *gf)
{
    ScmClass *klass;
    if (nargs != 2 || (nargs == 2 && !SCM_OPORTP(args[1]))) {
        Scm_Error("No applicable method for write-object with %S",
                  Scm_ArrayToList(args, nargs));
    }
    klass = Scm_ClassOf(args[0]);
    Scm_Printf(SCM_PORT(args[1]), "#<%A %p>", klass->name, args[0]);
    return SCM_TRUE;
}

/* We need two passes to realize write/ss.

   The first pass ("walk" pass) traverses the data and finds out
   all shared substructures and/or cyclic references.  It builds a
   hash table of objects that need special treatment.

   The second pass ("output" pass) writes out the data.
   
   For the walk pass, we can't use generic traversal algorithm
   if the data contains user-defined structures.  In which case,
   we delegate the walk task to the user-defined print routine.
   In the walk pass, a special dummy port is created.  It is a
   procedural port to which all output is discarded.  If the
   user-defined routine needs to traverse substructure, it calls
   back system's writer routine such as Scm_Write, Scm_Printf, 
   so we can effectively traverse entire data to be printed.

*/

/* Dummy port for the walk pass */
static ScmPortVTable walker_port_vtable = {
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL
};

static ScmPort *make_walker_port(void)
{
    ScmPort *port;
    ScmObj ht;
                                          
    port = SCM_PORT(Scm_MakeVirtualPort(SCM_CLASS_PORT, SCM_PORT_OUTPUT,
                                        &walker_port_vtable));
    ht = Scm_MakeHashTable(SCM_HASH_ADDRESS, NULL, 0);
    port->data = Scm_Cons(SCM_MAKE_INT(0), ht);
    port->flags = SCM_PORT_WALKING;
    return port;
}

/* pass 1 */
static void write_walk(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    ScmHashEntry *e;
    ScmHashTable *ht;
    ScmObj elt;
    
    ht = SCM_HASHTABLE(SCM_CDR(port->data));

    for (;;) {
        if (!SCM_PTRP(obj) || SCM_SYMBOLP(obj) || SCM_KEYWORDP(obj)
            || SCM_NUMBERP(obj)) {
            return;
        }
            
        if (SCM_PAIRP(obj)) {
            e = Scm_HashTableGet(ht, obj);
            if (e) { e->value = SCM_TRUE; return; }
            Scm_HashTablePut(ht, obj, SCM_FALSE);

            elt = SCM_CAR(obj);
            if (SCM_PTRP(elt)) write_walk(SCM_CAR(obj), port, ctx);
            obj = SCM_CDR(obj);
            continue;
        }
        if (SCM_STRINGP(obj) && SCM_STRING_SIZE(obj) > 0) {
            e = Scm_HashTableGet(ht, obj);
            if (e) { e->value = SCM_TRUE; return; }
            Scm_HashTablePut(ht, obj, SCM_FALSE);
            return;
        }
        if (SCM_VECTORP(obj) && SCM_VECTOR_SIZE(obj) > 0) {
            int i, len = SCM_VECTOR_SIZE(obj);

            e = Scm_HashTableGet(ht, obj);
            if (e) { e->value = SCM_TRUE; return; }
            Scm_HashTablePut(ht, obj, SCM_FALSE);

            for (i=0; i<len; i++) {
                elt = SCM_VECTOR_ELEMENT(obj, i);
                if (SCM_PTRP(elt)) write_walk(elt, port, ctx);
            }
            return;
        }
        else {
            /* Now we have user-defined object.
               Call the user's print routine. */
            ScmClass *c;
            e = Scm_HashTableGet(ht, obj);
            if (e) { e->value = SCM_TRUE; return; }
            Scm_HashTablePut(ht, obj, SCM_FALSE);

            write_general(obj, port, ctx);
            return;
        }
    }
}

/* pass 2 */
static void write_ss_rec(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    ScmHashEntry *e;
    char numbuf[50];  /* enough to contain long number */
    ScmHashTable *ht = NULL;

    if (ctx->flags & WRITE_LIMITED) {
        if (outlen(port) >= ctx->limit) return;
    }

    if (SCM_PAIRP(port->data) && SCM_HASHTABLEP(SCM_CDR(port->data))) {
        ht = SCM_HASHTABLE(SCM_CDR(port->data));
    }

    if (!SCM_PTRP(obj)) {
        if (SCM_IMMEDIATEP(obj)) {
            switch (SCM_ITAG(obj)) {
                CASE_ITAG(SCM_FALSE,     "#f");
                CASE_ITAG(SCM_TRUE,      "#t");
                CASE_ITAG(SCM_NIL,       "()");
                CASE_ITAG(SCM_EOF,       "#<eof>");
                CASE_ITAG(SCM_UNDEFINED, "#<undef>");
                CASE_ITAG(SCM_UNBOUND,   "#<unbound>");
            default:
                Scm_Panic("write: unknown itag object: %08x", SCM_WORD(obj));
            }
        }
        else if (SCM_INTP(obj)) {
            char buf[SPBUFSIZ];
            snprintf(buf, SPBUFSIZ, "%ld", SCM_INT_VALUE(obj));
            Scm_PutzUnsafe(buf, -1, port);
        }
        else if (SCM_CHARP(obj)) {
            ScmChar ch = SCM_CHAR_VALUE(obj);
            if (SCM_WRITE_MODE(ctx) == SCM_WRITE_DISPLAY) {
                Scm_PutcUnsafe(ch, port);
            } else {
                Scm_PutzUnsafe("#\\", -1, port);
                if (ch <= 0x20)       Scm_PutzUnsafe(char_names[ch], -1, port);
                else if (ch == 0x7f)  Scm_PutzUnsafe("del", -1, port);
                else                  Scm_PutcUnsafe(ch, port);
            }
        }
        else if (SCM_VM_INSNP(obj)) {
            Scm__VMInsnWrite(obj, port, ctx);
        }
        else Scm_Panic("write: got a bogus object: %08x", SCM_WORD(obj));
        return;
    }
    if (SCM_NUMBERP(obj)) {
        /* number may be heap allocated, but we don't use srfi-38 notation. */
        write_general(obj, port, ctx);
        return;
    }
    
    if ((SCM_STRINGP(obj) && SCM_STRING_SIZE(obj) == 0)
        || (SCM_VECTORP(obj) && SCM_VECTOR_SIZE(obj) == 0)) {
        /* special case where we don't put a reference tag. */
        write_general(obj, port, ctx);
        return;
    }

    if (ht) {
        e = Scm_HashTableGet(ht, obj);
        if (e && e->value != SCM_FALSE) {
            if (SCM_INTP(e->value)) {
                /* This object is already printed. */
                snprintf(numbuf, 50, "#%ld#", SCM_INT_VALUE(e->value));
                Scm_PutzUnsafe(numbuf, -1, port);
                return;
            } else {
                /* This object will be seen again. Put a reference tag. */
                int count = SCM_INT_VALUE(SCM_CAR(port->data));
                snprintf(numbuf, 50, "#%d=", count);
                e->value = SCM_MAKE_INT(count);
                SCM_SET_CAR(port->data, SCM_MAKE_INT(count+1));
                Scm_PutzUnsafe(numbuf, -1, port);
            }
        }
    }

    /* Writes aggregates */
    if (SCM_PAIRP(obj)) {
        /* special case for quote etc.*/
        if (SCM_PAIRP(SCM_CDR(obj)) && SCM_NULLP(SCM_CDDR(obj))) {
            int special = TRUE;
            if (SCM_CAR(obj) == SCM_SYM_QUOTE) {
                Scm_PutcUnsafe('\'', port);
            } else if (SCM_CAR(obj) == SCM_SYM_QUASIQUOTE) {
                Scm_PutcUnsafe('`', port);
            } else if (SCM_CAR(obj) == SCM_SYM_UNQUOTE) {
                Scm_PutcUnsafe(',', port);
            } else if (SCM_CAR(obj) == SCM_SYM_UNQUOTE_SPLICING) {
                Scm_PutzUnsafe(",@", -1, port);
            } else {
                special = FALSE;
            }
            if (special) {
                write_ss_rec(SCM_CADR(obj), port, ctx);
                return;
            }
        }
        
        /* normal case */
        Scm_PutcUnsafe('(', port);
        for (;;) {

            write_ss_rec(SCM_CAR(obj), port, ctx);
        
            obj = SCM_CDR(obj);
            if (SCM_NULLP(obj)) { Scm_PutcUnsafe(')', port); return; }
            if (!SCM_PAIRP(obj)) {
                Scm_PutzUnsafe(" . ", -1, port);
                write_ss_rec(obj, port, ctx);
                Scm_PutcUnsafe(')', port);
                return;
            }
            if (ht) {
                e = Scm_HashTableGet(ht, obj); /* check for shared cdr */
                if (e && e->value != SCM_FALSE) {
                    Scm_PutzUnsafe(" . ", -1, port);
                    write_ss_rec(obj, port, ctx);
                    Scm_PutcUnsafe(')', port);
                    return;
                }
            }
            Scm_PutcUnsafe(' ', port);
        }
    } else if (SCM_VECTORP(obj)) {
        int len, i;
        ScmObj *elts;
        
        Scm_PutzUnsafe("#(", -1, port);
        len = SCM_VECTOR_SIZE(obj);
        elts = SCM_VECTOR_ELEMENTS(obj);
        for (i=0; i<len-1; i++) {
            write_ss_rec(elts[i], port, ctx);
            Scm_PutcUnsafe(' ', port);
        }
        write_ss_rec(elts[i], port, ctx);
        Scm_PutcUnsafe(')', port);
    } else {
        /* string or user-defined object */
        write_general(obj, port, ctx);
    }
}

/* Write/ss main driver
   NB: this should never be called recursively. */
static void write_ss(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    ScmPort *walker_port = make_walker_port();
    
    /* pass 1 */
    write_walk(obj, walker_port, ctx);
    Scm_ClosePort(walker_port);

    /* pass 2 */
    /* TODO: we need to rewind port mode */
    port->data = walker_port->data;
    port->flags |= SCM_PORT_WRITESS;
    write_ss_rec(obj, port, ctx);
    port->data = SCM_FALSE;
    port->flags &= ~SCM_PORT_WRITESS;
}

/*===================================================================
 * Formatters
 */

/* TODO: provide option to compile format string. */

#define NEXT_ARG(arg, args)                                             \
    do {                                                                \
        if (!SCM_PAIRP(args))                                           \
            Scm_Error("too few arguments for format string: %S", fmt);  \
        arg = SCM_CAR(args);                                            \
        args = SCM_CDR(args);                                           \
        argcnt++;                                                       \
    } while (0)

/* max # of parameters for a format directive */
#define MAX_PARAMS 5

/* dispatch to proper writer */
static void format_write(ScmObj obj, ScmPort *port, ScmWriteContext *ctx,
                         int sharedp)
{
    if (port->flags & SCM_PORT_WALKING) {
        SCM_ASSERT(SCM_PAIRP(port->data)&&SCM_HASHTABLEP(SCM_CDR(port->data)));
        write_walk(obj, port, ctx);
        return;
    }
    if (port->flags & SCM_PORT_WRITESS) {
        SCM_ASSERT(SCM_PAIRP(port->data)&&SCM_HASHTABLEP(SCM_CDR(port->data)));
        write_ss_rec(obj, port, ctx);
        return;
    }
    if (sharedp) {
        write_ss(obj, port, ctx);
    } else {
        write_ss_rec(obj, port, ctx);
    }
}

/* output string with padding */
static void format_pad(ScmPort *out, ScmString *str,
                       int mincol, int colinc, ScmChar padchar,
                       int rightalign)
{
    int padcount = mincol- SCM_STRING_LENGTH(str);
    int i;
    
    if (padcount > 0) {
        if (colinc > 1) {
            padcount = ((padcount+colinc-1)/colinc)*colinc;
        }
        if (rightalign) {
            for (i=0; i<padcount; i++) Scm_PutcUnsafe(padchar, out);
        }
        Scm_PutsUnsafe(str, SCM_PORT(out));
        if (!rightalign) {
            for (i=0; i<padcount; i++) Scm_PutcUnsafe(padchar, out);
        }
    } else {
        Scm_PutsUnsafe(str, out);
    }
}

/* ~s and ~a writer */
static void format_sexp(ScmPort *out, ScmObj arg,
                        ScmObj *params, int nparams,
                        int rightalign, int dots, int mode)
{
    int mincol = 0, colinc = 1, minpad = 0, maxcol = -1, nwritten = 0, i;
    ScmChar padchar = ' ';
    ScmObj tmpout = Scm_MakeOutputStringPort(TRUE);
    ScmString *tmpstr;

    if (nparams>0 && SCM_INTP(params[0])) mincol = SCM_INT_VALUE(params[0]);
    if (nparams>1 && SCM_INTP(params[1])) colinc = SCM_INT_VALUE(params[1]);
    if (nparams>2 && SCM_INTP(params[2])) minpad = SCM_INT_VALUE(params[2]);
    if (nparams>3 && SCM_CHARP(params[3])) padchar = SCM_CHAR_VALUE(params[3]);
    if (nparams>4 && SCM_INTP(params[4])) maxcol = SCM_INT_VALUE(params[4]);

    if (minpad > 0 && rightalign) {
        for (i=0; i<minpad; i++) Scm_PutcUnsafe(padchar, SCM_PORT(tmpout));
    }
    if (maxcol > 0) {
        nwritten = Scm_WriteLimited(arg, tmpout, mode, maxcol);
    } else {
        Scm_Write(arg, tmpout, mode);
    }
    if (minpad > 0 && !rightalign) {
        for (i=0; i<minpad; i++) Scm_PutcUnsafe(padchar, SCM_PORT(tmpout));
    }
    tmpstr = SCM_STRING(Scm_GetOutputString(SCM_PORT(tmpout)));

    if (maxcol > 0 && nwritten < 0) {
        const char *s = SCM_STRING_START(tmpstr), *e;
        if (dots && maxcol > 4) {
            e = Scm_StringPosition(tmpstr, maxcol-4);
            Scm_PutzUnsafe(s, e-s, out);
            Scm_PutzUnsafe(" ...", 4, out);
        } else {
            e = Scm_StringPosition(tmpstr, maxcol);
            Scm_PutzUnsafe(s, e-s, out);
        }
    } else {
        format_pad(out, tmpstr, mincol, colinc, padchar, rightalign);
    }
}

/* ~d, ~b, ~o, and ~x */
static void format_integer(ScmPort *out, ScmObj arg,
                           ScmObj *params, int nparams, int radix,
                           int delimited, int alwayssign, int use_upper)
{
    int mincol = 0, commainterval = 3;
    ScmChar padchar = ' ', commachar = ',';
    ScmObj str;
    if (!Scm_IntegerP(arg)) {
        /* if arg is not an integer, use ~a */
        ScmWriteContext ictx;
        ictx.mode = SCM_WRITE_DISPLAY;
        ictx.flags = 0;
        format_write(arg, out, &ictx, FALSE);
        return;
    }
    if (SCM_FLONUMP(arg)) arg = Scm_InexactToExact(arg);
    if (nparams>0 && SCM_INTP(params[0])) mincol = SCM_INT_VALUE(params[0]);
    if (nparams>1 && SCM_CHARP(params[1])) padchar = SCM_CHAR_VALUE(params[1]);
    if (nparams>2 && SCM_CHARP(params[2])) commachar = SCM_CHAR_VALUE(params[2]);
    if (nparams>3 && SCM_INTP(params[3])) commainterval = SCM_INT_VALUE(params[3]);
    str = Scm_NumberToString(arg, radix, use_upper);
    if (alwayssign && SCM_STRING_START(str)[0] != '-') {
        str = Scm_StringAppend2(SCM_STRING(SCM_MAKE_STR("+")),
                                SCM_STRING(str));
    }
    if (delimited && commainterval) {
        /* Delimited output.  We use char*, for str never contains
           mbchar. */
        /* NB: I think the specification of delimited behavior in CLtL2
           contradicts its examples; it is ambiguous about what happens
           if the number is padded. */
        ScmDString tmpout;
        const char *ptr = SCM_STRING_START(str);
        int num_digits = SCM_STRING_LENGTH(str), colcnt;

        Scm_DStringInit(&tmpout);
        if (*ptr == '-' || *ptr == '+') {
            Scm_DStringPutc(&tmpout, *ptr);
            ptr++;
            num_digits--;
        }
        colcnt = num_digits % commainterval;
        if (colcnt != 0) Scm_DStringPutz(&tmpout, ptr, colcnt);
        while (colcnt < num_digits) {
            if (colcnt != 0) Scm_DStringPutc(&tmpout, commachar);
            Scm_DStringPutz(&tmpout, ptr+colcnt, commainterval);
            colcnt += commainterval;
        }
        str = Scm_DStringGet(&tmpout);
    }
    format_pad(out, SCM_STRING(str), mincol, 1, padchar, TRUE);
}

static void format_proc(ScmPort *out, ScmString *fmt, ScmObj args, int sharedp)
{
    ScmChar ch = 0;
    ScmObj arg, oargs = args;
    ScmPort *fmtstr = SCM_PORT(Scm_MakeInputStringPort(fmt, FALSE));
    int backtracked = FALSE;    /* true if ~:* is used */
    int arglen, argcnt;
    ScmWriteContext sctx, actx; /* context for ~s and ~a */

    arglen = Scm_Length(args);
    argcnt = 0;

    sctx.mode = SCM_WRITE_WRITE;
    sctx.flags = 0;
    actx.mode = SCM_WRITE_DISPLAY;
    actx.flags = 0;
    
    for (;;) {
        int atflag, colonflag;
        ScmObj params[MAX_PARAMS];
        int numParams;
        
        ch = Scm_GetcUnsafe(fmtstr);
        if (ch == EOF) {
            if (!backtracked && !SCM_NULLP(args)) {
                Scm_Error("too many arguments for format string: %S", fmt);
            }
            return;
        }

        if (ch != '~') {
            Scm_PutcUnsafe(ch, out);
            continue;
        }

        numParams = 0;
        atflag = colonflag = FALSE;
        
        for (;;) {
            ch = Scm_GetcUnsafe(fmtstr);
            switch (ch) {
            case '%':
                Scm_PutcUnsafe('\n', out);
                break;
            case 's':; case 'S':;
                NEXT_ARG(arg, args);
                if (numParams == 0) {
                    format_write(arg, out, &sctx, sharedp);
                } else {
                    format_sexp(out, arg, params, numParams, atflag,
                                colonflag, SCM_WRITE_WRITE);
                }
                break;
            case 'a':; case 'A':;
                NEXT_ARG(arg, args);
                if (numParams == 0) {
                    /* short path */
                    format_write(arg, out, &actx, sharedp);
                } else {
                    format_sexp(out, arg, params, numParams, atflag,
                                colonflag, SCM_WRITE_DISPLAY);
                }
                break;
            case 'd':; case 'D':;
                NEXT_ARG(arg, args);
                if (numParams == 0 && !atflag && !colonflag) {
                    format_write(arg, out, &actx, FALSE);
                } else {
                    format_integer(out, arg, params, numParams, 10,
                                   colonflag, atflag, FALSE);
                }
                break;
            case 'b':; case 'B':;
                NEXT_ARG(arg, args);
                if (numParams == 0 && !atflag && !colonflag) {
                    if (Scm_IntegerP(arg)) {
                        format_write(Scm_NumberToString(arg, 2, FALSE), out,
                                     &actx, FALSE);
                    } else {
                        format_write(arg, out, &actx, FALSE);
                    }
                } else {
                    format_integer(out, arg, params, numParams, 2,
                                   colonflag, atflag, FALSE);
                }
                break;
            case 'o':; case 'O':;
                NEXT_ARG(arg, args);
                if (numParams == 0 && !atflag && !colonflag) {
                    if (Scm_IntegerP(arg)) {
                        format_write(Scm_NumberToString(arg, 8, FALSE), out,
                                     &actx, FALSE);
                    } else {
                        format_write(arg, out, &actx, FALSE);
                    }
                } else {
                    format_integer(out, arg, params, numParams, 8,
                                   colonflag, atflag, FALSE);
                }
                break;
            case 'x':; case 'X':;
                NEXT_ARG(arg, args);
                if (numParams == 0 && !atflag && !colonflag) {
                    if (Scm_IntegerP(arg)) {
                        format_write(Scm_NumberToString(arg, 16, ch == 'X'),
                                     out, &actx, FALSE);
                    } else {
                        format_write(arg, out, &actx, FALSE);
                    }
                } else {
                    format_integer(out, arg, params, numParams, 16,
                                   colonflag, atflag, ch == 'X');
                }
                break;
            case '*':
                {
                    int argindex;
                    if (numParams) {
                        if (!SCM_INTP(params[0])) goto badfmt;
                        argindex = SCM_INT_VALUE(params[0]);
                    } else {
                        argindex = 1;
                    }
                    if (colonflag) {
                        if (atflag) goto badfmt;
                        argindex = argcnt - argindex;
                        backtracked = TRUE;
                    } else if (!atflag) {
                        argindex = argcnt + argindex;
                    } else {
                        backtracked = TRUE;
                    }
                    if (argindex < 0 || argindex >= arglen) {
                        Scm_Error("'~*' format directive refers outside of argument list in %S", fmt);
                    }
                    argcnt = argindex;
                    args = Scm_ListTail(oargs, argcnt);
                    break;
                }
            case 'v':; case 'V':;
                if (atflag || colonflag || numParams >= MAX_PARAMS)
                    goto badfmt;
                NEXT_ARG(arg, args);
                if (!SCM_FALSEP(arg) && !SCM_INTP(arg) && !SCM_CHARP(arg)) {
                    Scm_Error("argument for 'v' format parameter in %S should be either an integer, a character or #f, but got %S",
                              fmt, arg);
                }
                params[numParams++] = arg;
                ch = Scm_GetcUnsafe(fmtstr);
                if (ch != ',') Scm_UngetcUnsafe(ch, fmtstr);
                continue;
            case '@':
                if (atflag) {
                    Scm_Error("too many @-flag for formatting directive: %S",
                              fmt);
                }
                atflag = TRUE;
                continue;
            case ':':
                if (colonflag) {
                    Scm_Error("too many :-flag for formatting directive: %S",
                              fmt);
                }
                colonflag = TRUE;
                continue;
            case '\'':
                if (atflag || colonflag) goto badfmt;
                if (numParams >= MAX_PARAMS) goto badfmt;
                ch = Scm_GetcUnsafe(fmtstr);
                if (ch == EOF) goto badfmt;
                params[numParams++] = SCM_MAKE_CHAR(ch);
                ch = Scm_GetcUnsafe(fmtstr);
                if (ch != ',') Scm_UngetcUnsafe(ch, fmtstr);
                continue;
            case '0':; case '1':; case '2':; case '3':; case '4':;
            case '5':; case '6':; case '7':; case '8':; case '9':;
            case '-':; case '+':;
                if (atflag || colonflag || numParams >= MAX_PARAMS) {
                    goto badfmt;
                } else {
                    int sign = (ch == '-')? -1 : 1;
                    unsigned long value = isdigit(ch)? (ch - '0') : 0;
                    for (;;) {
                        ch = Scm_GetcUnsafe(fmtstr);
                        /* TODO: check valid character */
                        if (!isdigit(ch)) {
                            if (ch != ',') Scm_UngetcUnsafe(ch, fmtstr);
                            params[numParams++] = Scm_MakeInteger(sign*value);
                            break;
                        }
                        /* TODO: check overflow */
                        value = value * 10 + (ch - '0');
                    }
                }
                continue;
            case ',':
                if (atflag || colonflag || numParams >= MAX_PARAMS) {
                    goto badfmt;
                } else {
                    params[numParams++] = SCM_FALSE;
                    continue;
                }
            default:
                Scm_PutcUnsafe(ch, out);
                break;
            }
            break;
        }
    }
  badfmt:
    Scm_Error("illegal format string: %S", fmt);
    return;       /* dummy */
}

void Scm_Format(ScmPort *out, ScmString *fmt, ScmObj args, int sharedp)
{
    ScmVM *vm;
    
    if (!SCM_OPORTP(out)) {
        Scm_Error("output port required, but got %S", out);
    }

    vm = Scm_VM();
    PORT_LOCK(out, vm);
    PORT_SAFE_CALL(out, format_proc(SCM_PORT(out), fmt, args, sharedp));
    PORT_UNLOCK(out);
}

/*
 * Printf()-like formatters
 *
 *  These functions are familiar to C-programmers.   The differences
 *  from C's printf() family are:
 *
 *    - The first argument must be Scheme output port.
 *    - In the format string, the following conversion directives can
 *      be used, as well as the standard printf() directives:
 * 
 *        %[width][.prec]S    - The corresponding argument must be
 *                              ScmObj, which is written out by WRITE
 *                              mode.  If width is specified and no
 *                              prec is given, the output is padded
 *                              if it is shorter than width.  If both
 *                              width and prec are given, the output
 *                              is truncated if it is wider than width.
 *
 *        %[width][.prec]A    - Same as %S, but use DISPLAY mode.
 *
 *        %C                  - Take ScmChar argument and outputs it.
 *
 *  Both functions return a number of characters written.
 */

struct vprintf_ctx {
    const char *fmt;
    ScmObj args;
};

/* NB: Scm_Vprintf scans format string twice.  In the first pass, arguments
 * are retrieved from va_list variable and pushed to a list.  In the second
 * pass, they are printed according to the format string.
 * It is necessary because we need to do the printing part within a closure
 * called by Scm_WithPortLocking.  On some architecture, we can't pass
 * va_list type of argument in a closure packet easily.
 */

static void vprintf_proc(ScmPort *out, const char *fmt, ScmObj args,
                         int sharedp)
{
    const char *fmtp = fmt;
    ScmObj val;
    ScmDString argbuf;
    char buf[SPBUFSIZ];
    int c, longp = 0, len, mode;

    while ((c = *fmtp++) != 0) {
        int width, prec, dot_appeared, pound_appeared;

        if (c != '%') {
            Scm_PutcUnsafe(c, out);
            continue;
        }

        Scm_DStringInit(&argbuf);
        SCM_DSTRING_PUTB(&argbuf, c);
        width = 0, prec = 0, dot_appeared = 0, pound_appeared = 0;
        while ((c = *fmtp++) != 0) {
            switch (c) {
            case 'l':
                longp++;
                SCM_DSTRING_PUTB(&argbuf, c);
                continue;
            case 'd':; case 'i':; case 'c':
                {
                    SCM_ASSERT(SCM_PAIRP(args));
                    val = SCM_CAR(args);
                    args = SCM_CDR(args);
                    SCM_ASSERT(SCM_EXACTP(val));
                    SCM_DSTRING_PUTB(&argbuf, c);
                    SCM_DSTRING_PUTB(&argbuf, 0);
                    snprintf(buf, SPBUFSIZ, Scm_DStringGetz(&argbuf),
                             Scm_GetInteger(val));
                    Scm_PutzUnsafe(buf, -1, out);
                    break;
                }
            case 'o':; case 'u':; case 'x':; case 'X':
                {
                    SCM_ASSERT(SCM_PAIRP(args));
                    val = SCM_CAR(args);
                    args = SCM_CDR(args);
                    SCM_ASSERT(SCM_EXACTP(val));
                    SCM_DSTRING_PUTB(&argbuf, c);
                    SCM_DSTRING_PUTB(&argbuf, 0);
                    snprintf(buf, SPBUFSIZ, Scm_DStringGetz(&argbuf),
                             Scm_GetUInteger(val));
                    Scm_PutzUnsafe(buf, -1, out);
                    break;
                }
            case 'e':; case 'E':; case 'f':; case 'g':; case 'G':
                {
                    SCM_ASSERT(SCM_PAIRP(args));
                    val = SCM_CAR(args);
                    args = SCM_CDR(args);
                    SCM_ASSERT(SCM_FLONUMP(val));
                    SCM_DSTRING_PUTB(&argbuf, c);
                    SCM_DSTRING_PUTB(&argbuf, 0);
                    snprintf(buf, SPBUFSIZ, Scm_DStringGetz(&argbuf),
                             Scm_GetDouble(val));
                    Scm_PutzUnsafe(buf, -1, out);
                    break;
                }
            case 's':;
                {
                    SCM_ASSERT(SCM_PAIRP(args));
                    val = SCM_CAR(args);
                    args = SCM_CDR(args);
                    SCM_ASSERT(SCM_STRINGP(val));
                    Scm_PutsUnsafe(SCM_STRING(val), out);
                    
                    /* TODO: support right adjustment such as %-10s.
                       Currently we ignore minus sign and pad chars
                       on the right. */
                    for (len = SCM_STRING_LENGTH(val); len < width; len++) {
                        Scm_PutcUnsafe(' ', out);
                    }
                    break;
                }
            case '%':;
                {
                    Scm_PutcUnsafe('%', out);
                    break;
                }
            case 'p':
                {
                    SCM_ASSERT(SCM_PAIRP(args));
                    val = SCM_CAR(args);
                    args = SCM_CDR(args);
                    SCM_ASSERT(SCM_EXACTP(val));
                    SCM_DSTRING_PUTB(&argbuf, c);
                    SCM_DSTRING_PUTB(&argbuf, 0);
                    snprintf(buf, SPBUFSIZ, Scm_DStringGetz(&argbuf),
                             (void*)Scm_GetUInteger(val));
                    Scm_PutzUnsafe(buf, -1, out);
                    break;
                }
            case 'S':; case 'A':
                {
                    ScmWriteContext wctx;

                    SCM_ASSERT(SCM_PAIRP(args));
                    val = SCM_CAR(args);
                    args = SCM_CDR(args);

                    mode = (c == 'A')? SCM_WRITE_DISPLAY : SCM_WRITE_WRITE;
                    wctx.mode = mode | DEFAULT_CASE;
                    wctx.flags = 0;

                    if (pound_appeared) {
                        int n = Scm_WriteCircular(val, SCM_OBJ(out), mode, width);
                        if (n < 0 && prec > 0) {
                            Scm_PutzUnsafe(" ...", -1, out);
                        }
                        if (n > 0) {
                            for (; n < prec; n++) Scm_PutcUnsafe(' ', out);
                        }
                    } else if (width == 0) {
                        format_write(val, out, &wctx, sharedp);
                    } else if (dot_appeared) {
                        int n = Scm_WriteLimited(val, SCM_OBJ(out), mode, width);
                        if (n < 0 && prec > 0) {
                            Scm_PutzUnsafe(" ...", -1, out);
                        }
                        if (n > 0) {
                            for (; n < prec; n++) Scm_PutcUnsafe(' ', out);
                        }
                    } else {
                        format_write(val, out, &wctx, sharedp);
                    }
                    break;
                }
            case 'C':
                {
                    SCM_ASSERT(SCM_PAIRP(args));
                    val = SCM_CAR(args);
                    args = SCM_CDR(args);
                    SCM_ASSERT(SCM_EXACTP(val));
                    Scm_PutcUnsafe(Scm_GetInteger(val), out);
                    break;
                }
            case '0':; case '1':; case '2':; case '3':; case '4':;
            case '5':; case '6':; case '7':; case '8':; case '9':
                if (dot_appeared) {
                    prec = prec*10 + (c - '0');
                } else {
                    width = width*10 + (c - '0');
                }
                goto fallback;
            case '.':
                dot_appeared++;
                goto fallback;
            case '#':
                pound_appeared++;
                goto fallback;
            fallback:
            default:
                SCM_DSTRING_PUTB(&argbuf, c);
                continue;
            }
            break;
        }
        if (c == 0) {
            Scm_Error("incomplete %-directive in format string: %s", fmt);
        }
    }
}

void Scm_Vprintf(ScmPort *out, const char *fmt, va_list ap, int sharedp)
{
    ScmObj h = SCM_NIL, t = SCM_NIL;
    const char *fmtp = fmt;
    ScmVM *vm;
    int c;
    
    if (!SCM_OPORTP(out)) {
        Scm_Error("output port required, but got %S", out);
    }
    /*
     * First pass : pop vararg and make a list of arguments.
     */
    while ((c = *fmtp++) != 0) {
        if (c != '%') continue;
        while ((c = *fmtp++) != 0) {
            switch (c) {
            case 'd':; case 'i':; case 'c':
                {
                    signed int val = va_arg(ap, signed int);
                    SCM_APPEND1(h, t, Scm_MakeInteger(val));
                    break;
                }
            case 'o':; case 'u':; case 'x':; case 'X':
                {
                    unsigned long val = va_arg(ap, unsigned long);
                    SCM_APPEND1(h, t, Scm_MakeIntegerFromUI(val));
                    break;
                }
            case 'e':; case 'E':; case 'f':; case 'g':; case 'G':
                {
                    double val = va_arg(ap, double);
                    SCM_APPEND1(h, t, Scm_MakeFlonum(val));
                    break;
                }
            case 's':;
                {
                    char *val = va_arg(ap, char *);
                    /* for safety */
                    if (val != NULL) SCM_APPEND1(h, t, SCM_MAKE_STR(val));
                    else SCM_APPEND1(h, t, SCM_MAKE_STR("(null)"));
                    break;
                }
            case '%':;
                {
                    break;
                }
            case 'p':
                {
                    void *val = va_arg(ap, void *);
                    SCM_APPEND1(h, t, Scm_MakeIntegerFromUI((unsigned long)val));
                    break;
                }
            case 'S':; case 'A':
                {
                    ScmObj o = va_arg(ap, ScmObj);
                    SCM_APPEND1(h, t, o);
                    break;
                }
            case 'C':
                {
                    int c = va_arg(ap, int);
                    SCM_APPEND1(h, t, Scm_MakeInteger(c));
                    break;
                }
            default:
                continue;
            }
            break;
        }
        if (c == 0) {
            Scm_Error("incomplete %-directive in format string: %s", fmt);
        }
    }
    /*
     * Second pass is called while locking the port.
     */
    vm = Scm_VM();
    PORT_LOCK(out, vm);
    PORT_SAFE_CALL(out, vprintf_proc(out, fmt, h, sharedp));
    PORT_UNLOCK(out);
}

void Scm_Printf(ScmPort *out, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    Scm_Vprintf(out, fmt, ap, FALSE);
    va_end(ap);
}

void Scm_PrintfShared(ScmPort *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Scm_Vprintf(out, fmt, ap, TRUE);
    va_end(ap);
}

/*
 * Initialization
 */
void Scm__InitWrite(void)
{
    Scm_InitBuiltinGeneric(&Scm_GenericWriteObject, "write-object",
                           Scm_GaucheModule());
}
