#include "cruby.h"

VALUE
llrb_insn_opt_eq(VALUE lhs, VALUE rhs)
{
  return rb_funcall(lhs, '+', 1, rhs);
}