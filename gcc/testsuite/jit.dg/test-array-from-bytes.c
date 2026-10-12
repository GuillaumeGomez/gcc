#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libgccjit.h"

#include "harness.h"

#ifndef LIBGCCJIT_HAVE_gcc_jit_context_new_array_from_bytes
#error LIBGCCJIT_HAVE_gcc_jit_context_new_array_from_bytes was not defined
#endif

/* Bytes with an interior NUL, a non-ASCII byte and no NUL terminator.  */
static const unsigned char array_from_bytes_bytes[5] = { 'a', 0, 'b', 0xff, 'c' };

void
create_code (gcc_jit_context *ctxt, void *user_data)
{
  /* Let's try to inject the equivalent of:

       unsigned char array_from_bytes_array[5] = "a\0b\377c";

       struct S { int i; char arr[5]; } array_from_bytes_struct
         = { 42, "a\0b\377c" };
  */
  gcc_jit_type *uchar_type =
    gcc_jit_context_get_type (ctxt, GCC_JIT_TYPE_UNSIGNED_CHAR);
  gcc_jit_type *char_type =
    gcc_jit_context_get_type (ctxt, GCC_JIT_TYPE_CHAR);
  gcc_jit_type *int_type =
    gcc_jit_context_get_type (ctxt, GCC_JIT_TYPE_INT);

  gcc_jit_type *uchar_array_type =
    gcc_jit_context_new_array_type (ctxt, NULL, uchar_type, 5);
  gcc_jit_rvalue *array_rvalue =
    gcc_jit_context_new_array_from_bytes (ctxt, NULL, uchar_array_type,
					  array_from_bytes_bytes, sizeof (array_from_bytes_bytes));
  CHECK_STRING_VALUE (gcc_jit_object_get_debug_string (
			gcc_jit_rvalue_as_object (array_rvalue)),
		      "\"a\\000b\\377c\"");

  gcc_jit_lvalue *test_array =
    gcc_jit_context_new_global (ctxt, NULL, GCC_JIT_GLOBAL_EXPORTED,
				uchar_array_type,
				"array_from_bytes_array");
  gcc_jit_global_set_initializer_rvalue (test_array, array_rvalue);

  /* Use it as a field of a struct constructor too.  */
  gcc_jit_type *char_array_type =
    gcc_jit_context_new_array_type (ctxt, NULL, char_type, 5);
  gcc_jit_field *fields[2] = {
    gcc_jit_context_new_field (ctxt, NULL, int_type, "i"),
    gcc_jit_context_new_field (ctxt, NULL, char_array_type, "arr"),
  };
  gcc_jit_struct *struct_type =
    gcc_jit_context_new_struct_type (ctxt, NULL, "S", 2, fields);
  gcc_jit_rvalue *values[2] = {
    gcc_jit_context_new_rvalue_from_int (ctxt, int_type, 42),
    gcc_jit_context_new_array_from_bytes (ctxt, NULL, char_array_type,
					  array_from_bytes_bytes, sizeof (array_from_bytes_bytes)),
  };
  gcc_jit_rvalue *struct_rvalue =
    gcc_jit_context_new_struct_constructor (
      ctxt, NULL, gcc_jit_struct_as_type (struct_type), 2, fields, values);

  gcc_jit_lvalue *test_struct =
    gcc_jit_context_new_global (ctxt, NULL, GCC_JIT_GLOBAL_EXPORTED,
				gcc_jit_struct_as_type (struct_type),
				"array_from_bytes_struct");
  gcc_jit_global_set_initializer_rvalue (test_struct, struct_rvalue);
}

struct array_from_bytes_s
{
  int i;
  char arr[5];
};

void
verify_code (gcc_jit_context *ctxt, gcc_jit_result *result)
{
  CHECK_NON_NULL (result);

  const unsigned char *test_array =
    (const unsigned char *)gcc_jit_result_get_global (result, "array_from_bytes_array");
  CHECK_NON_NULL (test_array);
  CHECK_VALUE (memcmp (test_array, array_from_bytes_bytes, sizeof (array_from_bytes_bytes)), 0);

  const struct array_from_bytes_s *test_struct =
    (const struct array_from_bytes_s *)gcc_jit_result_get_global (result, "array_from_bytes_struct");
  CHECK_NON_NULL (test_struct);
  CHECK_VALUE (test_struct->i, 42);
  CHECK_VALUE (memcmp (test_struct->arr, array_from_bytes_bytes, sizeof (array_from_bytes_bytes)), 0);
}
