/* { dg-do compile } */
/* { dg-options "-march=rv64gcv_zvfh_zvl4096b -mabi=lp64d -O3 --param=riscv-autovec-lmul=m8 -ffast-math -fdump-tree-optimized-details" } */

#include "def.h"

DEF_REDUC_MAXMIN (double, max, >, 4)
DEF_REDUC_MAXMIN (double, max, >, 8)
DEF_REDUC_MAXMIN (double, max, >, 16)
DEF_REDUC_MAXMIN (double, max, >, 32)
DEF_REDUC_MAXMIN (double, max, >, 64)
DEF_REDUC_MAXMIN (double, max, >, 128)
DEF_REDUC_MAXMIN (double, max, >, 256)
DEF_REDUC_MAXMIN (double, max, >, 512)

DEF_REDUC_MAXMIN (double, min, <, 4)
DEF_REDUC_MAXMIN (double, min, <, 8)
DEF_REDUC_MAXMIN (double, min, <, 16)
DEF_REDUC_MAXMIN (double, min, <, 32)
DEF_REDUC_MAXMIN (double, min, <, 64)
DEF_REDUC_MAXMIN (double, min, <, 128)
DEF_REDUC_MAXMIN (double, min, <, 256)
DEF_REDUC_MAXMIN (double, min, <, 512)

/* { dg-final { scan-assembler-times {vfredmax\.vs} 8 } } */
/* { dg-final { scan-assembler-times {vfredmin\.vs} 8 } } */
/* { dg-final { scan-assembler-not {csrr} } } */
/* { dg-final { scan-tree-dump-not "1,1" "optimized" } } */
/* { dg-final { scan-tree-dump-not "2,2" "optimized" } } */
/* { dg-final { scan-tree-dump-not "4,4" "optimized" } } */
/* { dg-final { scan-tree-dump-not "16,16" "optimized" } } */
/* { dg-final { scan-tree-dump-not "32,32" "optimized" } } */
/* { dg-final { scan-tree-dump-not "64,64" "optimized" } } */
/* { dg-final { scan-tree-dump-not "128,128" "optimized" } } */
/* { dg-final { scan-tree-dump-not "256,256" "optimized" } } */
/* { dg-final { scan-tree-dump-not "512,512" "optimized" } } */
/* { dg-final { scan-tree-dump-not "1024,1024" "optimized" } } */
/* { dg-final { scan-tree-dump-not "2048,2048" "optimized" } } */
/* { dg-final { scan-tree-dump-not "4096,4096" "optimized" } } */
