#include <stdbool.h>
#include "llvm-c/Core.h"
#include "llvm-c/ExecutionEngine.h"
#include "ruby.h"
#include "compiler.h"
#include "insns.inc"
#include "insns_info.inc"

static VALUE rb_eCompileError;

// Emulates rb_control_frame's sp, which is function local
struct llrb_cfstack {
  unsigned int size;
  unsigned int max;
  LLVMValueRef *body;
};

// Store compiler's internal state and shared variables
struct llrb_compiler {
  const struct rb_iseq_constant_body *body;
  const char *funcname;
  LLVMBuilderRef builder;
  LLVMModuleRef mod;
  VALUE block_by_start;
  VALUE block_end_by_start;
};

static LLVMValueRef
llvm_value(VALUE value)
{
  return LLVMConstInt(LLVMInt64Type(), value, false); // TODO: support 32bit for VALUE type
}

static void
llrb_stack_push(struct llrb_cfstack *stack, LLVMValueRef value)
{
  if (stack->size >= stack->max) {
    rb_raise(rb_eCompileError, "LLRB's internal stack overflow: max=%d, next size=%d", stack->max, stack->size+1);
  }
  stack->body[stack->size] = value;
  stack->size++;
}

static LLVMValueRef
llrb_stack_pop(struct llrb_cfstack *stack)
{
  if (stack->size <= 0) {
    rb_raise(rb_eCompileError, "LLRB's internal stack underflow: next size=%d", stack->size-1);
  }
  stack->size--;
  return stack->body[stack->size];
}

// Don't use `rb_iseq_original_iseq` to avoid unnecessary memory allocation.
int rb_vm_insn_addr2insn(const void *addr);

// Return sorted unique Ruby array of BasicBlock start positions like [0, 2, 8].
//
// It's constructed in the following rule.
//   Rule 1: 0 is always included
//   Rule 2: All TS_OFFSET numers are included
//   Rule 3: Positions immediately after jump, branchif and branchunless are included
static VALUE
llrb_basic_block_starts(const struct rb_iseq_constant_body *body)
{
  // Rule 1
  VALUE starts = rb_ary_new_capa(1);
  rb_ary_push(starts, INT2FIX(0));

  for (unsigned int i = 0; i < body->iseq_size;) {
    int insn = rb_vm_insn_addr2insn((void *)body->iseq_encoded[i]);

    // Rule 2
    for (int j = 1; j < insn_len(insn); j++) {
      VALUE op = body->iseq_encoded[i+j];
      switch (insn_op_type(insn, j-1)) {
        case TS_OFFSET:
          rb_ary_push(starts, INT2FIX((int)(i+insn_len(insn)+op)));
          break;
      }
    }

    // Rule 3
    switch (insn) {
      case YARVINSN_jump:
      case YARVINSN_branchif:
      case YARVINSN_branchunless:
        rb_ary_push(starts, INT2FIX(i+insn_len(insn)));
        break;
    }

    i += insn_len(insn);
  }
  starts = rb_ary_sort_bang(starts);
  rb_funcall(starts, rb_intern("uniq!"), 0);
  return starts;
}

// Given llrb_basic_block_starts result like [0, 2, 8, 9], it returns a Hash
// whose value specifies basic block ends like { 0 => 1, 2 => 7, 8 => 8, 9 => 10 }.
//
// TODO: Note that sometimes "end" points to actual end's operands. This may hit a bug... So fix it later for safety.
static VALUE
llrb_basic_block_end_by_start(const struct rb_iseq_constant_body *body)
{
  VALUE end_by_start = rb_hash_new();
  VALUE starts = llrb_basic_block_starts(body); // TODO: free? And it's duplicated with llrb_basic_block_by_start.

  for (long i = 0; i < RARRAY_LEN(starts); i++) {
    VALUE start = RARRAY_AREF(starts, i);

    if (i == RARRAY_LEN(starts)-1) {
      rb_hash_aset(end_by_start, start, INT2FIX(body->iseq_size-1)); // This assumes the end is always "leave". Is it true?
    } else {
      VALUE next_start = RARRAY_AREF(starts, i+1);
      rb_hash_aset(end_by_start, start, INT2FIX(FIX2INT(next_start)-1));
    }
  }
  return end_by_start;
}

// Given basic block starts like [0, 2, 8], it returns a Hash like
// { 0 => label_0 BasicBlock, 2 => label_2 BasicBlock, 8 => label_8 BasicBlock }.
//
// Those blocks are appended to a given func.
// This hash and its blocks are necessary to be built first for forwared reference.
static VALUE
llrb_basic_block_by_start(const struct rb_iseq_constant_body *body, LLVMValueRef func)
{
  VALUE block_by_start = rb_hash_new();
  VALUE starts = llrb_basic_block_starts(body); // TODO: free?

  for (long i = 0; i < RARRAY_LEN(starts); i++) {
    VALUE start = RARRAY_AREF(starts, i);
    VALUE label = rb_str_new_cstr("label_"); // TODO: free?
    rb_str_catf(label, "%d", FIX2INT(start));
    rb_hash_aset(block_by_start, start, (VALUE)LLVMAppendBasicBlock(func, RSTRING_PTR(label)));
  }
  return block_by_start;
}

static void
llrb_disasm_insns(const struct rb_iseq_constant_body *body)
{
  fprintf(stderr, "\n== disasm: LLRB ================================\n");
  for (unsigned int i = 0; i < body->iseq_size;) {
    int insn = rb_vm_insn_addr2insn((void *)body->iseq_encoded[i]);
    fprintf(stderr, "%04d %-27s [%-4s] ", i, insn_name(insn), insn_op_types(insn));

    for (int j = 1; j < insn_len(insn); j++) {
      VALUE op = body->iseq_encoded[i+j];
      switch (insn_op_type(insn, j-1)) {
        case TS_VALUE:
          fprintf(stderr, "%-4s ", RSTRING_PTR(rb_inspect(op)));
          break;
        case TS_NUM:
          fprintf(stderr, "%-4ld ", (rb_num_t)op);
          break;
        case TS_OFFSET:
          fprintf(stderr, "%"PRIdVALUE" ", (VALUE)(i + j + op + 1));
          break;
      }
    }
    fprintf(stderr, "\n");
    i += insn_len(insn);
  }
  VALUE starts = llrb_basic_block_starts(body); // TODO: free?
  VALUE end_by_start = llrb_basic_block_end_by_start(body); // TODO: free?
  fprintf(stderr, "\nbasic block starts: %s\n", RSTRING_PTR(rb_inspect(starts)));
  fprintf(stderr, "basic block ends by starts: %s\n\n", RSTRING_PTR(rb_inspect(end_by_start)));
}

LLVMValueRef
llrb_argument_at(struct llrb_compiler *c, unsigned index)
{
  LLVMValueRef func = LLVMGetNamedFunction(c->mod, c->funcname);
  return LLVMGetParam(func, index);
}

// In base 2, RTEST is: (v != Qfalse && v != Qnil) -> (v != 0000 && v != 1000) -> (v & 0111) != 0000 -> (v & ~Qnil) != 0
static LLVMValueRef
llrb_build_rtest(LLVMBuilderRef builder, LLVMValueRef value)
{
  LLVMValueRef masked = LLVMBuildAnd(builder, value, llvm_value(~Qnil), "RTEST_mask");
  return LLVMBuildICmp(builder, LLVMIntNE, masked, llvm_value(0), "RTEST");
}

static LLVMValueRef
llrb_get_function(LLVMModuleRef mod, const char *name)
{
  LLVMValueRef func = LLVMGetNamedFunction(mod, "rb_funcall");
  if (func) return func;

  if (!strcmp(name, "rb_funcall")) {
    LLVMTypeRef arg_types[] = { LLVMInt64Type(), LLVMInt64Type() };
    return LLVMAddFunction(mod, "rb_funcall", LLVMFunctionType(LLVMInt64Type(), arg_types, 2, true));
  } else {
    rb_raise(rb_eCompileError, "'%s' is not defined in llrb_get_function", name);
  }
}

static LLVMValueRef
llrb_compile_funcall(struct llrb_compiler *c, struct llrb_cfstack *stack, ID mid, int argc)
{
  LLVMValueRef func = llrb_get_function(c->mod, "rb_funcall");
  LLVMValueRef *args = ALLOC_N(LLVMValueRef, 3+argc); // 3 is recv, mid, n

  for (int i = argc-1; 0 <= i; i--) {
    args[3+i] = llrb_stack_pop(stack); // 3 is recv, mid, n
  }
  args[0] = llrb_stack_pop(stack);
  args[1] = llvm_value(mid);
  args[2] = LLVMConstInt(LLVMInt32Type(), argc, false);

  return LLVMBuildCall(c->builder, func, args, 3+argc, "rb_funcall");
}

static void llrb_compile_phi_block(struct llrb_compiler *c, unsigned int start, LLVMValueRef *incoming_values, LLVMBasicBlockRef *incoming_blocks, unsigned count);
static LLVMValueRef llrb_compile_branch_block(struct llrb_compiler *c, unsigned int start, unsigned int *jump_dest);
static void llrb_compile_basic_block(struct llrb_compiler *c, unsigned int start);

// @return position of next instruction
static unsigned int
llrb_compile_insn(struct llrb_compiler *c, struct llrb_cfstack *stack, const unsigned int pos, const int insn, const VALUE *operands)
{
  switch (insn) {
    //case YARVINSN_nop:
    //case YARVINSN_getlocal:
    //case YARVINSN_setlocal:
    //case YARVINSN_getspecial:
    //case YARVINSN_setspecial:
    //case YARVINSN_getinstancevariable:
    //case YARVINSN_setinstancevariable:
    //case YARVINSN_getclassvariable:
    //case YARVINSN_setclassvariable:
    //case YARVINSN_getconstant:
    //case YARVINSN_setconstant:
    //case YARVINSN_getglobal:
    //case YARVINSN_setglobal:
    case YARVINSN_putnil:
      llrb_stack_push(stack, llvm_value(Qnil));
      break;
    //case YARVINSN_putself:
    case YARVINSN_putobject:
      llrb_stack_push(stack, llvm_value(operands[0]));
      break;
    //case YARVINSN_putspecialobject:
    //case YARVINSN_putiseq:
    //case YARVINSN_putstring:
    //case YARVINSN_concatstrings:
    //case YARVINSN_tostring:
    //case YARVINSN_freezestring:
    //case YARVINSN_toregexp:
    //case YARVINSN_newarray:
    //case YARVINSN_duparray:
    //case YARVINSN_expandarray:
    //case YARVINSN_concatarray:
    //case YARVINSN_splatarray:
    //case YARVINSN_newhash:
    //case YARVINSN_newrange:
    //case YARVINSN_pop:
    //case YARVINSN_dup:
    //case YARVINSN_dupn:
    //case YARVINSN_swap:
    //case YARVINSN_reverse:
    //case YARVINSN_reput:
    //case YARVINSN_topn:
    //case YARVINSN_setn:
    //case YARVINSN_adjuststack:
    //case YARVINSN_defined:
    //case YARVINSN_checkmatch:
    //case YARVINSN_checkkeyword:
    case YARVINSN_trace:
      break; // TODO: implement
    //case YARVINSN_defineclass:
    //case YARVINSN_send:
    //case YARVINSN_opt_str_freeze:
    //case YARVINSN_opt_newarray_max:
    //case YARVINSN_opt_newarray_min:
    //case YARVINSN_opt_send_without_block:
    //case YARVINSN_invokesuper:
    //case YARVINSN_invokeblock:
    case YARVINSN_leave:
      if (stack->size != 1) {
        rb_raise(rb_eCompileError, "unexpected stack size at leave: %d", stack->size);
      }
      LLVMBuildRet(c->builder, llrb_stack_pop(stack));
      break;
    //case YARVINSN_throw:
    case YARVINSN_jump:
      return pos + (unsigned)insn_len(insn) + operands[0];
    //case YARVINSN_branchif:
    case YARVINSN_branchunless: {
      unsigned branch_dest = pos + (unsigned)insn_len(insn) + operands[0];
      unsigned fallthrough = pos + (unsigned)insn_len(insn);
      LLVMBasicBlockRef branch_dest_block = (LLVMBasicBlockRef)rb_hash_aref(c->block_by_start, INT2FIX(branch_dest));
      LLVMBasicBlockRef fallthrough_block = (LLVMBasicBlockRef)rb_hash_aref(c->block_by_start, INT2FIX(fallthrough));

      LLVMValueRef cond = llrb_stack_pop(stack);
      LLVMBuildCondBr(c->builder, llrb_build_rtest(c->builder, cond), fallthrough_block, branch_dest_block);

      unsigned int branch_dest_dest, fallthrough_dest;
      LLVMValueRef results[] = {
        llrb_compile_branch_block(c, branch_dest, &branch_dest_dest),
        llrb_compile_branch_block(c, fallthrough, &fallthrough_dest),
      };
      LLVMBasicBlockRef blocks[] = { branch_dest_block, fallthrough_block };
      if (branch_dest_dest != fallthrough_dest) {
        rb_raise(rb_eCompileError, "branch_dest_dest (%d) != fallthrough_dest (%d)", branch_dest_dest, fallthrough_dest);
      }

      // FIXME: pass stack to phi block
      llrb_compile_phi_block(c, fallthrough_dest, results, blocks, 2);
      break;
    }
    //case YARVINSN_branchnil:
    //case YARVINSN_getinlinecache:
    //case YARVINSN_setinlinecache:
    //case YARVINSN_once:
    //case YARVINSN_opt_case_dispatch:
    case YARVINSN_opt_plus:
      llrb_stack_push(stack, llrb_compile_funcall(c, stack, '+', 1));
      break;
    case YARVINSN_opt_minus:
      llrb_stack_push(stack, llrb_compile_funcall(c, stack, '-', 1));
      break;
    //case YARVINSN_opt_mult:
    //case YARVINSN_opt_div:
    //case YARVINSN_opt_mod:
    //case YARVINSN_opt_eq:
    //case YARVINSN_opt_neq:
    //case YARVINSN_opt_lt:
    //case YARVINSN_opt_le:
    //case YARVINSN_opt_gt:
    //case YARVINSN_opt_ge:
    //case YARVINSN_opt_ltlt:
    //case YARVINSN_opt_aref:
    //case YARVINSN_opt_aset:
    //case YARVINSN_opt_aset_with:
    //case YARVINSN_opt_aref_with:
    //case YARVINSN_opt_length:
    //case YARVINSN_opt_size:
    //case YARVINSN_opt_empty_p:
    //case YARVINSN_opt_succ:
    //case YARVINSN_opt_not:
    //case YARVINSN_opt_regexpmatch1:
    //case YARVINSN_opt_regexpmatch2:
    //case YARVINSN_opt_call_c_function:
    //case YARVINSN_bitblt:
    //case YARVINSN_answer:
    case YARVINSN_getlocal_OP__WC__0: {
      unsigned local_size = c->body->local_table_size;
      unsigned arg_size   = c->body->param.size;
      llrb_stack_push(stack, llrb_argument_at(c, 3 - local_size + 2 * arg_size - (unsigned)operands[0])); // XXX: is this okay????
      break;
    }
    //case YARVINSN_getlocal_OP__WC__1:
    //case YARVINSN_setlocal_OP__WC__0:
    //case YARVINSN_setlocal_OP__WC__1:
    case YARVINSN_putobject_OP_INT2FIX_O_0_C_:
      llrb_stack_push(stack, llvm_value(INT2FIX(0)));
      break;
    case YARVINSN_putobject_OP_INT2FIX_O_1_C_:
      llrb_stack_push(stack, llvm_value(INT2FIX(1)));
      break;
    default:
      llrb_disasm_insns(c->body);
      rb_raise(rb_eCompileError, "Unhandled insn at llrb_compile_insn: %s", insn_name(insn));
      break;
  }
  return pos + (unsigned)insn_len(insn);
}


// Creates phi node in the first of the block.
static void
llrb_compile_phi_block(struct llrb_compiler *c, unsigned int start, LLVMValueRef *incoming_values, LLVMBasicBlockRef *incoming_blocks, unsigned count)
{
  LLVMBasicBlockRef block = (LLVMBasicBlockRef)rb_hash_aref(c->block_by_start, INT2FIX(start));
  LLVMPositionBuilderAtEnd(c->builder, block);

  struct llrb_cfstack stack = (struct llrb_cfstack){
    .body = ALLOC_N(LLVMValueRef, c->body->stack_max),
    .size = 0,
    .max  = c->body->stack_max
  };

  LLVMValueRef phi = LLVMBuildPhi(c->builder, LLVMInt64Type(), "llrb_compile_phi_block");
  LLVMAddIncoming(phi, incoming_values, incoming_blocks, count);
  llrb_stack_push(&stack, phi);

  VALUE block_end = rb_hash_aref(c->block_end_by_start, INT2FIX(start));
  for (unsigned int i = start; i <= (unsigned int)FIX2INT(block_end);) {
    int insn = rb_vm_insn_addr2insn((void *)c->body->iseq_encoded[i]);
    llrb_compile_insn(c, &stack, i, insn, c->body->iseq_encoded + (i+1));
    i += insn_len(insn);
  }
  return;
}

// After block evaluation, this function expects the block to have one value in a stack and return jump destination.
static LLVMValueRef
llrb_compile_branch_block(struct llrb_compiler *c, unsigned int start, unsigned int *jump_dest)
{
  LLVMBasicBlockRef block = (LLVMBasicBlockRef)rb_hash_aref(c->block_by_start, INT2FIX(start));
  LLVMPositionBuilderAtEnd(c->builder, block);

  struct llrb_cfstack stack = (struct llrb_cfstack){
    .body = ALLOC_N(LLVMValueRef, c->body->stack_max),
    .size = 0,
    .max  = c->body->stack_max
  };

  VALUE block_end = rb_hash_aref(c->block_end_by_start, INT2FIX(start));
  for (unsigned int i = start; i <= (unsigned int)FIX2INT(block_end);) {
    int insn = rb_vm_insn_addr2insn((void *)c->body->iseq_encoded[i]);
    unsigned int dest = llrb_compile_insn(c, &stack, i, insn, c->body->iseq_encoded + (i+1));
    // TODO: assert dest pos for non-last?
    i += insn_len(insn);

    if ((unsigned int)FIX2INT(block_end) < i) {
      *jump_dest = dest;
      LLVMBasicBlockRef br_block = (LLVMBasicBlockRef)rb_hash_aref(c->block_by_start, INT2FIX(dest));
      LLVMBuildBr(c->builder, br_block);
    }
  }

  if (stack.size != 1) {
    rb_raise(rb_eCompileError, "unexpected stack size at llrb_compile_branch_block: %d", stack.size);
  }
  return llrb_stack_pop(&stack);
}

static void
llrb_compile_basic_block(struct llrb_compiler *c, unsigned int start)
{
  LLVMBasicBlockRef block = (LLVMBasicBlockRef)rb_hash_aref(c->block_by_start, INT2FIX(start));
  LLVMPositionBuilderAtEnd(c->builder, block);

  struct llrb_cfstack stack = (struct llrb_cfstack){
    .body = ALLOC_N(LLVMValueRef, c->body->stack_max),
    .size = 0,
    .max  = c->body->stack_max
  };

  VALUE block_end = rb_hash_aref(c->block_end_by_start, INT2FIX(start));
  for (unsigned int i = start; i <= (unsigned int)FIX2INT(block_end);) {
    int insn = rb_vm_insn_addr2insn((void *)c->body->iseq_encoded[i]);
    llrb_compile_insn(c, &stack, i, insn, c->body->iseq_encoded + (i+1));
    i += insn_len(insn);
  }
  return;
}

LLVMModuleRef
llrb_compile_iseq(const rb_iseq_t *iseq, const char* funcname)
{
  LLVMModuleRef mod = LLVMModuleCreateWithName("llrb");

  LLVMTypeRef *arg_types = ALLOC_N(LLVMTypeRef, iseq->body->param.size+1);
  for (unsigned i = 0; i <= iseq->body->param.size; i++) arg_types[i] = LLVMInt64Type();
  LLVMValueRef func = LLVMAddFunction(mod, funcname,
      LLVMFunctionType(LLVMInt64Type(), arg_types, iseq->body->param.size+1, false));

  struct llrb_compiler compiler = (struct llrb_compiler){
    .body = iseq->body,
    .funcname = funcname,
    .builder = LLVMCreateBuilder(),
    .mod = mod,
    .block_by_start = llrb_basic_block_by_start(iseq->body, func), // TODO: free?
    .block_end_by_start = llrb_basic_block_end_by_start(iseq->body) // TODO: free?
  };

  llrb_compile_basic_block(&compiler, 0);
  //LLVMDumpModule(mod);
  return mod;
}

void
Init_compiler(VALUE rb_mJIT)
{
  rb_eCompileError = rb_define_class_under(rb_mJIT, "CompileError", rb_eStandardError);
}