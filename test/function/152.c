/* 
TEST_HEADER
 id = $Id$
 summary = SNC scanning test
 language = c
 link = testlib.o rankfmt.o
OUTPUT_SPEC
 inc1 = 1
 inc2 = 0
 result = pass
END_HEADER
*/

/* This test was derived from MMQA_test_function!148.c */

/* This test uses an AMC pool and an SNC pool, with objects in the SNC
   pool keeping objects in the ANC pool alive. No ambiguous roots, so we
   can tell exactly what's been scanned and check that the SNC pool is
   working properly.
*/

#include "testlib.h"
#include "mpscamc.h"
#include "mpscsnc.h"
#include "mpsavm.h"
#include "rankfmt.h"


#define ARENA_SIZE (100ul*1024ul*1024ul)
#define BIGSIZE (10ul*1024*1024)
#define SMALLSIZE (4096)

#define genCOUNT (3)

static mps_gen_param_s testChain[genCOUNT] = {
  { 6000, 0.90 }, { 8000, 0.65 }, { 16000, 0.50 } };


static void test(void)
{
 mps_ap_t ap, sap;
 mps_arena_t arena;
 mps_fmt_t format;
 mps_chain_t chain;
 mps_pool_t pool, spool;
 mps_thr_t thread;
 mps_frame_t frame1;
 mycell *p, *q;
 size_t com, com1;

 formatcomments=1;
 alloccomments=1;
 fixcomments=1;

 /* create an arena (no particular size limit) */

 cdie(mps_arena_create(&arena, mps_arena_class_vm(), (size_t)ARENA_SIZE),
      "create arena");
 mps_arena_spare_set(arena, 0.0);

 cdie(mps_thread_reg(&thread, arena), "register thread");

 /* because we know objects in the stack pool don't move, 
    we can do without roots. Hooray! */

 cdie(mps_fmt_create_A(&format, arena, &fmtA), "create format");
 cdie(mps_chain_create(&chain, arena, genCOUNT, testChain), "chain_create");

 die(mmqa_pool_create_chain(&pool, arena, mps_class_amc(), format, chain),
     "create AMC pool");
 cdie(mps_ap_create(&ap, pool, mps_rank_exact()), "create ap");
 cdie(mps_pool_create(&spool, arena, mps_class_snc(), format),
      "create SNC pool");
 cdie(mps_ap_create(&sap, spool, mps_rank_exact()), "create ap");

 /* push, alloc, check object is scanned */

 com = mps_arena_committed(arena);
 report("com", "%ld", com);
 cdie(mps_ap_frame_push(&frame1, sap), "push");
 p = allocone(sap, 2, mps_rank_exact());
 q = allocdumb(ap, BIGSIZE, mps_rank_exact());
 setref(p, 0, q);
 q = allocdumb(ap, SMALLSIZE, mps_rank_exact());
 comment("collect...");
 mps_arena_collect(arena);
 com1 = mps_arena_committed(arena);
 mps_arena_release(arena);
 report("com1", "%lu", (unsigned long)com1);
 report("inc1", "%ld", ((long)com1-(long)com)/(long)BIGSIZE);

 /* pop, check object isn't scanned */

 cdie(mps_ap_frame_pop(sap, frame1), "pop");
 p = allocone(sap, 2, mps_rank_exact());
 comment("collect...");
 mps_arena_collect(arena);
 com1 = mps_arena_committed(arena);
 mps_arena_release(arena);
 report("com2", "%lu", (unsigned long)com1);
 report("inc2", "%ld", ((long)com1-(long)com)/(long)BIGSIZE);

 mps_arena_park(arena);
 mps_ap_destroy(ap);
 mps_ap_destroy(sap);
 comment("Destroyed ap.");

 mps_pool_destroy(pool);
 mps_pool_destroy(spool);
 comment("Destroyed pool.");

 mps_chain_destroy(chain);
 mps_fmt_destroy(format);
 mps_thread_dereg(thread);
 mps_arena_destroy(arena);
 comment("Destroyed arena.");
}


int main(void)
{
 easy_tramp(test);
 pass();
 return 0;
}
