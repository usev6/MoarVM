#include "moar.h"
#include "dasm_proto.h"
#include "x64_tile_decl.h"
#include "x64_tile_tables.h"

static MVMint32 assign_tile(MVMThreadContext *tc, MVMJitExprTree *tree,
                            MVMJitExprNode node, const MVMJitTile *tile) {
    if (tree->info[node].tile == NULL || tree->info[node].tile == tile ||
        memcmp(tile, tree->info[node].tile, sizeof(MVMJitTile)) == 0) {
        /* happy case, no conflict */
        tree->info[node].tile = tile;
        return node;
    } else {
        /* resolve conflict by copying this node */
        const MVMJitExprOpInfo *info = tree->info[node].op_info;
        MVMint32 space = (info->nchild < 0 ?
                          2 + tree->nodes[node+1] + info->nargs :
                          1 + info->nchild + info->nargs);
        MVMint32 num   = tree->nodes_num;
        /* Internal copy, so ensure space is available (no realloc may
           happen during append), so we don't try to read from memory
           freed in the realloc */
        MVM_DYNAR_ENSURE_SPACE(tree->nodes, space);
        MVM_DYNAR_APPEND(tree->nodes, tree->nodes + node, space);
        /* Copy the information node */
        MVM_DYNAR_ENSURE_SIZE(tree->info, num);
        memcpy(tree->info + num, tree->info + node, sizeof(MVMJitExprOpInfo));
        /* TODO - I think we potentially should change some fields
           (e.g. num_uses, last_use), have to figure out which */
        /* Assign the new tile */
        tree->info[num].tile = tile;
        /* Return reference to new node */
        return num;
    }
}

static void tile_node(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                      MVMJitExprTree *tree, MVMint32 node) {
    MVMJitExprNode op            = tree->nodes[node];
    MVMJitExprNodeInfo *symbol   = &tree->info[node];
    const MVMJitExprOpInfo *info = symbol->op_info;
    MVMint32 first_child = node+1;
    MVMint32 nchild      = info->nchild < 0 ? tree->nodes[first_child++] : info->nchild;
    MVMint32 state_idx, i;
    MVMint32 *state_info;
    if (traverser->visits[node] > 1)
        return;
    switch (op) {
        /* TODO implement case for variadic nodes (DO/ALL/ANY/ARGLIST)
           and IF, which has 3 children */
    case MVM_JIT_DO:
    case MVM_JIT_ALL:
    case MVM_JIT_ANY:
    case MVM_JIT_ARGLIST:
    case MVM_JIT_IF:
        MVM_oops(tc, "Tiling %s NYI\n", info->name);
        break;
    default:
        {
            if (nchild == 0) {
                state_idx = MVM_jit_tile_states_lookup(tc, op, -1, -1);
            } else if (nchild == 1) {
                MVMint32 left = tree->nodes[first_child];
                MVMint32 lstate = tree->info[left].tile_state;
                state_idx = MVM_jit_tile_states_lookup(tc, op, lstate, -1);
            } else if (nchild == 2) {
                MVMint32 left  = tree->nodes[first_child];
                MVMint32 lstate = tree->info[left].tile_state;
                MVMint32 right = tree->nodes[first_child+1];
                MVMint32 rstate = tree->info[left].tile_state;
                state_idx = MVM_jit_tile_states_lookup(tc, op, lstate, rstate);
            } else {
                MVM_oops(tc, "Can't deal with %d children of node %s\n", nchild, info->name);
            }
            if (state_idx < 0)
                MVM_oops(tc, "Tiler table could not find next state for %s\n",
                         info->name);
            state_info         = MVM_jit_tile_states[state_idx];
            symbol->tile_idx   = state_idx;
            symbol->tile_state = state_info[3];
            for (i = 0; i < MIN(2,nchild); i++) {
                /* Assign tiles to child */
                MVMint32 child         = tree->nodes[first_child+i];
                const MVMJitTile *tile = &MVM_jit_tile_table[state_info[5+i]];
                MVMint32 replaced      = assign_tile(tc, tree, child, tile);
                tree->nodes[first_child+i] = replaced;
            }
            break;
        }
    }
}


void MVM_jit_tile_expr_tree(MVMThreadContext *tc, MVMJitExprTree *tree) {
    MVMJitTreeTraverser traverser;
    MVMint32 i;
    traverser.inorder = NULL;
    traverser.preorder = NULL;
    traverser.postorder = &tile_node;
    traverser.data = NULL;
    MVM_jit_expr_tree_traverse(tc, tree, &traverser);
    /* Add tiles for all roots, if none have been computed so far */
    for (i = 0; i < tree->roots_num; i++) {
        MVMint32 node = tree->roots[i];
        if (tree->info[node].tile == NULL) {
            MVMint32 tile_nr  = MVM_jit_tile_states[tree->info[node].tile_idx][4];
            /* no replacements are possible, because this tile is NULL */
            assign_tile(tc, tree, node, &MVM_jit_tile_table[tile_nr]);
        }
    }

}

#define FIRST_CHILD(t,x) (t->info[x].op_info->nchild < 0 ? x + 2 : x + 1)
/* Get input for a tile rule, write into values */
void MVM_jit_tile_get_values(MVMThreadContext *tc, MVMJitExprTree *tree,
                             MVMint32 node, const MVMint8 *path,
                             MVMJitExprValue **values) {
    while (*path > 0) {
        MVMint32 cur_node = node;
        do {
            MVMint32 first_child = FIRST_CHILD(tree, cur_node) - 1;
            cur_node = tree->nodes[first_child+*path++];
        } while (*path > 0);
        *values++ = &tree->info[cur_node].value;
        path++;
    }
}

#undef FIRST_CHILD
