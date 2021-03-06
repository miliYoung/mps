/* poolmfs.c: MANUAL FIXED SMALL UNIT POOL
 *
 * $Id$
 * Copyright (c) 2001-2018 Ravenbrook Limited.  See end of file for license.
 *
 * This is the implementation of the MFS pool class.
 *
 * See design.mps.poolmfs.
 *
 * .restriction: This pool cannot allocate from the arena control
 * pool (as the control pool is an instance of PoolClassMV and MV uses
 * MFS in its implementation), nor can it allocate sub-pools, as that
 * causes allocation in the control pool.
 *
 * Notes
 *
 * .freelist.fragments: The simple freelist policy might lead to poor
 * locality of allocation if the list gets fragmented.
 *
 * .buffer.not: This pool doesn't support fast cache allocation, which
 * is a shame.
 */

#include "mpscmfs.h"
#include "dbgpool.h"
#include "poolmfs.h"
#include "mpm.h"

SRCID(poolmfs, "$Id$");


/* ROUND -- Round up
 *
 *  Rounds n up to the nearest multiple of unit.
 */

#define ROUND(unit, n)  ((n)+(unit)-1 - ((n)+(unit)-1)%(unit))


/* HeaderStruct -- Freelist structure */

typedef struct MFSHeaderStruct {
  struct MFSHeaderStruct *next;
} HeaderStruct, *Header;


#define UNIT_MIN        sizeof(HeaderStruct)


/* MFSVarargs -- decode obsolete varargs */

static void MFSVarargs(ArgStruct args[MPS_ARGS_MAX], va_list varargs)
{
  args[0].key = MPS_KEY_EXTEND_BY;
  args[0].val.size = va_arg(varargs, Size);
  args[1].key = MPS_KEY_MFS_UNIT_SIZE;
  args[1].val.size = va_arg(varargs, Size);
  args[2].key = MPS_KEY_ARGS_END;
  AVERT(ArgList, args);
}

ARG_DEFINE_KEY(MFS_UNIT_SIZE, Size);
ARG_DEFINE_KEY(MFSExtendSelf, Bool);

static Res MFSInit(Pool pool, Arena arena, PoolClass klass, ArgList args)
{
  Size extendBy = MFS_EXTEND_BY_DEFAULT;
  Bool extendSelf = TRUE;
  Size unitSize, ringSize, minExtendBy;
  MFS mfs;
  ArgStruct arg;
  Res res;

  AVER(pool != NULL);
  AVERT(Arena, arena);
  AVERT(ArgList, args);
  UNUSED(klass); /* used for debug pools only */
  
  ArgRequire(&arg, args, MPS_KEY_MFS_UNIT_SIZE);
  unitSize = arg.val.size;
  if (ArgPick(&arg, args, MPS_KEY_EXTEND_BY))
    extendBy = arg.val.size;
  if (ArgPick(&arg, args, MFSExtendSelf))
    extendSelf = arg.val.b;

  AVER(unitSize > 0);
  AVER(extendBy > 0);
  AVERT(Bool, extendSelf);

  res = NextMethod(Pool, MFSPool, init)(pool, arena, klass, args);
  if (res != ResOK)
    goto failNextInit;
  mfs = CouldBeA(MFSPool, pool);

  mfs->unroundedUnitSize = unitSize;

  if (unitSize < UNIT_MIN)
    unitSize = UNIT_MIN;
  unitSize = SizeAlignUp(unitSize, MPS_PF_ALIGN);
  ringSize = SizeAlignUp(sizeof(RingStruct), MPS_PF_ALIGN);
  minExtendBy = ringSize + unitSize;
  if (extendBy < minExtendBy)
    extendBy = minExtendBy;

  extendBy = SizeArenaGrains(extendBy, arena);

  mfs->extendBy = extendBy;
  mfs->extendSelf = extendSelf;
  mfs->unitSize = unitSize;
  mfs->freeList = NULL;
  RingInit(&mfs->extentRing);
  mfs->total = 0;
  mfs->free = 0;

  SetClassOfPoly(pool, CLASS(MFSPool));
  mfs->sig = MFSSig;
  AVERC(MFS, mfs);

  EVENT4(PoolInitMFS, pool, extendBy, BOOLOF(extendSelf), unitSize);
  return ResOK;

failNextInit:
  AVER(res != ResOK);
  return res;
}


void MFSFinishExtents(Pool pool, MFSExtentVisitor visitor,
                      void *closure)
{
  MFS mfs = MustBeA(MFSPool, pool);
  Ring ring, node, next;

  AVER(FUNCHECK(visitor));
  /* Can't check closure */

  ring = &mfs->extentRing;
  node = RingNext(ring);
  RING_FOR(node, ring, next) {
    Addr base = (Addr)node;     /* See .ring-node.at-base. */
    RingRemove(node);
    visitor(pool, base, mfs->extendBy, closure);
  }
}


static void MFSExtentFreeVisitor(Pool pool, Addr base, Size size,
                                 void *closure)
{
  AVER(closure == UNUSED_POINTER);
  UNUSED(closure);
  ArenaFree(base, size, pool);
}


static void MFSFinish(Inst inst)
{
  Pool pool = MustBeA(AbstractPool, inst);
  MFS mfs = MustBeA(MFSPool, pool);

  MFSFinishExtents(pool, MFSExtentFreeVisitor, UNUSED_POINTER);

  mfs->sig = SigInvalid;

  NextMethod(Inst, MFSPool, finish)(inst);
}


void MFSExtend(Pool pool, Addr base, Addr limit)
{
  MFS mfs = MustBeA(MFSPool, pool);
  Word i, unitsPerExtent;
  Size size;
  Size unitSize;
  Size ringSize;
  Header header = NULL;
  Ring mfsRing;

  AVER(base < limit);
  AVER(AddrOffset(base, limit) == mfs->extendBy);

  /* Ensure that the memory we're adding belongs to this pool.  This is
     automatic if it was allocated using ArenaAlloc, but if the memory is
     being inserted from elsewhere then it must have been set up correctly. */
  AVER(PoolHasAddr(pool, base));

  /* .ring-node.at-base: Store the extent ring node at the base of the
     extent. This transgresses the rule that pools should allocate
     control structures from another pool, because an MFS is required
     during bootstrap when no other pools are available. See
     <design/poolmfs/#impl.extent-ring.justify> */
  mfsRing = (Ring)base;
  RingInit(mfsRing);
  RingAppend(&mfs->extentRing, mfsRing);

  ringSize = SizeAlignUp(sizeof(RingStruct), MPS_PF_ALIGN);
  base = AddrAdd(base, ringSize);
  AVER(base < limit);
  size = AddrOffset(base, limit);

  /* Update accounting */
  mfs->total += size;
  mfs->free += size;

  /* Sew together all the new empty units in the region, working down */
  /* from the top so that they are in ascending order of address on the */
  /* free list. */

  unitSize = mfs->unitSize;
  unitsPerExtent = size/unitSize;
  AVER(unitsPerExtent > 0);

#define SUB(b, s, i)    ((Header)AddrAdd(b, (s)*(i)))

  for(i = 0; i < unitsPerExtent; ++i)
  {
    header = SUB(base, unitSize, unitsPerExtent-i - 1);
    AVER(AddrIsAligned(header, pool->alignment));
    AVER(AddrAdd((Addr)header, unitSize) <= AddrAdd(base, size));
    header->next = mfs->freeList;
    mfs->freeList = header;
  }

#undef SUB
}


/*  == Allocate ==
 *
 *  Allocation simply involves taking a unit from the front of the freelist
 *  and returning it.  If there are none, a new region is allocated from the
 *  arena.
 */

static Res MFSAlloc(Addr *pReturn, Pool pool, Size size)
{
  MFS mfs = MustBeA(MFSPool, pool);
  Header f;
  Res res;

  AVER(pReturn != NULL);
  AVER(size == mfs->unroundedUnitSize);

  f = mfs->freeList;

  /* If the free list is empty then extend the pool with a new region. */

  if(f == NULL)
  {
    Addr base;

    /* See design.mps.bootstrap.land.sol.pool. */
    if (!mfs->extendSelf)
      return ResLIMIT;

    /* Create a new extent and attach it to the pool. */
    res = ArenaAlloc(&base, LocusPrefDefault(), mfs->extendBy, pool);
    if(res != ResOK)
      return res;

    MFSExtend(pool, base, AddrAdd(base, mfs->extendBy));

    /* The first unit in the region is now the head of the new free list. */
    f = mfs->freeList;
  }

  AVER(f != NULL);

  /* Detach the first free unit from the free list and return its address. */

  mfs->freeList = f->next;
  AVER(mfs->free >= mfs->unitSize);
  mfs->free -= mfs->unitSize;

  *pReturn = (Addr)f;
  return ResOK;
}


/*  == Free ==
 *
 *  Freeing a unit simply involves pushing it onto the front of the
 *  freelist.
 */

static void MFSFree(Pool pool, Addr old, Size size)
{
  MFS mfs = MustBeA(MFSPool, pool);
  Header h;

  AVER(old != (Addr)0);
  AVER(size == mfs->unroundedUnitSize);

  /* .freelist.fragments */
  h = (Header)old;
  h->next = mfs->freeList;
  mfs->freeList = h;
  mfs->free += mfs->unitSize;
}


/* MFSTotalSize -- total memory allocated from the arena */

static Size MFSTotalSize(Pool pool)
{
  MFS mfs = MustBeA(MFSPool, pool);
  return mfs->total;
}


/* MFSFreeSize -- free memory (unused by client program) */

static Size MFSFreeSize(Pool pool)
{
  MFS mfs = MustBeA(MFSPool, pool);
  return mfs->free;
}


static Res MFSDescribe(Inst inst, mps_lib_FILE *stream, Count depth)
{
  Pool pool = CouldBeA(AbstractPool, inst);
  MFS mfs = CouldBeA(MFSPool, pool);
  Res res;

  if (!TESTC(MFSPool, mfs))
    return ResPARAM;
  if (stream == NULL)
    return ResPARAM;

  res = NextMethod(Inst, MFSPool, describe)(inst, stream, depth);
  if (res != ResOK)
    return res;

  return WriteF(stream, depth + 2,
                "unroundedUnitSize $W\n", (WriteFW)mfs->unroundedUnitSize,
                "extendBy $W\n", (WriteFW)mfs->extendBy,
                "extendSelf $S\n", WriteFYesNo(mfs->extendSelf),
                "unitSize $W\n", (WriteFW)mfs->unitSize,
                "freeList $P\n", (WriteFP)mfs->freeList,
                "total $W\n", (WriteFW)mfs->total,
                "free $W\n", (WriteFW)mfs->free,
                NULL);
}


DEFINE_CLASS(Pool, MFSPool, klass)
{
  INHERIT_CLASS(klass, MFSPool, AbstractPool);
  klass->instClassStruct.describe = MFSDescribe;
  klass->instClassStruct.finish = MFSFinish;
  klass->size = sizeof(MFSStruct);
  klass->varargs = MFSVarargs;
  klass->init = MFSInit;
  klass->alloc = MFSAlloc;
  klass->free = MFSFree;
  klass->totalSize = MFSTotalSize;
  klass->freeSize = MFSFreeSize;  
  AVERT(PoolClass, klass);
}


PoolClass PoolClassMFS(void)
{
  return CLASS(MFSPool);
}


mps_pool_class_t mps_class_mfs(void)
{
  return (mps_pool_class_t)PoolClassMFS();
}


Bool MFSCheck(MFS mfs)
{
  Arena arena;

  CHECKS(MFS, mfs);
  CHECKC(MFSPool, mfs);
  CHECKD(Pool, MFSPool(mfs));
  CHECKC(MFSPool, mfs);
  CHECKL(mfs->unitSize >= UNIT_MIN);
  CHECKL(mfs->extendBy >= UNIT_MIN);
  CHECKL(BoolCheck(mfs->extendSelf));
  arena = PoolArena(MFSPool(mfs));
  CHECKL(SizeIsArenaGrains(mfs->extendBy, arena));
  CHECKL(SizeAlignUp(mfs->unroundedUnitSize, PoolAlignment(MFSPool(mfs))) ==
         mfs->unitSize);
  CHECKD_NOSIG(Ring, &mfs->extentRing);
  CHECKL(mfs->free <= mfs->total);
  CHECKL((mfs->total - mfs->free) % mfs->unitSize == 0);
  return TRUE;
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2001-2018 Ravenbrook Limited <http://www.ravenbrook.com/>.
 * All rights reserved.  This is an open source license.  Contact
 * Ravenbrook for commercial licensing options.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. Redistributions in any form must be accompanied by information on how
 * to obtain complete source code for this software and any accompanying
 * software that uses this software.  The source code must either be
 * included in the distribution or be available for no more than the cost
 * of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions.  For an executable file, complete source
 * code means the source code for all modules it contains. It does not
 * include source code for modules or files that typically accompany the
 * major components of the operating system on which the executable file
 * runs.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
