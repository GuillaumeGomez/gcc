/* Basic block reordering routines for the GNU compiler.
   Copyright (C) 2000, 2002, 2003, 2004 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING.  If not, write to the Free
   Software Foundation, 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA.  */

/* This (greedy) algorithm constructs traces in several rounds.
   The construction starts from "seeds".  The seed for the first round
   is the entry point of function.  When there are more than one seed
   that one is selected first that has the lowest key in the heap
   (see function bb_to_key).  Then the algorithm repeatedly adds the most
   probable successor to the end of a trace.  Finally it connects the traces.

   There are two parameters: Branch Threshold and Exec Threshold.
   If the edge to a successor of the actual basic block is lower than
   Branch Threshold or the frequency of the successor is lower than
   Exec Threshold the successor will be the seed in one of the next rounds.
   Each round has these parameters lower than the previous one.
   The last round has to have these parameters set to zero
   so that the remaining blocks are picked up.

   The algorithm selects the most probable successor from all unvisited
   successors and successors that have been added to this trace.
   The other successors (that has not been "sent" to the next round) will be
   other seeds for this round and the secondary traces will start in them.
   If the successor has not been visited in this trace it is added to the trace
   (however, there is some heuristic for simple branches).
   If the successor has been visited in this trace the loop has been found.
   If the loop has many iterations the loop is rotated so that the
   source block of the most probable edge going out from the loop
   is the last block of the trace.
   If the loop has few iterations and there is no edge from the last block of
   the loop going out from loop the loop header is duplicated.
   Finally, the construction of the trace is terminated.

   When connecting traces it first checks whether there is an edge from the
   last block of one trace to the first block of another trace.
   When there are still some unconnected traces it checks whether there exists
   a basic block BB such that BB is a successor of the last bb of one trace
   and BB is a predecessor of the first block of another trace. In this case,
   BB is duplicated and the traces are connected through this duplicate.
   The rest of traces are simply connected so there will be a jump to the
   beginning of the rest of trace.


   References:

   "Software Trace Cache"
   A. Ramirez, J. Larriba-Pey, C. Navarro, J. Torrellas and M. Valero; 1999
   http://citeseer.nj.nec.com/15361.html

*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "basic-block.h"
#include "flags.h"
#include "timevar.h"
#include "output.h"
#include "cfglayout.h"
#include "fibheap.h"
#include "target.h"
#include "function.h"
#include "tm_p.h"
#include "obstack.h"
#include "expr.h"
#include "regs.h"

/* The number of rounds.  In most cases there will only be 4 rounds, but
   when partitioning hot and cold basic blocks into separate sections of
   the .o file there will be an extra round.*/
#define N_ROUNDS 5

/* Stubs in case we don't have a return insn.
   We have to check at runtime too, not only compiletime.  */  

#ifndef HAVE_return
#define HAVE_return 0
#define gen_return() NULL_RTX
#endif


/* Branch thresholds in thousandths (per mille) of the REG_BR_PROB_BASE.  */
static int branch_threshold[N_ROUNDS] = {400, 200, 100, 0, 0};

/* Exec thresholds in thousandths (per mille) of the frequency of bb 0.  */
static int exec_threshold[N_ROUNDS] = {500, 200, 50, 0, 0};

/* If edge frequency is lower than DUPLICATION_THRESHOLD per mille of entry
   block the edge destination is not duplicated while connecting traces.  */
#define DUPLICATION_THRESHOLD 100

/* Length of unconditional jump instruction.  */
static int uncond_jump_length;

/* Structure to hold needed information for each basic block.  */
typedef struct bbro_basic_block_data_def
{
  /* Which trace is the bb start of (-1 means it is not a start of a trace).  */
  int start_of_trace;

  /* Which trace is the bb end of (-1 means it is not an end of a trace).  */
  int end_of_trace;

  /* Which heap is BB in (if any)?  */
  fibheap_t heap;

  /* Which heap node is BB in (if any)?  */
  fibnode_t node;
} bbro_basic_block_data;

/* The current size of the following dynamic array.  */
static int array_size;

/* The array which holds needed information for basic blocks.  */
static bbro_basic_block_data *bbd;

/* To avoid frequent reallocation the size of arrays is greater than needed,
   the number of elements is (not less than) 1.25 * size_wanted.  */
#define GET_ARRAY_SIZE(X) ((((X) / 4) + 1) * 5)

/* Free the memory and set the pointer to NULL.  */
#define FREE(P) \
  do { if (P) { free (P); P = 0; } else { abort (); } } while (0)

/* Structure for holding information about a trace.  */
struct trace
{
  /* First and last basic block of the trace.  */
  basic_block first, last;

  /* The round of the STC creation which this trace was found in.  */
  int round;

  /* The length (i.e. the number of basic blocks) of the trace.  */
  int length;
};

/* Maximum frequency and count of one of the entry blocks.  */
int max_entry_frequency;
gcov_type max_entry_count;

/* Local function prototypes.  */
static void find_traces (int *, struct trace *);
static basic_block rotate_loop (edge, struct trace *, int);
static void mark_bb_visited (basic_block, int);
static void find_traces_1_round (int, int, gcov_type, struct trace *, int *,
				 int, fibheap_t *, int);
static basic_block copy_bb (basic_block, edge, basic_block, int);
static fibheapkey_t bb_to_key (basic_block);
static bool better_edge_p (basic_block, edge, int, int, int, int, edge);
static void connect_traces (int, struct trace *);
static bool copy_bb_p (basic_block, int);
static int get_uncond_jump_length (void);
static bool push_to_next_round_p (basic_block, int, int, int, gcov_type);
static void add_unlikely_executed_notes (void);
static void find_rarely_executed_basic_blocks_and_crossing_edges (edge *, 
								  int *,
								  int *);
static void mark_bb_for_unlikely_executed_section  (basic_block);
static void add_labels_and_missing_jumps (edge *, int);
static void add_reg_crossing_jump_notes (void);
static void fix_up_fall_thru_edges (void);
static void fix_edges_for_rarely_executed_code (edge *, int);
static void fix_crossing_conditional_branches (void);
static void fix_crossing_unconditional_branches (void);

/* Check to see if bb should be pushed into the next round of trace
   collections or not.  Reasons for pushing the block forward are 1).
   If the block is cold, we are doing partitioning, and there will be
   another round (cold partition blocks are not supposed to be
   collected into traces until the very last round); or 2). There will
   be another round, and the basic block is not "hot enough" for the
   current round of trace collection.  */

static bool
push_to_next_round_p (basic_block bb, int round, int number_of_rounds,
		      int exec_th, gcov_type count_th)
{
  bool there_exists_another_round;
  bool cold_block;
  bool block_not_hot_enough;

  there_exists_another_round = round < number_of_rounds - 1;

  cold_block = (flag_reorder_blocks_and_partition 
		&& bb->partition == COLD_PARTITION);

  block_not_hot_enough = (bb->frequency < exec_th 
			  || bb->count < count_th
			  || probably_never_executed_bb_p (bb));

  if (there_exists_another_round
      && (cold_block || block_not_hot_enough))
    return true;
  else 
    return false;
}

/* Find the traces for Software Trace Cache.  Chain each trace through
   RBI()->next.  Store the number of traces to N_TRACES and description of
   traces to TRACES.  */

static void
find_traces (int *n_traces, struct trace *traces)
{
  int i;
  int number_of_rounds;
  edge e;
  fibheap_t heap;

  /* Add one extra round of trace collection when partitioning hot/cold
     basic blocks into separate sections.  The last round is for all the
     cold blocks (and ONLY the cold blocks).  */

  number_of_rounds = N_ROUNDS - 1;
  if (flag_reorder_blocks_and_partition)
    number_of_rounds = N_ROUNDS;

  /* Insert entry points of function into heap.  */
  heap = fibheap_new ();
  max_entry_frequency = 0;
  max_entry_count = 0;
  for (e = ENTRY_BLOCK_PTR->succ; e; e = e->succ_next)
    {
      bbd[e->dest->index].heap = heap;
      bbd[e->dest->index].node = fibheap_insert (heap, bb_to_key (e->dest),
						    e->dest);
      if (e->dest->frequency > max_entry_frequency)
	max_entry_frequency = e->dest->frequency;
      if (e->dest->count > max_entry_count)
	max_entry_count = e->dest->count;
    }

  /* Find the traces.  */
  for (i = 0; i < number_of_rounds; i++)
    {
      gcov_type count_threshold;

      if (dump_file)
	fprintf (dump_file, "STC - round %d\n", i + 1);

      if (max_entry_count < INT_MAX / 1000)
	count_threshold = max_entry_count * exec_threshold[i] / 1000;
      else
	count_threshold = max_entry_count / 1000 * exec_threshold[i];

      find_traces_1_round (REG_BR_PROB_BASE * branch_threshold[i] / 1000,
			   max_entry_frequency * exec_threshold[i] / 1000,
			   count_threshold, traces, n_traces, i, &heap,
			   number_of_rounds);
    }
  fibheap_delete (heap);

  if (dump_file)
    {
      for (i = 0; i < *n_traces; i++)
	{
	  basic_block bb;
	  fprintf (dump_file, "Trace %d (round %d):  ", i + 1,
		   traces[i].round + 1);
	  for (bb = traces[i].first; bb != traces[i].last; bb = bb->rbi->next)
	    fprintf (dump_file, "%d [%d] ", bb->index, bb->frequency);
	  fprintf (dump_file, "%d [%d]\n", bb->index, bb->frequency);
	}
      fflush (dump_file);
    }
}

/* Rotate loop whose back edge is BACK_EDGE in the tail of trace TRACE
   (with sequential number TRACE_N).  */

static basic_block
rotate_loop (edge back_edge, struct trace *trace, int trace_n)
{
  basic_block bb;

  /* Information about the best end (end after rotation) of the loop.  */
  basic_block best_bb = NULL;
  edge best_edge = NULL;
  int best_freq = -1;
  gcov_type best_count = -1;
  /* The best edge is preferred when its destination is not visited yet
     or is a start block of some trace.  */
  bool is_preferred = false;

  /* Find the most frequent edge that goes out from current trace.  */
  bb = back_edge->dest;
  do
    {
      edge e;
      for (e = bb->succ; e; e = e->succ_next)
	if (e->dest != EXIT_BLOCK_PTR
	    && e->dest->rbi->visited != trace_n
	    && (e->flags & EDGE_CAN_FALLTHRU)
	    && !(e->flags & EDGE_COMPLEX))
	{
	  if (is_preferred)
	    {
	      /* The best edge is preferred.  */
	      if (!e->dest->rbi->visited
		  || bbd[e->dest->index].start_of_trace >= 0)
		{
		  /* The current edge E is also preferred.  */
		  int freq = EDGE_FREQUENCY (e);
		  if (freq > best_freq || e->count > best_count)
		    {
		      best_freq = freq;
		      best_count = e->count;
		      best_edge = e;
		      best_bb = bb;
		    }
		}
	    }
	  else
	    {
	      if (!e->dest->rbi->visited
		  || bbd[e->dest->index].start_of_trace >= 0)
		{
		  /* The current edge E is preferred.  */
		  is_preferred = true;
		  best_freq = EDGE_FREQUENCY (e);
		  best_count = e->count;
		  best_edge = e;
		  best_bb = bb;
		}
	      else
		{
		  int freq = EDGE_FREQUENCY (e);
		  if (!best_edge || freq > best_freq || e->count > best_count)
		    {
		      best_freq = freq;
		      best_count = e->count;
		      best_edge = e;
		      best_bb = bb;
		    }
		}
	    }
	}
      bb = bb->rbi->next;
    }
  while (bb != back_edge->dest);

  if (best_bb)
    {
      /* Rotate the loop so that the BEST_EDGE goes out from the last block of
	 the trace.  */
      if (back_edge->dest == trace->first)
	{
	  trace->first = best_bb->rbi->next;
	}
      else
	{
	  basic_block prev_bb;

	  for (prev_bb = trace->first;
	       prev_bb->rbi->next != back_edge->dest;
	       prev_bb = prev_bb->rbi->next)
	    ;
	  prev_bb->rbi->next = best_bb->rbi->next;

	  /* Try to get rid of uncond jump to cond jump.  */
	  if (prev_bb->succ && !prev_bb->succ->succ_next)
	    {
	      basic_block header = prev_bb->succ->dest;

	      /* Duplicate HEADER if it is a small block containing cond jump
		 in the end.  */
	      if (any_condjump_p (BB_END (header)) && copy_bb_p (header, 0))
		{
		  copy_bb (header, prev_bb->succ, prev_bb, trace_n);
		}
	    }
	}
    }
  else
    {
      /* We have not found suitable loop tail so do no rotation.  */
      best_bb = back_edge->src;
    }
  best_bb->rbi->next = NULL;
  return best_bb;
}

/* This function marks BB that it was visited in trace number TRACE.  */

static void
mark_bb_visited (basic_block bb, int trace)
{
  bb->rbi->visited = trace;
  if (bbd[bb->index].heap)
    {
      fibheap_delete_node (bbd[bb->index].heap, bbd[bb->index].node);
      bbd[bb->index].heap = NULL;
      bbd[bb->index].node = NULL;
    }
}

/* One round of finding traces. Find traces for BRANCH_TH and EXEC_TH i.e. do
   not include basic blocks their probability is lower than BRANCH_TH or their
   frequency is lower than EXEC_TH into traces (or count is lower than
   COUNT_TH).  It stores the new traces into TRACES and modifies the number of
   traces *N_TRACES. Sets the round (which the trace belongs to) to ROUND. It
   expects that starting basic blocks are in *HEAP and at the end it deletes
   *HEAP and stores starting points for the next round into new *HEAP.  */

static void
find_traces_1_round (int branch_th, int exec_th, gcov_type count_th,
		     struct trace *traces, int *n_traces, int round,
		     fibheap_t *heap, int number_of_rounds)
{
  /* The following variable refers to the last round in which non-"cold" 
     blocks may be collected into a trace.  */

  int last_round = N_ROUNDS - 1;

  /* Heap for discarded basic blocks which are possible starting points for
     the next round.  */
  fibheap_t new_heap = fibheap_new ();

  while (!fibheap_empty (*heap))
    {
      basic_block bb;
      struct trace *trace;
      edge best_edge, e;
      fibheapkey_t key;

      bb = fibheap_extract_min (*heap);
      bbd[bb->index].heap = NULL;
      bbd[bb->index].node = NULL;

      if (dump_file)
	fprintf (dump_file, "Getting bb %d\n", bb->index);

      /* If the BB's frequency is too low send BB to the next round.  When
         partitioning hot/cold blocks into separate sections, make sure all
         the cold blocks (and ONLY the cold blocks) go into the (extra) final
         round.  */

      if (push_to_next_round_p (bb, round, number_of_rounds, exec_th, 
				count_th))
	{
	  int key = bb_to_key (bb);
	  bbd[bb->index].heap = new_heap;
	  bbd[bb->index].node = fibheap_insert (new_heap, key, bb);

	  if (dump_file)
	    fprintf (dump_file,
		     "  Possible start point of next round: %d (key: %d)\n",
		     bb->index, key);
	  continue;
	}

      trace = traces + *n_traces;
      trace->first = bb;
      trace->round = round;
      trace->length = 0;
      (*n_traces)++;

      do
	{
	  int prob, freq;

	  /* The probability and frequency of the best edge.  */
	  int best_prob = INT_MIN / 2;
	  int best_freq = INT_MIN / 2;

	  best_edge = NULL;
	  mark_bb_visited (bb, *n_traces);
	  trace->length++;

	  if (dump_file)
	    fprintf (dump_file, "Basic block %d was visited in trace %d\n",
		     bb->index, *n_traces - 1);

	  /* Select the successor that will be placed after BB.  */
	  for (e = bb->succ; e; e = e->succ_next)
	    {
#ifdef ENABLE_CHECKING
	      if (e->flags & EDGE_FAKE)
		abort ();
#endif

	      if (e->dest == EXIT_BLOCK_PTR)
		continue;

	      if (e->dest->rbi->visited
		  && e->dest->rbi->visited != *n_traces)
		continue;

	      if (e->dest->partition == COLD_PARTITION
		  && round < last_round)
		continue;

	      prob = e->probability;
	      freq = EDGE_FREQUENCY (e);

	      /* Edge that cannot be fallthru or improbable or infrequent
		 successor (ie. it is unsuitable successor).  */
	      if (!(e->flags & EDGE_CAN_FALLTHRU) || (e->flags & EDGE_COMPLEX)
		  || prob < branch_th || freq < exec_th || e->count < count_th)
		continue;

	      /* If partitioning hot/cold basic blocks, don't consider edges
		 that cross section boundaries.  */

	      if (better_edge_p (bb, e, prob, freq, best_prob, best_freq,
				 best_edge))
		{
		  best_edge = e;
		  best_prob = prob;
		  best_freq = freq;
		}
	    }

	  /* If the best destination has multiple predecessors, and can be
	     duplicated cheaper than a jump, don't allow it to be added
	     to a trace.  We'll duplicate it when connecting traces.  */
	  if (best_edge && best_edge->dest->pred->pred_next
	      && copy_bb_p (best_edge->dest, 0))
	    best_edge = NULL;

	  /* Add all non-selected successors to the heaps.  */
	  for (e = bb->succ; e; e = e->succ_next)
	    {
	      if (e == best_edge
		  || e->dest == EXIT_BLOCK_PTR
		  || e->dest->rbi->visited)
		continue;

	      key = bb_to_key (e->dest);

	      if (bbd[e->dest->index].heap)
		{
		  /* E->DEST is already in some heap.  */
		  if (key != bbd[e->dest->index].node->key)
		    {
		      if (dump_file)
			{
			  fprintf (dump_file,
				   "Changing key for bb %d from %ld to %ld.\n",
				   e->dest->index,
				   (long) bbd[e->dest->index].node->key,
				   key);
			}
		      fibheap_replace_key (bbd[e->dest->index].heap,
					   bbd[e->dest->index].node, key);
		    }
		}
	      else
		{
		  fibheap_t which_heap = *heap;

		  prob = e->probability;
		  freq = EDGE_FREQUENCY (e);

		  if (!(e->flags & EDGE_CAN_FALLTHRU)
		      || (e->flags & EDGE_COMPLEX)
		      || prob < branch_th || freq < exec_th
		      || e->count < count_th)
		    {
		      /* When partitioning hot/cold basic blocks, make sure
			 the cold blocks (and only the cold blocks) all get
			 pushed to the last round of trace collection.  */

		      if (push_to_next_round_p (e->dest, round, 
						number_of_rounds,
						exec_th, count_th))
			which_heap = new_heap;
		    }

		  bbd[e->dest->index].heap = which_heap;
		  bbd[e->dest->index].node = fibheap_insert (which_heap,
								key, e->dest);

		  if (dump_file)
		    {
		      fprintf (dump_file,
			       "  Possible start of %s round: %d (key: %ld)\n",
			       (which_heap == new_heap) ? "next" : "this",
			       e->dest->index, (long) key);
		    }

		}
	    }

	  if (best_edge) /* Suitable successor was found.  */
	    {
	      if (best_edge->dest->rbi->visited == *n_traces)
		{
		  /* We do nothing with one basic block loops.  */
		  if (best_edge->dest != bb)
		    {
		      if (EDGE_FREQUENCY (best_edge)
			  > 4 * best_edge->dest->frequency / 5)
			{
			  /* The loop has at least 4 iterations.  If the loop
			     header is not the first block of the function
			     we can rotate the loop.  */

			  if (best_edge->dest != ENTRY_BLOCK_PTR->next_bb)
			    {
			      if (dump_file)
				{
				  fprintf (dump_file,
					   "Rotating loop %d - %d\n",
					   best_edge->dest->index, bb->index);
				}
			      bb->rbi->next = best_edge->dest;
			      bb = rotate_loop (best_edge, trace, *n_traces);
			    }
			}
		      else
			{
			  /* The loop has less than 4 iterations.  */

			  /* Check whether there is another edge from BB.  */
			  edge another_edge;
			  for (another_edge = bb->succ;
			       another_edge;
			       another_edge = another_edge->succ_next)
			    if (another_edge != best_edge)
			      break;

			  if (!another_edge && copy_bb_p (best_edge->dest,
							  !optimize_size))
			    {
			      bb = copy_bb (best_edge->dest, best_edge, bb,
					    *n_traces);
			    }
			}
		    }

		  /* Terminate the trace.  */
		  break;
		}
	      else
		{
		  /* Check for a situation

		    A
		   /|
		  B |
		   \|
		    C

		  where
		  EDGE_FREQUENCY (AB) + EDGE_FREQUENCY (BC)
		    >= EDGE_FREQUENCY (AC).
		  (i.e. 2 * B->frequency >= EDGE_FREQUENCY (AC) )
		  Best ordering is then A B C.

		  This situation is created for example by:

		  if (A) B;
		  C;

		  */

		  for (e = bb->succ; e; e = e->succ_next)
		    if (e != best_edge
			&& (e->flags & EDGE_CAN_FALLTHRU)
			&& !(e->flags & EDGE_COMPLEX)
			&& !e->dest->rbi->visited
			&& !e->dest->pred->pred_next
			&& !e->crossing_edge
			&& e->dest->succ
			&& (e->dest->succ->flags & EDGE_CAN_FALLTHRU)
			&& !(e->dest->succ->flags & EDGE_COMPLEX)
			&& !e->dest->succ->succ_next
			&& e->dest->succ->dest == best_edge->dest
			&& 2 * e->dest->frequency >= EDGE_FREQUENCY (best_edge))
		      {
			best_edge = e;
			if (dump_file)
			  fprintf (dump_file, "Selecting BB %d\n",
				   best_edge->dest->index);
			break;
		      }

		  bb->rbi->next = best_edge->dest;
		  bb = best_edge->dest;
		}
	    }
	}
      while (best_edge);
      trace->last = bb;
      bbd[trace->first->index].start_of_trace = *n_traces - 1;
      bbd[trace->last->index].end_of_trace = *n_traces - 1;

      /* The trace is terminated so we have to recount the keys in heap
	 (some block can have a lower key because now one of its predecessors
	 is an end of the trace).  */
      for (e = bb->succ; e; e = e->succ_next)
	{
	  if (e->dest == EXIT_BLOCK_PTR
	      || e->dest->rbi->visited)
	    continue;

	  if (bbd[e->dest->index].heap)
	    {
	      key = bb_to_key (e->dest);
	      if (key != bbd[e->dest->index].node->key)
		{
		  if (dump_file)
		    {
		      fprintf (dump_file,
			       "Changing key for bb %d from %ld to %ld.\n",
			       e->dest->index,
			       (long) bbd[e->dest->index].node->key, key);
		    }
		  fibheap_replace_key (bbd[e->dest->index].heap,
				       bbd[e->dest->index].node,
				       key);
		}
	    }
	}
    }

  fibheap_delete (*heap);

  /* "Return" the new heap.  */
  *heap = new_heap;
}

/* Create a duplicate of the basic block OLD_BB and redirect edge E to it, add
   it to trace after BB, mark OLD_BB visited and update pass' data structures
   (TRACE is a number of trace which OLD_BB is duplicated to).  */

static basic_block
copy_bb (basic_block old_bb, edge e, basic_block bb, int trace)
{
  basic_block new_bb;

  new_bb = duplicate_block (old_bb, e);
  if (e->dest != new_bb)
    abort ();
  if (e->dest->rbi->visited)
    abort ();
  if (dump_file)
    fprintf (dump_file,
	     "Duplicated bb %d (created bb %d)\n",
	     old_bb->index, new_bb->index);
  new_bb->rbi->visited = trace;
  new_bb->rbi->next = bb->rbi->next;
  bb->rbi->next = new_bb;

  if (new_bb->index >= array_size || last_basic_block > array_size)
    {
      int i;
      int new_size;

      new_size = MAX (last_basic_block, new_bb->index + 1);
      new_size = GET_ARRAY_SIZE (new_size);
      bbd = xrealloc (bbd, new_size * sizeof (bbro_basic_block_data));
      for (i = array_size; i < new_size; i++)
	{
	  bbd[i].start_of_trace = -1;
	  bbd[i].end_of_trace = -1;
	  bbd[i].heap = NULL;
	  bbd[i].node = NULL;
	}
      array_size = new_size;

      if (dump_file)
	{
	  fprintf (dump_file,
		   "Growing the dynamic array to %d elements.\n",
		   array_size);
	}
    }

  return new_bb;
}

/* Compute and return the key (for the heap) of the basic block BB.  */

static fibheapkey_t
bb_to_key (basic_block bb)
{
  edge e;

  int priority = 0;

  /* Do not start in probably never executed blocks.  */

  if (bb->partition == COLD_PARTITION || probably_never_executed_bb_p (bb))
    return BB_FREQ_MAX;

  /* Prefer blocks whose predecessor is an end of some trace
     or whose predecessor edge is EDGE_DFS_BACK.  */
  for (e = bb->pred; e; e = e->pred_next)
    {
      if ((e->src != ENTRY_BLOCK_PTR && bbd[e->src->index].end_of_trace >= 0)
	  || (e->flags & EDGE_DFS_BACK))
	{
	  int edge_freq = EDGE_FREQUENCY (e);

	  if (edge_freq > priority)
	    priority = edge_freq;
	}
    }

  if (priority)
    /* The block with priority should have significantly lower key.  */
    return -(100 * BB_FREQ_MAX + 100 * priority + bb->frequency);
  return -bb->frequency;
}

/* Return true when the edge E from basic block BB is better than the temporary
   best edge (details are in function).  The probability of edge E is PROB. The
   frequency of the successor is FREQ.  The current best probability is
   BEST_PROB, the best frequency is BEST_FREQ.
   The edge is considered to be equivalent when PROB does not differ much from
   BEST_PROB; similarly for frequency.  */

static bool
better_edge_p (basic_block bb, edge e, int prob, int freq, int best_prob,
	       int best_freq, edge cur_best_edge)
{
  bool is_better_edge;

  /* The BEST_* values do not have to be best, but can be a bit smaller than
     maximum values.  */
  int diff_prob = best_prob / 10;
  int diff_freq = best_freq / 10;

  if (prob > best_prob + diff_prob)
    /* The edge has higher probability than the temporary best edge.  */
    is_better_edge = true;
  else if (prob < best_prob - diff_prob)
    /* The edge has lower probability than the temporary best edge.  */
    is_better_edge = false;
  else if (freq < best_freq - diff_freq)
    /* The edge and the temporary best edge  have almost equivalent
       probabilities.  The higher frequency of a successor now means
       that there is another edge going into that successor.
       This successor has lower frequency so it is better.  */
    is_better_edge = true;
  else if (freq > best_freq + diff_freq)
    /* This successor has higher frequency so it is worse.  */
    is_better_edge = false;
  else if (e->dest->prev_bb == bb)
    /* The edges have equivalent probabilities and the successors
       have equivalent frequencies.  Select the previous successor.  */
    is_better_edge = true;
  else
    is_better_edge = false;

  /* If we are doing hot/cold partitioning, make sure that we always favor
     non-crossing edges over crossing edges.  */

  if (!is_better_edge
      && flag_reorder_blocks_and_partition 
      && cur_best_edge 
      && cur_best_edge->crossing_edge
      && !e->crossing_edge)
    is_better_edge = true;

  return is_better_edge;
}

/* Connect traces in array TRACES, N_TRACES is the count of traces.  */

static void
connect_traces (int n_traces, struct trace *traces)
{
  int i;
  int unconnected_hot_trace_count = 0;
  bool cold_connected = true;
  bool *connected;
  bool *cold_traces;
  int last_trace;
  int freq_threshold;
  gcov_type count_threshold;

  freq_threshold = max_entry_frequency * DUPLICATION_THRESHOLD / 1000;
  if (max_entry_count < INT_MAX / 1000)
    count_threshold = max_entry_count * DUPLICATION_THRESHOLD / 1000;
  else
    count_threshold = max_entry_count / 1000 * DUPLICATION_THRESHOLD;

  connected = xcalloc (n_traces, sizeof (bool));
  last_trace = -1;

  /* If we are partitioning hot/cold basic blocks, mark the cold
     traces as already connected, to remove them from consideration
     for connection to the hot traces.  After the hot traces have all
     been connected (determined by "unconnected_hot_trace_count"), we
     will go back and connect the cold traces.  */

  cold_traces = xcalloc (n_traces, sizeof (bool));

  if (flag_reorder_blocks_and_partition)
    for (i = 0; i < n_traces; i++)
      {
	if (traces[i].first->partition == COLD_PARTITION)
	  {
	    connected[i] = true;
	    cold_traces[i] = true;
	    cold_connected = false;
	  }
	else
	  unconnected_hot_trace_count++;
      }
  
  for (i = 0; i < n_traces || !cold_connected ; i++)
    {
      int t = i;
      int t2;
      edge e, best;
      int best_len;

      /* If we are partitioning hot/cold basic blocks, check to see
	 if all the hot traces have been connected.  If so, go back
	 and mark the cold traces as unconnected so we can connect
	 them up too.  Re-set "i" to the first (unconnected) cold
	 trace. Use flag "cold_connected" to make sure we don't do
         this step more than once.  */

      if (flag_reorder_blocks_and_partition
	  && (i >= n_traces || unconnected_hot_trace_count <= 0)
	  && !cold_connected)
	{
	  int j;
	  int first_cold_trace = -1;

	  for (j = 0; j < n_traces; j++)
	    if (cold_traces[j])
	      {
		connected[j] = false;
		if (first_cold_trace == -1)
		  first_cold_trace = j;
	      }
	  i = t = first_cold_trace;
	  cold_connected = true;
	}

      if (connected[t])
	continue;

      connected[t] = true;
      if (unconnected_hot_trace_count > 0)
	unconnected_hot_trace_count--;

      /* Find the predecessor traces.  */
      for (t2 = t; t2 > 0;)
	{
	  best = NULL;
	  best_len = 0;
	  for (e = traces[t2].first->pred; e; e = e->pred_next)
	    {
	      int si = e->src->index;

	      if (e->src != ENTRY_BLOCK_PTR
		  && (e->flags & EDGE_CAN_FALLTHRU)
		  && !(e->flags & EDGE_COMPLEX)
		  && bbd[si].end_of_trace >= 0
		  && !connected[bbd[si].end_of_trace]
		  && (!best
		      || e->probability > best->probability
		      || (e->probability == best->probability
			  && traces[bbd[si].end_of_trace].length > best_len)))
		{
		  best = e;
		  best_len = traces[bbd[si].end_of_trace].length;
		}
	    }
	  if (best)
	    {
	      best->src->rbi->next = best->dest;
	      t2 = bbd[best->src->index].end_of_trace;
	      connected[t2] = true;

	      if (unconnected_hot_trace_count > 0)
		unconnected_hot_trace_count--;

	      if (dump_file)
		{
		  fprintf (dump_file, "Connection: %d %d\n",
			   best->src->index, best->dest->index);
		}
	    }
	  else
	    break;
	}

      if (last_trace >= 0)
	traces[last_trace].last->rbi->next = traces[t2].first;
      last_trace = t;

      /* Find the successor traces.  */
      while (1)
	{
	  /* Find the continuation of the chain.  */
	  best = NULL;
	  best_len = 0;
	  for (e = traces[t].last->succ; e; e = e->succ_next)
	    {
	      int di = e->dest->index;

	      if (e->dest != EXIT_BLOCK_PTR
		  && (e->flags & EDGE_CAN_FALLTHRU)
		  && !(e->flags & EDGE_COMPLEX)
		  && bbd[di].start_of_trace >= 0
		  && !connected[bbd[di].start_of_trace]
		  && (!best
		      || e->probability > best->probability
		      || (e->probability == best->probability
			  && traces[bbd[di].start_of_trace].length > best_len)))
		{
		  best = e;
		  best_len = traces[bbd[di].start_of_trace].length;
		}
	    }

	  if (best)
	    {
	      if (dump_file)
		{
		  fprintf (dump_file, "Connection: %d %d\n",
			   best->src->index, best->dest->index);
		}
	      t = bbd[best->dest->index].start_of_trace;
	      traces[last_trace].last->rbi->next = traces[t].first;
	      connected[t] = true;
	      if (unconnected_hot_trace_count > 0)
		unconnected_hot_trace_count--;
	      last_trace = t;
	    }
	  else
	    {
	      /* Try to connect the traces by duplication of 1 block.  */
	      edge e2;
	      basic_block next_bb = NULL;
	      bool try_copy = false;

	      for (e = traces[t].last->succ; e; e = e->succ_next)
		if (e->dest != EXIT_BLOCK_PTR
		    && (e->flags & EDGE_CAN_FALLTHRU)
		    && !(e->flags & EDGE_COMPLEX)
		    && (!best || e->probability > best->probability))
		  {
		    edge best2 = NULL;
		    int best2_len = 0;

		    /* If the destination is a start of a trace which is only
		       one block long, then no need to search the successor
		       blocks of the trace.  Accept it.  */
		    if (bbd[e->dest->index].start_of_trace >= 0
			&& traces[bbd[e->dest->index].start_of_trace].length
			   == 1)
		      {
			best = e;
			try_copy = true;
			continue;
		      }

		    for (e2 = e->dest->succ; e2; e2 = e2->succ_next)
		      {
			int di = e2->dest->index;

			if (e2->dest == EXIT_BLOCK_PTR
			    || ((e2->flags & EDGE_CAN_FALLTHRU)
				&& !(e2->flags & EDGE_COMPLEX)
				&& bbd[di].start_of_trace >= 0
				&& !connected[bbd[di].start_of_trace]
				&& (EDGE_FREQUENCY (e2) >= freq_threshold)
				&& (e2->count >= count_threshold)
				&& (!best2
				    || e2->probability > best2->probability
				    || (e2->probability == best2->probability
					&& traces[bbd[di].start_of_trace].length
					   > best2_len))))
			  {
			    best = e;
			    best2 = e2;
			    if (e2->dest != EXIT_BLOCK_PTR)
			      best2_len = traces[bbd[di].start_of_trace].length;
			    else
			      best2_len = INT_MAX;
			    next_bb = e2->dest;
			    try_copy = true;
			  }
		      }
		  }

	      if (flag_reorder_blocks_and_partition)
		try_copy = false;

	      /* Copy tiny blocks always; copy larger blocks only when the
		 edge is traversed frequently enough.  */
	      if (try_copy
		  && copy_bb_p (best->dest,
				!optimize_size
				&& EDGE_FREQUENCY (best) >= freq_threshold
				&& best->count >= count_threshold))
		{
		  basic_block new_bb;

		  if (dump_file)
		    {
		      fprintf (dump_file, "Connection: %d %d ",
			       traces[t].last->index, best->dest->index);
		      if (!next_bb)
			fputc ('\n', dump_file);
		      else if (next_bb == EXIT_BLOCK_PTR)
			fprintf (dump_file, "exit\n");
		      else
			fprintf (dump_file, "%d\n", next_bb->index);
		    }

		  new_bb = copy_bb (best->dest, best, traces[t].last, t);
		  traces[t].last = new_bb;
		  if (next_bb && next_bb != EXIT_BLOCK_PTR)
		    {
		      t = bbd[next_bb->index].start_of_trace;
		      traces[last_trace].last->rbi->next = traces[t].first;
		      connected[t] = true;
		      if (unconnected_hot_trace_count > 0)
			unconnected_hot_trace_count--;
		      last_trace = t;
		    }
		  else
		    break;	/* Stop finding the successor traces.  */
		}
	      else
		break;	/* Stop finding the successor traces.  */
	    }
	}
    }

  if (dump_file)
    {
      basic_block bb;

      fprintf (dump_file, "Final order:\n");
      for (bb = traces[0].first; bb; bb = bb->rbi->next)
	fprintf (dump_file, "%d ", bb->index);
      fprintf (dump_file, "\n");
      fflush (dump_file);
    }

  FREE (connected);
  FREE (cold_traces);
}

/* Return true when BB can and should be copied. CODE_MAY_GROW is true
   when code size is allowed to grow by duplication.  */

static bool
copy_bb_p (basic_block bb, int code_may_grow)
{
  int size = 0;
  int max_size = uncond_jump_length;
  rtx insn;
  int n_succ;
  edge e;

  if (!bb->frequency)
    return false;
  if (!bb->pred || !bb->pred->pred_next)
    return false;
  if (!can_duplicate_block_p (bb))
    return false;

  /* Avoid duplicating blocks which have many successors (PR/13430).  */
  n_succ = 0;
  for (e = bb->succ; e; e = e->succ_next)
    {
      n_succ++;
      if (n_succ > 8)
	return false;
    }

  if (code_may_grow && maybe_hot_bb_p (bb))
    max_size *= 8;

  for (insn = BB_HEAD (bb); insn != NEXT_INSN (BB_END (bb));
       insn = NEXT_INSN (insn))
    {
      if (INSN_P (insn))
	size += get_attr_length (insn);
    }

  if (size <= max_size)
    return true;

  if (dump_file)
    {
      fprintf (dump_file,
	       "Block %d can't be copied because its size = %d.\n",
	       bb->index, size);
    }

  return false;
}

/* Return the length of unconditional jump instruction.  */

static int
get_uncond_jump_length (void)
{
  rtx label, jump;
  int length;

  label = emit_label_before (gen_label_rtx (), get_insns ());
  jump = emit_jump_insn (gen_jump (label));

  length = get_attr_length (jump);

  delete_insn (jump);
  delete_insn (label);
  return length;
}

static void
add_unlikely_executed_notes (void)
{
  basic_block bb;

  FOR_EACH_BB (bb)
    if (bb->partition == COLD_PARTITION)
      mark_bb_for_unlikely_executed_section (bb);
}

/* Find the basic blocks that are rarely executed and need to be moved to
   a separate section of the .o file (to cut down on paging and improve
   cache locality).  */

static void
find_rarely_executed_basic_blocks_and_crossing_edges (edge *crossing_edges, 
						      int *n_crossing_edges, 
						      int *max_idx)
{
  basic_block bb;
  edge e;
  int i;

  /* Mark which partition (hot/cold) each basic block belongs in.  */
  
  FOR_EACH_BB (bb)
    {
      if (probably_never_executed_bb_p (bb))
	bb->partition = COLD_PARTITION;
      else
	bb->partition = HOT_PARTITION;
    }

  /* Mark every edge that crosses between sections.  */

  i = 0;
  FOR_EACH_BB (bb)
    for (e = bb->succ; e; e = e->succ_next)
      {
	if (e->src != ENTRY_BLOCK_PTR
	    && e->dest != EXIT_BLOCK_PTR
	    && e->src->partition != e->dest->partition)
	  {
	    e->crossing_edge = true;
	    if (i == *max_idx)
	      {
		*max_idx *= 2;
		crossing_edges = xrealloc (crossing_edges,
					   (*max_idx) * sizeof (edge));
	      }
	    crossing_edges[i++] = e;
	  }
	else
	  e->crossing_edge = false;
      }

  *n_crossing_edges = i;
}

/* Add NOTE_INSN_UNLIKELY_EXECUTED_CODE to top of basic block.   This note
   is later used to mark the basic block to be put in the 
   unlikely-to-be-executed section of the .o file.  */

static void
mark_bb_for_unlikely_executed_section (basic_block bb) 
{
  rtx cur_insn;
  rtx insert_insn = NULL;
  rtx new_note;
  
  /* Find first non-note instruction and insert new NOTE before it (as
     long as new NOTE is not first instruction in basic block).  */
  
  for (cur_insn = BB_HEAD (bb); cur_insn != NEXT_INSN (BB_END (bb)); 
       cur_insn = NEXT_INSN (cur_insn))
    if (!NOTE_P (cur_insn)
	&& !LABEL_P (cur_insn))
      {
	insert_insn = cur_insn;
	break;
      }
  
  /* Insert note and assign basic block number to it.  */
  
  if (insert_insn) 
    {
      new_note = emit_note_before (NOTE_INSN_UNLIKELY_EXECUTED_CODE, 
 				   insert_insn);
      NOTE_BASIC_BLOCK (new_note) = bb;
    }
  else
    {
      new_note = emit_note_after (NOTE_INSN_UNLIKELY_EXECUTED_CODE,
				  BB_END (bb));
      NOTE_BASIC_BLOCK (new_note) = bb;
    }
}

/* If any destination of a crossing edge does not have a label, add label;
   Convert any fall-through crossing edges (for blocks that do not contain
   a jump) to unconditional jumps.  */

static void 
add_labels_and_missing_jumps (edge *crossing_edges, int n_crossing_edges)
{
  int i;
  basic_block src;
  basic_block dest;
  rtx label;
  rtx barrier;
  rtx new_jump;
  
  for (i=0; i < n_crossing_edges; i++) 
    {
      if (crossing_edges[i]) 
  	{
  	  src = crossing_edges[i]->src; 
  	  dest = crossing_edges[i]->dest;
 	  
  	  /* Make sure dest has a label.  */
  	  
  	  if (dest && (dest != EXIT_BLOCK_PTR))
  	    {
	      label = block_label (dest);
	      
 	      /* Make sure source block ends with a jump.  */
	      
 	      if (src && (src != ENTRY_BLOCK_PTR)) 
 		{
		  if (!JUMP_P (BB_END (src)))
 		    /* bb just falls through.  */
 		    {
 		      /* make sure there's only one successor */
 		      if (src->succ && (src->succ->succ_next == NULL))
 			{
 			  /* Find label in dest block.  */
			  label = block_label (dest);

			  new_jump = emit_jump_insn_after (gen_jump (label), 
							   BB_END (src));
			  barrier = emit_barrier_after (new_jump);
			  JUMP_LABEL (new_jump) = label;
			  LABEL_NUSES (label) += 1;
			  src->rbi->footer = unlink_insn_chain (barrier,
								barrier);
			  /* Mark edge as non-fallthru.  */
			  crossing_edges[i]->flags &= ~EDGE_FALLTHRU;
			}
 		      else
 			{ 
 			  /* Basic block has two successors, but
 			     doesn't end in a jump; something is wrong
 			     here!  */
 			  abort();
 			}
 		    } /* end: 'if (GET_CODE ... '  */
 		} /* end: 'if (src && src->index...'  */
  	    } /* end: 'if (dest && dest->index...'  */
  	} /* end: 'if (crossing_edges[i]...'  */
    } /* end for loop  */
}

/* Find any bb's where the fall-through edge is a crossing edge (note that
   these bb's must also contain a conditional jump; we've already
   dealt with fall-through edges for blocks that didn't have a
   conditional jump in the call to add_labels_and_missing_jumps).
   Convert the fall-through edge to non-crossing edge by inserting a
   new bb to fall-through into.  The new bb will contain an
   unconditional jump (crossing edge) to the original fall through
   destination.  */

static void 
fix_up_fall_thru_edges (void)
{
  basic_block cur_bb;
  basic_block new_bb;
  edge succ1;
  edge succ2;
  edge fall_thru;
  edge cond_jump = NULL;
  edge e;
  bool cond_jump_crosses;
  int invert_worked;
  rtx old_jump;
  rtx fall_thru_label;
  rtx barrier;
  
  FOR_EACH_BB (cur_bb)
    {
      fall_thru = NULL;
      succ1 = cur_bb->succ;
      if (succ1)
  	succ2 = succ1->succ_next;
      else
  	succ2 = NULL;
      
      /* Find the fall-through edge.  */
      
      if (succ1 
 	  && (succ1->flags & EDGE_FALLTHRU))
 	{
 	  fall_thru = succ1;
 	  cond_jump = succ2;
 	}
      else if (succ2 
 	       && (succ2->flags & EDGE_FALLTHRU))
 	{
 	  fall_thru = succ2;
 	  cond_jump = succ1;
 	}
      
      if (fall_thru && (fall_thru->dest != EXIT_BLOCK_PTR))
  	{
  	  /* Check to see if the fall-thru edge is a crossing edge.  */
	
	  if (fall_thru->crossing_edge)
  	    {
	      /* The fall_thru edge crosses; now check the cond jump edge, if
	         it exists.  */
	      
 	      cond_jump_crosses = true;
 	      invert_worked  = 0;
	      old_jump = BB_END (cur_bb);
	      
 	      /* Find the jump instruction, if there is one.  */
	      
 	      if (cond_jump)
 		{
		  if (!cond_jump->crossing_edge)
 		    cond_jump_crosses = false;
		  
 		  /* We know the fall-thru edge crosses; if the cond
 		     jump edge does NOT cross, and its destination is the
		     next block in the bb order, invert the jump
 		     (i.e. fix it so the fall thru does not cross and
 		     the cond jump does).  */
 		  
		  if (!cond_jump_crosses
		      && cur_bb->rbi->next == cond_jump->dest)
 		    {
 		      /* Find label in fall_thru block. We've already added
 		         any missing labels, so there must be one.  */
 		      
 		      fall_thru_label = block_label (fall_thru->dest);

 		      if (old_jump && fall_thru_label)
 			invert_worked = invert_jump (old_jump, 
 						     fall_thru_label,0);
 		      if (invert_worked)
 			{
 			  fall_thru->flags &= ~EDGE_FALLTHRU;
 			  cond_jump->flags |= EDGE_FALLTHRU;
 			  update_br_prob_note (cur_bb);
 			  e = fall_thru;
 			  fall_thru = cond_jump;
 			  cond_jump = e;
			  cond_jump->crossing_edge = true;
			  fall_thru->crossing_edge = false;
 			}
 		    }
 		}
	      
 	      if (cond_jump_crosses || !invert_worked)
 		{
 		  /* This is the case where both edges out of the basic
 		     block are crossing edges. Here we will fix up the
		     fall through edge. The jump edge will be taken care
		     of later.  */
		  
 		  new_bb = force_nonfallthru (fall_thru);  
		  
 		  if (new_bb)
 		    {
 		      new_bb->rbi->next = cur_bb->rbi->next;
 		      cur_bb->rbi->next = new_bb;
		      
 		      /* Make sure new fall-through bb is in same 
			 partition as bb it's falling through from.  */
 		      
		      new_bb->partition = cur_bb->partition;
		      new_bb->succ->crossing_edge = true;
 		    }
		  
 		  /* Add barrier after new jump */
		  
 		  if (new_bb)
 		    {
 		      barrier = emit_barrier_after (BB_END (new_bb));
 		      new_bb->rbi->footer = unlink_insn_chain (barrier, 
 							       barrier);
 		    }
 		  else
 		    {
 		      barrier = emit_barrier_after (BB_END (cur_bb));
 		      cur_bb->rbi->footer = unlink_insn_chain (barrier,
 							       barrier);
 		    }
 		}
  	    }
  	}
    }
}

/* This function checks the destination blockof a "crossing jump" to
   see if it has any crossing predecessors that begin with a code label
   and end with an unconditional jump.  If so, it returns that predecessor
   block.  (This is to avoid creating lots of new basic blocks that all
   contain unconditional jumps to the same destination).  */

static basic_block
find_jump_block (basic_block jump_dest) 
{ 
  basic_block source_bb = NULL; 
  edge e;
  rtx insn;

  for (e = jump_dest->pred; e; e = e->pred_next)
    if (e->crossing_edge)
      {
	basic_block src = e->src;
	
	/* Check each predecessor to see if it has a label, and contains
	   only one executable instruction, which is an unconditional jump.
	   If so, we can use it.  */
	
	if (LABEL_P (BB_HEAD (src)))
	  for (insn = BB_HEAD (src); 
	       !INSN_P (insn) && insn != NEXT_INSN (BB_END (src));
	       insn = NEXT_INSN (insn))
	    {
	      if (INSN_P (insn)
		  && insn == BB_END (src)
		  && JUMP_P (insn)
		  && !any_condjump_p (insn))
		{
		  source_bb = src;
		  break;
		}
	    }
	
	if (source_bb)
	  break;
      }

  return source_bb;
}

/* Find all BB's with conditional jumps that are crossing edges;
   insert a new bb and make the conditional jump branch to the new
   bb instead (make the new bb same color so conditional branch won't
   be a 'crossing' edge).  Insert an unconditional jump from the
   new bb to the original destination of the conditional jump.  */

static void
fix_crossing_conditional_branches (void)
{
  basic_block cur_bb;
  basic_block new_bb;
  basic_block last_bb;
  basic_block dest;
  basic_block prev_bb;
  edge succ1;
  edge succ2;
  edge crossing_edge;
  edge new_edge;
  rtx old_jump;
  rtx set_src;
  rtx old_label = NULL_RTX;
  rtx new_label;
  rtx new_jump;
  rtx barrier;

 last_bb = EXIT_BLOCK_PTR->prev_bb;
  
  FOR_EACH_BB (cur_bb)
    {
      crossing_edge = NULL;
      succ1 = cur_bb->succ;
      if (succ1)
 	succ2 = succ1->succ_next;
      else
 	succ2 = NULL;
      
      /* We already took care of fall-through edges, so only one successor
	 can be a crossing edge.  */
      
      if (succ1 && succ1->crossing_edge)
	crossing_edge = succ1;
      else if (succ2 && succ2->crossing_edge)
 	crossing_edge = succ2;
      
      if (crossing_edge) 
 	{
	  old_jump = BB_END (cur_bb);
	  
	  /* Check to make sure the jump instruction is a
	     conditional jump.  */
	  
	  set_src = NULL_RTX;

	  if (any_condjump_p (old_jump))
	    {
	      if (GET_CODE (PATTERN (old_jump)) == SET)
		set_src = SET_SRC (PATTERN (old_jump));
	      else if (GET_CODE (PATTERN (old_jump)) == PARALLEL)
		{
		  set_src = XVECEXP (PATTERN (old_jump), 0,0);
		  if (GET_CODE (set_src) == SET)
		    set_src = SET_SRC (set_src);
		  else
		    set_src = NULL_RTX;
		}
	    }

	  if (set_src && (GET_CODE (set_src) == IF_THEN_ELSE))
	    {
	      if (GET_CODE (XEXP (set_src, 1)) == PC)
		old_label = XEXP (set_src, 2);
	      else if (GET_CODE (XEXP (set_src, 2)) == PC)
		old_label = XEXP (set_src, 1);
	      
	      /* Check to see if new bb for jumping to that dest has
		 already been created; if so, use it; if not, create
		 a new one.  */

	      new_bb = find_jump_block (crossing_edge->dest);
	      
	      if (new_bb)
		new_label = block_label (new_bb);
	      else
		{
		  /* Create new basic block to be dest for
		     conditional jump.  */
		  
		  new_bb = create_basic_block (NULL, NULL, last_bb);
		  new_bb->rbi->next = last_bb->rbi->next;
		  last_bb->rbi->next = new_bb;
		  prev_bb = last_bb;
		  last_bb = new_bb;
		  
		  /* Update register liveness information.  */
		  
		  new_bb->global_live_at_start = 
		    OBSTACK_ALLOC_REG_SET (&flow_obstack);
		  new_bb->global_live_at_end = 
		    OBSTACK_ALLOC_REG_SET (&flow_obstack);
		  COPY_REG_SET (new_bb->global_live_at_end,
				prev_bb->global_live_at_end);
		  COPY_REG_SET (new_bb->global_live_at_start,
				prev_bb->global_live_at_end);
		  
		  /* Put appropriate instructions in new bb.  */
		  
		  new_label = gen_label_rtx ();
		  emit_label_before (new_label, BB_HEAD (new_bb));
		  BB_HEAD (new_bb) = new_label;
		  
		  if (GET_CODE (old_label) == LABEL_REF)
		    {
		      old_label = JUMP_LABEL (old_jump);
		      new_jump = emit_jump_insn_after (gen_jump 
						       (old_label), 
						       BB_END (new_bb));
		    }
		  else if (HAVE_return
			   && GET_CODE (old_label) == RETURN)
		    new_jump = emit_jump_insn_after (gen_return (), 
						     BB_END (new_bb));
		  else
		    abort ();
		  
		  barrier = emit_barrier_after (new_jump);
		  JUMP_LABEL (new_jump) = old_label;
		  new_bb->rbi->footer = unlink_insn_chain (barrier, 
							   barrier);
		  
		  /* Make sure new bb is in same partition as source
		     of conditional branch.  */
		  
		  new_bb->partition = cur_bb->partition;
		}
	      
	      /* Make old jump branch to new bb.  */
	      
	      redirect_jump (old_jump, new_label, 0);
	      
	      /* Remove crossing_edge as predecessor of 'dest'.  */
	      
	      dest = crossing_edge->dest;
	      
	      redirect_edge_succ (crossing_edge, new_bb);
	      
	      /* Make a new edge from new_bb to old dest; new edge
		 will be a successor for new_bb and a predecessor
		 for 'dest'.  */
	      
	      if (!new_bb->succ)
		new_edge = make_edge (new_bb, dest, 0);
	      else
		new_edge = new_bb->succ;
	      
	      crossing_edge->crossing_edge = false;
	      new_edge->crossing_edge = true;
	    }
 	}
    }
}

/* Find any unconditional branches that cross between hot and cold
   sections.  Convert them into indirect jumps instead.  */

static void
fix_crossing_unconditional_branches (void)
{
  basic_block cur_bb;
  rtx last_insn;
  rtx label;
  rtx label_addr;
  rtx indirect_jump_sequence;
  rtx jump_insn = NULL_RTX;
  rtx new_reg;
  rtx cur_insn;
  edge succ;
  
  FOR_EACH_BB (cur_bb)
    {
      last_insn = BB_END (cur_bb);
      succ = cur_bb->succ;

      /* Check to see if bb ends in a crossing (unconditional) jump.  At
         this point, no crossing jumps should be conditional.  */

      if (JUMP_P (last_insn)
	  && succ->crossing_edge)
	{
	  rtx label2, table;

	  if (any_condjump_p (last_insn))
	    abort ();

	  /* Make sure the jump is not already an indirect or table jump.  */

	  else if (!computed_jump_p (last_insn)
		   && !tablejump_p (last_insn, &label2, &table))
	    {
	      /* We have found a "crossing" unconditional branch.  Now
		 we must convert it to an indirect jump.  First create
		 reference of label, as target for jump.  */
	      
	      label = JUMP_LABEL (last_insn);
	      label_addr = gen_rtx_LABEL_REF (Pmode, label);
	      LABEL_NUSES (label) += 1;
	      
	      /* Get a register to use for the indirect jump.  */
	      
	      new_reg = gen_reg_rtx (Pmode);
	      
	      /* Generate indirect the jump sequence.  */
	      
	      start_sequence ();
	      emit_move_insn (new_reg, label_addr);
	      emit_indirect_jump (new_reg);
	      indirect_jump_sequence = get_insns ();
	      end_sequence ();
	      
	      /* Make sure every instruction in the new jump sequence has
		 its basic block set to be cur_bb.  */
	      
	      for (cur_insn = indirect_jump_sequence; cur_insn;
		   cur_insn = NEXT_INSN (cur_insn))
		{
		  BLOCK_FOR_INSN (cur_insn) = cur_bb;
		  if (JUMP_P (cur_insn))
		    jump_insn = cur_insn;
		}
	      
	      /* Insert the new (indirect) jump sequence immediately before
		 the unconditional jump, then delete the unconditional jump.  */
	      
	      emit_insn_before (indirect_jump_sequence, last_insn);
	      delete_insn (last_insn);
	      
	      /* Make BB_END for cur_bb be the jump instruction (NOT the
		 barrier instruction at the end of the sequence...).  */
	      
	      BB_END (cur_bb) = jump_insn;
	    }
	}
    }
}

/* Add REG_CROSSING_JUMP note to all crossing jump insns.  */

static void
add_reg_crossing_jump_notes (void)
{
  basic_block bb;
  edge e;

  FOR_EACH_BB (bb)
    for (e = bb->succ; e; e = e->succ_next)
      if (e->crossing_edge
	  && JUMP_P (BB_END (e->src)))
	REG_NOTES (BB_END (e->src)) = gen_rtx_EXPR_LIST (REG_CROSSING_JUMP, 
							 NULL_RTX, 
						         REG_NOTES (BB_END 
								  (e->src)));
}

/* Basic blocks containing NOTE_INSN_UNLIKELY_EXECUTED_CODE will be
   put in a separate section of the .o file, to reduce paging and
   improve cache performance (hopefully).  This can result in bits of
   code from the same function being widely separated in the .o file.
   However this is not obvious to the current bb structure.  Therefore
   we must take care to ensure that: 1). There are no fall_thru edges
   that cross between sections;  2). For those architectures which
   have "short" conditional branches, all conditional branches that
   attempt to cross between sections are converted to unconditional
   branches; and, 3). For those architectures which have "short"
   unconditional branches, all unconditional branches that attempt
   to cross between sections are converted to indirect jumps.
   
   The code for fixing up fall_thru edges that cross between hot and
   cold basic blocks does so by creating new basic blocks containing 
   unconditional branches to the appropriate label in the "other" 
   section.  The new basic block is then put in the same (hot or cold)
   section as the original conditional branch, and the fall_thru edge
   is modified to fall into the new basic block instead.  By adding
   this level of indirection we end up with only unconditional branches
   crossing between hot and cold sections.  
   
   Conditional branches are dealt with by adding a level of indirection.
   A new basic block is added in the same (hot/cold) section as the 
   conditional branch, and the conditional branch is retargeted to the
   new basic block.  The new basic block contains an unconditional branch
   to the original target of the conditional branch (in the other section).

   Unconditional branches are dealt with by converting them into
   indirect jumps.  */

static void 
fix_edges_for_rarely_executed_code (edge *crossing_edges, 
				    int n_crossing_edges)
{
  /* Make sure the source of any crossing edge ends in a jump and the
     destination of any crossing edge has a label.  */
  
  add_labels_and_missing_jumps (crossing_edges, n_crossing_edges);
  
  /* Convert all crossing fall_thru edges to non-crossing fall
     thrus to unconditional jumps (that jump to the original fall
     thru dest).  */
  
  fix_up_fall_thru_edges ();
  
  /* If the architecture does not have conditional branches that can
     span all of memory, convert crossing conditional branches into
     crossing unconditional branches.  */
  
  if (!HAS_LONG_COND_BRANCH)
    fix_crossing_conditional_branches ();
  
  /* If the architecture does not have unconditional branches that
     can span all of memory, convert crossing unconditional branches
     into indirect jumps.  Since adding an indirect jump also adds
     a new register usage, update the register usage information as
     well.  */
  
  if (!HAS_LONG_UNCOND_BRANCH)
    {
      fix_crossing_unconditional_branches ();
      reg_scan (get_insns(), max_reg_num (), 1);
    }

  add_reg_crossing_jump_notes ();
}

/* Reorder basic blocks.  The main entry point to this file.  FLAGS is
   the set of flags to pass to cfg_layout_initialize().  */

void
reorder_basic_blocks (unsigned int flags)
{
  int n_traces;
  int i;
  struct trace *traces;

  if (n_basic_blocks <= 1)
    return;

  if (targetm.cannot_modify_jumps_p ())
    return;

  timevar_push (TV_REORDER_BLOCKS);

  cfg_layout_initialize (flags);

  set_edge_can_fallthru_flag ();
  mark_dfs_back_edges ();

  /* We are estimating the length of uncond jump insn only once since the code
     for getting the insn length always returns the minimal length now.  */
  if (uncond_jump_length == 0)
    uncond_jump_length = get_uncond_jump_length ();

  /* We need to know some information for each basic block.  */
  array_size = GET_ARRAY_SIZE (last_basic_block);
  bbd = xmalloc (array_size * sizeof (bbro_basic_block_data));
  for (i = 0; i < array_size; i++)
    {
      bbd[i].start_of_trace = -1;
      bbd[i].end_of_trace = -1;
      bbd[i].heap = NULL;
      bbd[i].node = NULL;
    }

  traces = xmalloc (n_basic_blocks * sizeof (struct trace));
  n_traces = 0;
  find_traces (&n_traces, traces);
  connect_traces (n_traces, traces);
  FREE (traces);
  FREE (bbd);

  if (dump_file)
    dump_flow_info (dump_file);

  if (flag_reorder_blocks_and_partition)
    add_unlikely_executed_notes ();

  cfg_layout_finalize ();

  timevar_pop (TV_REORDER_BLOCKS);
}

/* This function is the main 'entrance' for the optimization that
   partitions hot and cold basic blocks into separate sections of the
   .o file (to improve performance and cache locality).  Ideally it
   would be called after all optimizations that rearrange the CFG have
   been called.  However part of this optimization may introduce new
   register usage, so it must be called before register allocation has
   occurred.  This means that this optimization is actually called
   well before the optimization that reorders basic blocks (see function
   above).

   This optimization checks the feedback information to determine
   which basic blocks are hot/cold and adds
   NOTE_INSN_UNLIKELY_EXECUTED_CODE to non-hot basic blocks.  The
   presence or absence of this note is later used for writing out
   sections in the .o file.  This optimization must also modify the
   CFG to make sure there are no fallthru edges between hot & cold
   blocks, as those blocks will not necessarily be contiguous in the
   .o (or assembly) file; and in those cases where the architecture
   requires it, conditional and unconditional branches that cross
   between sections are converted into unconditional or indirect
   jumps, depending on what is appropriate.  */

void
partition_hot_cold_basic_blocks (void)
{
  basic_block cur_bb;
  edge *crossing_edges;
  int n_crossing_edges;
  int max_edges = 2 * last_basic_block;
  
  if (n_basic_blocks <= 1)
    return;
  
  crossing_edges = xcalloc (max_edges, sizeof (edge));

  cfg_layout_initialize (0);
  
  FOR_EACH_BB (cur_bb)
    if (cur_bb->index >= 0
 	&& cur_bb->next_bb->index >= 0)
      cur_bb->rbi->next = cur_bb->next_bb;
  
  find_rarely_executed_basic_blocks_and_crossing_edges (crossing_edges, 
							&n_crossing_edges, 
							&max_edges);

  if (n_crossing_edges > 0)
    fix_edges_for_rarely_executed_code (crossing_edges, n_crossing_edges);
  
  free (crossing_edges);

  cfg_layout_finalize();
}
