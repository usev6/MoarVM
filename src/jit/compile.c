#include "moar.h"
#include "internal.h"
#include "platform/mmap.h"


void MVM_jit_compiler_init(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *jg);
void MVM_jit_compiler_deinit(MVMThreadContext *tc, MVMJitCompiler *compiler);
MVMJitCode * MVM_jit_compiler_assemble(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *jg);
void MVM_jit_compile_expr_tree(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *graph, MVMJitExprTree *tree);

#define COPY_ARRAY(a, n) memcpy(MVM_malloc(n * sizeof(a[0])), a, n * sizeof(a[0]))

static const MVMuint16 MAGIC_BYTECODE[] = { MVM_OP_sp_jit_enter, 0 };

void MVM_jit_compiler_init(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitGraph *jg) {
    MVMint32  num_globals = MVM_jit_num_globals();
    /* Create dasm state */
    dasm_init(cl, 1);
    cl->dasm_globals = MVM_malloc(num_globals * sizeof(void*));
    dasm_setupglobal(cl, cl->dasm_globals, num_globals);
    dasm_setup(cl, MVM_jit_actions());

    /* Store graph we're compiling */
    cl->graph        = jg;
    /* next (internal) label to assign */
    cl->label_offset = jg->num_labels;
    /* space for dynamic labels */
    dasm_growpc(cl, jg->num_labels);
    /* Offset in temporary array in which we can spill */
    cl->spill_offset = (jg->sg->num_locals + jg->sg->sf->body.cu->body.max_callsite_size) * MVM_JIT_REG_SZ;
    cl->max_spill    = 2*MVM_JIT_PTR_SZ;

}

void MVM_jit_compiler_deinit(MVMThreadContext *tc, MVMJitCompiler *cl) {
    dasm_free(cl);
    MVM_free(cl->dasm_globals);
}

MVMJitCode * MVM_jit_compile_graph(MVMThreadContext *tc, MVMJitGraph *jg) {
    MVMJitCompiler cl;
    MVMJitCode *code;
    MVMJitNode *node = jg->first_node;

    MVM_jit_log(tc, "Starting compilation\n");
    /* initialation */
    MVM_jit_compiler_init(tc, &cl, jg);
    /* generate code */
    MVM_jit_emit_prologue(tc, &cl, jg);
    while (node) {
        switch(node->type) {
        case MVM_JIT_NODE_LABEL:
            MVM_jit_emit_label(tc, &cl, jg, node->u.label.name);
            break;
        case MVM_JIT_NODE_PRIMITIVE:
            MVM_jit_emit_primitive(tc, &cl, jg, &node->u.prim);
            break;
        case MVM_JIT_NODE_BRANCH:
            MVM_jit_emit_block_branch(tc, &cl, jg, &node->u.branch);
            break;
        case MVM_JIT_NODE_CALL_C:
            MVM_jit_emit_call_c(tc, &cl, jg, &node->u.call);
            break;
        case MVM_JIT_NODE_GUARD:
            MVM_jit_emit_guard(tc, &cl, jg, &node->u.guard);
            break;
        case MVM_JIT_NODE_INVOKE:
            MVM_jit_emit_invoke(tc, &cl, jg, &node->u.invoke);
            break;
        case MVM_JIT_NODE_JUMPLIST:
            MVM_jit_emit_jumplist(tc, &cl, jg, &node->u.jumplist);
            break;
        case MVM_JIT_NODE_CONTROL:
            MVM_jit_emit_control(tc, &cl, jg, &node->u.control);
            break;
        case MVM_JIT_NODE_EXPR_TREE:
            MVM_jit_compile_expr_tree(tc, &cl, jg, node->u.tree);
            break;
        }
        node = node->next;
    }
    MVM_jit_emit_epilogue(tc, &cl, jg);

    /* Generate code */
    code = MVM_jit_compiler_assemble(tc, &cl, jg);

    /* Clear up the compiler */
    MVM_jit_compiler_deinit(tc, &cl);

    /* Logging for insight */
    if (tc->instance->jit_bytecode_dir) {
        MVM_jit_log_bytecode(tc, code);
    }
    if (tc->instance->jit_log_fh)
        fflush(tc->instance->jit_log_fh);
    return code;
}

MVMJitCode * MVM_jit_compiler_assemble(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitGraph *jg) {
    MVMJitCode * code;
    MVMint32 i;
    char * memory;
    size_t codesize;

   /* compile the function */
    if (dasm_link(cl, &codesize) != 0)
        MVM_oops(tc, "dynasm could not link :-(");
    memory = MVM_platform_alloc_pages(codesize, MVM_PAGE_READ|MVM_PAGE_WRITE);
    if (dasm_encode(cl, memory) != 0)
        MVM_oops(tc, "dynasm could not encode :-(");

    /* set memory readable + executable */
    MVM_platform_set_page_mode(memory, codesize, MVM_PAGE_READ|MVM_PAGE_EXEC);

    MVM_jit_log(tc, "Bytecode size: %"MVM_PRSz"\n", codesize);

    /* Create code segment */
    code = MVM_malloc(sizeof(MVMJitCode));
    code->func_ptr   = (void (*)(MVMThreadContext*,MVMCompUnit*,void*)) memory;
    code->size       = codesize;
    code->bytecode   = (MVMuint8*)MAGIC_BYTECODE;
    code->sf         = jg->sg->sf;
    code->spill_size = cl->max_spill;

    /* Get the basic block labels */
    code->num_labels = jg->num_labels;
    code->labels = MVM_calloc(code->num_labels, sizeof(void*));

    for (i = 0; i < code->num_labels; i++) {
        MVMint32 offset = dasm_getpclabel(cl, i);
        if (offset < 0)
            MVM_jit_log(tc, "Got negative offset for dynamic label %d\n", i);
        code->labels[i] = memory + offset;
    }

    /* Copy the deopts, inlines, and handlers. Because these use the
     * label index rather than the direct pointer, no fixup is
     * necessary */
    code->num_deopts   = jg->deopts_num;
    code->deopts       = code->num_deopts ? COPY_ARRAY(jg->deopts, jg->deopts_num) : NULL;
    code->num_handlers = jg->handlers_num;
    code->handlers     = code->num_handlers ? COPY_ARRAY(jg->handlers, jg->handlers_alloc) : NULL;
    code->num_inlines  = jg->inlines_num;
    code->inlines      = code->num_inlines ? COPY_ARRAY(jg->inlines, jg->inlines_alloc) : NULL;

    code->seq_nr = MVM_incr(&tc->instance->jit_seq_nr);

    return code;
}

void MVM_jit_destroy_code(MVMThreadContext *tc, MVMJitCode *code) {
    MVM_platform_free_pages(code->func_ptr, code->size);
    MVM_free(code->labels);
    MVM_free(code->deopts);
    MVM_free(code->handlers);
    MVM_free(code->inlines);
    MVM_free(code);
}


#define NYI(x) MVM_oops(tc, #x " NYI")


static void alloc_value(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprValue *value) {
    if (value->type == MVM_JIT_NUM) {
        MVMint8 reg = MVM_jit_register_alloc(tc, cl, MVM_JIT_REGCLS_NUM);
        MVM_jit_register_assign(tc, cl, value, MVM_JIT_REGCLS_NUM, reg);
    } else {
        MVMint8 reg = MVM_jit_register_alloc(tc, cl, MVM_JIT_REGCLS_GPR);
        MVM_jit_register_assign(tc, cl, value, MVM_JIT_REGCLS_GPR, reg);
    }
}

static void use_value(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprValue *value) {
    MVM_jit_register_use(tc, cl, value->reg_cls, value->reg_num);
}

static void release_value(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprValue *value) {
    MVM_jit_register_release(tc, cl, value->reg_cls, value->reg_num);
}

static void load_value(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprValue *value) {
    MVMint8 reg_num = MVM_jit_register_alloc(tc, cl, MVM_JIT_REGCLS_GPR);
    MVM_jit_register_load(tc, cl, value->spill_location, MVM_JIT_REGCLS_GPR, reg_num, value->size);
}

static void ensure_values(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprValue **values, MVMint32 num_values) {
    MVMint32 i;
    /* Ensure all register values are live */
    for (i = 0; i < num_values; i++) {
        MVMJitExprValue *value = values[i];
        if (value->type == MVM_JIT_REG) {
            if (value->state == MVM_JIT_VALUE_SPILLED)
                load_value(tc, compiler, value);
            else if (value->state == MVM_JIT_VALUE_EMPTY ||
                     value->state == MVM_JIT_VALUE_DEAD) {
                MVM_oops(tc, "Required Value Is Not Live");
            }
            /* Mark value as in-use */
            use_value(tc, compiler, value);
        }
    }
}



#if MVM_JIT_ARCH == MVM_JIT_ARCH_X64
/* Localized definitions winning! */
static MVMint8 x64_gpr_args[] = {
    X64_ARG_GPR(MVM_JIT_REGNAME)
};


static MVMint8 x64_sse_args[] = {
    X64_ARG_SSE(MVM_JIT_REGNAME)
};


#if MVM_JIT_PLATFORM == MVM_JIT_PLATFORM_POSIX
static void compile_arglist(MVMThreadContext *tc, MVMJitCompiler *compiler,
                            MVMJitExprTree *tree, MVMint32 node) {
    /* Let's first ensure we have all the necessary values in place */
    MVMint32 i, nchild = tree->nodes[node+1];
    MVMJitExprValue *gpr_values[6], *sse_values[8], *stack_values[16];
    MVMint32 num_gpr = 0, num_sse = 0, num_stack = 0;
    for (i = 0; i < nchild; i++) {
        MVMint32 carg = tree->nodes[node+2+i];
        MVMint32 argtyp = tree->nodes[carg+2];
        MVMJitExprValue *argval = &tree->info[tree->nodes[carg+1]].value;
        if (argtyp == MVM_JIT_NUM) {
            if (num_sse < (sizeof(sse_values)/sizeof(sse_values[0]))) {
                sse_values[num_sse++] = argval;
            } else {
                stack_values[num_stack++] = argval;
            }
        } else {
            if (num_gpr < (sizeof(gpr_values)/sizeof(gpr_values[0]))) {
                gpr_values[num_gpr++] = argval;
            } else {
                stack_values[num_stack++] = argval;
            }
        }
    }
    /* The following is pretty far from optimal. The optimal code
       would determine which values currently reside in registers and
       where they should be placed, and move them there, possibly
       freeing up registers for later access. But that requires some
       pretty complicated logic.  */

    /* Now emit the gpr values */
    for (i = 0; i < num_gpr; i++) {
        MVM_jit_register_put(tc, compiler, gpr_values[i], MVM_JIT_REGCLS_GPR, x64_gpr_args[i]);
        /* Mark GPR as used - nb, that means we need some cleanup logic afterwards */
        MVM_jit_register_use(tc, compiler, MVM_JIT_REGCLS_GPR, x64_gpr_args[i]);
    }

    /* SSE logic is pretty much the same as GPR logic, just with SSE rather than GPR args  */
    for (i = 0; i < num_sse; i++) {
        MVM_jit_register_put(tc, compiler, gpr_values[i], MVM_JIT_REGCLS_NUM, x64_sse_args[i]);
        MVM_jit_register_use(tc, compiler, MVM_JIT_REGCLS_NUM, x64_sse_args[i]);
    }

    /* Stack arguments are simpler than register arguments */
    for (i = 0; i < num_stack; i++) {
        MVMJitExprValue *argval = stack_values[i];
        if (argval->state == MVM_JIT_VALUE_SPILLED) {
            /* Allocate a temporary register, load and place, and free the register */
            MVMint8 reg_num = MVM_jit_register_alloc(tc, compiler, MVM_JIT_REGCLS_GPR);
            /* We do the load directly because, this value being
             * spilled and invalidated just after the call, there is
             * no reason to involve the register allocator */
            MVM_jit_emit_load(tc, compiler, argval->spill_location,
                              MVM_JIT_REGCLS_GPR, reg_num,  argval->size);
            MVM_jit_emit_stack_arg(tc, compiler, i * 8, MVM_JIT_REGCLS_GPR, reg_num, argval->size);
            MVM_jit_register_free(tc, compiler, MVM_JIT_REGCLS_GPR, reg_num);
        } else if (argval->state == MVM_JIT_VALUE_ALLOCATED) {
            /* Emitting a stack argument is not a free */
            MVM_jit_emit_stack_arg(tc, compiler, i * 8, argval->reg_cls,
                                   argval->reg_num, argval->size);
        } else {
            MVM_oops(tc, "ARGLIST: Live Value Inaccessible");
        }
    }

}
#else
static void compile_arglist(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree,
                            MVMint32 node) {
    MVMint32 i, nchild = tree->nodes[node+1], first_child = node+2;
    /* TODO implement this too */
    NYI(compile_arglist_win32);
}
#endif

#else
/* No such architecture */
static void compile_arglist(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree,
                            MVMint32 node) {
    NYI(compile_arglist);
}
#endif


static void pre_call(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    /* Calls invalidate all caller-saved registers. Values that have
       not yet been spilled and are needed after the call will need to
       be spilled */
    MVM_jit_spill_before_call(tc, cl);
}

#if MVM_JIT_ARCH == MVM_JIT_ARCH_X64
static void post_call(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    /* Post-call, we might implement restore, if the call was
       conditional. But that is something for another day */
    MVMint32 i;
    /* ARGLIST locked all registers and we have to wait until after the CALL to unlock them */
    for (i = 0; i < sizeof(x64_gpr_args); i++) {
        MVM_jit_register_release(tc, cl, MVM_JIT_REGCLS_GPR, x64_gpr_args[i]);
    }
    /* TODO same for numargs */
}

#else
static void post_call(MVMThreadContext *tc, MVMJitCompiler *cl, MVMJitExprTree *tree, MVMint32 node) {
    NYI(post_call);
}
#endif

void MVM_jit_compile_breakpoint(void) {
    fprintf(stderr, "Pause here please\n");
}

static void MVM_jit_compile_tile(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree, MVMJitTile *tile) {
    MVMJitExprValue **values = tile->values;
    MVMint32 i;

    /* Increment order nr - must follow the same convention as in tile.c:select_values */
    compiler->order_nr++;

    if (tile->emit == NULL)
        /* Empty tile rule */
        return;

    /* Extract value pointers from the tree */
    ensure_values(tc, compiler, values+1, tile->num_vals);

    if (values[0] != NULL && values[0]->type == MVM_JIT_REG) {
        /* allocate a register for the result */
        if (tile->num_vals > 0 &&
            values[1]->type == MVM_JIT_REG &&
            values[1]->state == MVM_JIT_VALUE_ALLOCATED &&
            values[1]->last_use == compiler->order_nr) {
            /* First register expires immediately, therefore we can safely cross-assign */
            MVM_jit_register_assign(tc, compiler, values[0], values[1]->reg_cls, values[1]->reg_num);
        } else {
            alloc_value(tc, compiler, values[0]);
        }
        use_value(tc, compiler, values[0]);
    }
    /* Emit code */
    tile->emit(tc, compiler, tree, tile->node, tile->values, tile->args);
    /* Release registers from use */
    for (i = 0; i < tile->num_vals + 1; i++) {
        if (values[i] != NULL && values[i]->type == MVM_JIT_REG) {
            release_value(tc, compiler, values[i]);
        }
    }

    /* Expire dead values */
    MVM_jit_expire_values(tc, compiler);
}

static void MVM_jit_compute_use(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitTileList *list) {
    MVMJitTile *tile;
    MVMint32 i;
    for (tile = list->first; tile != NULL; tile = tile->next) {
        if (tile->values[0] == NULL) /* pseudotiles */
            continue;
        tile->values[0]->first_created = tile->order_nr;
        for (i = 0; i < tile->num_vals; i++) {
            tile->values[i+1]->last_use = tile->order_nr;
            tile->values[i+1]->num_use++;
        }
    }
}

/* pseudotile emit functions */
void MVM_jit_compile_branch(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree,
                            MVMint32 node, MVMJitExprValue **value, MVMJitExprNode *args) {
    MVM_jit_emit_branch(tc, compiler, args[0] + compiler->label_offset);
}

void MVM_jit_compile_conditional_branch(MVMThreadContext *tc, MVMJitCompiler *compiler,
                                        MVMJitExprTree *tree, MVMint32 node,
                                        MVMJitExprValue **values, MVMJitExprNode *args) {
    MVM_jit_emit_conditional_branch(tc, compiler, args[0], args[1] + compiler->label_offset);
}

void MVM_jit_compile_label(MVMThreadContext *tc, MVMJitCompiler *compiler,
                           MVMJitExprTree *tree, MVMint32 node,
                           MVMJitExprValue **values, MVMJitExprNode *args) {
    MVM_jit_emit_label(tc, compiler, tree->graph, args[0] + compiler->label_offset);
}


void MVM_jit_compile_expr_tree(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitGraph *jg, MVMJitExprTree *tree) {
    MVMJitRegisterAllocator allocator;
    MVMJitTileList *list;
    MVMJitTile *tile;
    /* First stage, tile the tree */
    list = MVM_jit_tile_expr_tree(tc, tree);
    MVM_jit_compute_use(tc, compiler, list);

    /* Allocate sufficient space for the internal labels */
    dasm_growpc(compiler, compiler->label_offset + tree->num_labels);

    /* Second stage, emit the code - interleaved with the register allocator */

    MVM_jit_register_allocator_init(tc, compiler, &allocator);
    compiler->order_nr = 0;
    for (tile = list->first; tile != NULL; tile = tile->next) {
        MVM_jit_compile_tile(tc, compiler, tree, tile);
    }
    MVM_jit_register_allocator_deinit(tc, compiler, &allocator);

    /* Make sure no other tree reuses the same labels */
    compiler->label_offset += tree->num_labels;
}



/* Enter the JIT code segment. The label is a continuation point where control
 * is resumed after the frame is properly setup. */
void MVM_jit_enter_code(MVMThreadContext *tc, MVMCompUnit *cu,
                        MVMJitCode *code) {
    void *label = tc->cur_frame->jit_entry_label;
    code->func_ptr(tc, cu, label);
}
