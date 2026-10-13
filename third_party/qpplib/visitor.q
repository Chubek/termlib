# visitor.q — iterative graph DFS with enter/leave callbacks and cycle suppression

@module visitor
@version 1.0

@param PREFIX visitor
@param T
@param EQ
@param CAP 256

@require {$CAP > 0} "CAP must be positive"

@include <stddef.h>

@guard

@raw {
    typedef size_t (*${PREFIX}_count_fn)(${T} node, void *ctx);
    typedef ${T} (*${PREFIX}_child_fn)(${T} node, size_t index, void *ctx);
    typedef int (*${PREFIX}_visit_fn)(${T} node, int leaving, void *ctx);
    typedef struct { ${T} node; size_t next, count; } ${PREFIX}_frame;
    /* Returns 1 on completion, 0 on callback stop, -1 on capacity/argument error.
     * Visit returns >0 to continue, 0 to stop, <0 to prune (enter only).
     * EQ(a,b) defines node identity. Shared nodes are visited only once. */
}

@fn walk {
    static inline int ${PREFIX}_walk(${T} root, ${PREFIX}_count_fn count, ${PREFIX}_child_fn child, ${PREFIX}_visit_fn visit, void *ctx) {
        ${PREFIX}_frame stack[${CAP}]; ${T} seen[${CAP}]; size_t depth = 0, used = 0, i;
        ${T} node = root;
        if (!count || !child || !visit) return -1;
        for (;;) {
            int known = 0;
            for (i = 0; i < used; ++i) if (${EQ}(seen[i], node)) { known = 1; break; }
            if (!known) {
                int action;
                if (used == ${CAP} || depth == ${CAP}) return -1;
                seen[used++] = node; action = visit(node, 0, ctx);
                if (!action) return 0;
                stack[depth].node = node; stack[depth].next = 0;
                stack[depth++].count = action < 0 ? 0 : count(node, ctx);
            }
            while (depth && stack[depth-1].next == stack[depth-1].count) {
                if (!visit(stack[depth-1].node, 1, ctx)) return 0;
                --depth;
            }
            if (!depth) return 1;
            node = child(stack[depth-1].node, stack[depth-1].next++, ctx);
        }
    }
}
