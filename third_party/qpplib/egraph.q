# egraph.q — bounded term e-graph with union-find and congruence closure

@module egraph
@version 1.0

@param PREFIX egraph
@param OP int
@param EQ
@param CAP 256
@param ARITY 4

@require {$CAP > 0 && $ARITY > 0} "CAP and ARITY must be positive"

@include <stddef.h>
@include <stdint.h>

@guard

@raw {
    typedef struct { ${OP} op; size_t arity, children[${ARITY}], owner; } ${PREFIX}_node;
    typedef struct { ${PREFIX}_node nodes[${CAP}]; size_t parent[${CAP}], size; } ${PREFIX}_t;
}

@fn init {
    static inline void ${PREFIX}_init(${PREFIX}_t *g) { g->size=0; }
}

@fn find {
    static inline size_t ${PREFIX}_find(const ${PREFIX}_t *g, size_t id) {
        if (id>=g->size) return SIZE_MAX;
        while (g->parent[id]!=id) id=g->parent[id];
        return id;
    }
}

@fn union {
    static inline int ${PREFIX}_union(${PREFIX}_t *g, size_t a, size_t b) {
        a=${PREFIX}_find(g,a); b=${PREFIX}_find(g,b);
        if (a==SIZE_MAX || b==SIZE_MAX) return -1;
        if (a==b) return 0;
        if (a<b) g->parent[b]=a; else g->parent[a]=b;
        return 1;
    }
}

@fn congruent {
    static inline int ${PREFIX}_congruent(const ${PREFIX}_t *g, const ${PREFIX}_node *a, const ${PREFIX}_node *b) {
        size_t k;
        if (!${EQ}(a->op,b->op) || a->arity!=b->arity) return 0;
        for (k=0;k<a->arity;++k) if (${PREFIX}_find(g,a->children[k])!=${PREFIX}_find(g,b->children[k])) return 0;
        return 1;
    }
}

@fn rebuild {
    static inline void ${PREFIX}_rebuild(${PREFIX}_t *g) {
        size_t i,j; int changed;
        do {
            changed=0;
            for (i=0;i<g->size;++i) for (j=0;j<i;++j)
                if (${PREFIX}_congruent(g,&g->nodes[i],&g->nodes[j]) && ${PREFIX}_union(g,i,j)>0) changed=1;
        } while (changed);
        for (i=0;i<g->size;++i) { g->parent[i]=${PREFIX}_find(g,i); g->nodes[i].owner=g->parent[i]; }
    }
}

@fn add {
    static inline size_t ${PREFIX}_add(${PREFIX}_t *g, ${OP} op, const size_t *children, size_t arity) {
        ${PREFIX}_node node; size_t i,id;
        if (arity>${ARITY} || (!children && arity)) return SIZE_MAX;
        node.op=op; node.arity=arity; node.owner=0;
        for (i=0;i<arity;++i) {
            node.children[i]=${PREFIX}_find(g,children[i]); if (node.children[i]==SIZE_MAX) return SIZE_MAX;
        }
        for (;i<${ARITY};++i) node.children[i]=0;
        for (i=0;i<g->size;++i) if (${PREFIX}_congruent(g,&node,&g->nodes[i])) return ${PREFIX}_find(g,i);
        if (g->size==${CAP}) return SIZE_MAX;
        id=g->size++; node.owner=id; g->nodes[id]=node; g->parent[id]=id; return id;
    }
}

@fn merge {
    static inline int ${PREFIX}_merge(${PREFIX}_t *g, size_t a, size_t b) {
        int result=${PREFIX}_union(g,a,b);
        if (result>0) ${PREFIX}_rebuild(g);
        return result;
    }
}

@fn equal {
    static inline int ${PREFIX}_equal(const ${PREFIX}_t *g, size_t a, size_t b) {
        size_t x=${PREFIX}_find(g,a), y=${PREFIX}_find(g,b); return x!=SIZE_MAX && x==y;
    }
    /* Positive per-node costs ensure cyclic classes without finite terms cannot
     * win. best_node contains SIZE_MAX for classes with no finite extraction. */
}

@fn extract {
    static inline int ${PREFIX}_extract(const ${PREFIX}_t *g, size_t root, size_t (*cost)(${OP}, void *), void *ctx, size_t *best_node, size_t *total) {
        size_t costs[${CAP}],best[${CAP}],i,k,pass,r=${PREFIX}_find(g,root);
        if (r==SIZE_MAX || !cost || !best_node || !total) return 0;
        for (i=0;i<g->size;++i) { costs[i]=SIZE_MAX; best[i]=SIZE_MAX; }
        for (pass=0;pass<g->size;++pass) {
            int changed=0;
            for (i=0;i<g->size;++i) {
                const ${PREFIX}_node *n=&g->nodes[i]; size_t c=cost(n->op,ctx), owner=${PREFIX}_find(g,i);
                if (!c || c==SIZE_MAX) continue;
                for (k=0;k<n->arity;++k) {
                    size_t child=costs[${PREFIX}_find(g,n->children[k])];
                    if (child==SIZE_MAX || c>=SIZE_MAX-child) { c=SIZE_MAX; break; }
                    c+=child;
                }
                if (c<costs[owner]) { costs[owner]=c; best[owner]=i; changed=1; }
            }
            if (!changed) break;
        }
        if (costs[r]==SIZE_MAX) return 0;
        for (i=0;i<g->size;++i) best_node[i]=best[${PREFIX}_find(g,i)];
        *total=costs[r]; return 1;
    }
}
