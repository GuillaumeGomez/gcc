/* { dg-do compile { target x86_64-*-* } } */

#include <stdlib.h>
#include <stdio.h>

#include "libgccjit.h"

#define TEST_COMPILING_TO_FILE
#define OUTPUT_KIND      GCC_JIT_OUTPUT_KIND_ASSEMBLER
#define OUTPUT_FILENAME  "output-of-test-array-from-bytes-asm.c.s"
#include "harness.h"

static void
make_global (gcc_jit_context *ctxt, const char *name, const char *bytes,
	     size_t num_bytes)
{
  gcc_jit_type *uchar_type =
    gcc_jit_context_get_type (ctxt, GCC_JIT_TYPE_UNSIGNED_CHAR);
  gcc_jit_type *array_type =
    gcc_jit_context_new_array_type (ctxt, NULL, uchar_type, num_bytes);
  gcc_jit_lvalue *global =
    gcc_jit_context_new_global (ctxt, NULL, GCC_JIT_GLOBAL_EXPORTED,
				array_type, name);
  gcc_jit_global_set_initializer_rvalue (
    global,
    gcc_jit_context_new_array_from_bytes (ctxt, NULL, array_type, bytes,
					  num_bytes));
}

void
create_code (gcc_jit_context *ctxt, void *user_data)
{
  /* Let's try to inject the equivalent of:

       unsigned char not_nul_terminated[5] = "tadam";
       unsigned char nul_terminated[4] = "foo";

     and check that they're emitted as strings rather than byte by
     byte.  */
  make_global (ctxt, "not_nul_terminated", "tadam", 5);
  make_global (ctxt, "nul_terminated", "foo", 4);
}

/* { dg-final { jit-verify-output-file-was-created "" } } */
/* { dg-final { jit-verify-assembler-output "not_nul_terminated:\\s+\\.ascii\\s+\"tadam\"" } } */
/* { dg-final { jit-verify-assembler-output "nul_terminated:\\s+\\.string\\s+\"foo\"" } } */
