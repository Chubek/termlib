set(GLR_REWRITE_LIBRARY_FILES
    eiminate-ambguity.grl
    eliminate-epsilon-production.grl
    eliminate-useless-symbols.grl
    left-factor.grl
    make-lr-compat.grl
    remove-left-recursion.grl
    remove-unit-productions.grl
    remove-duplicate-productions.grl
    remove-self-unit-productions.grl
    eliminate-unreachable-symbols.grl
    eliminate-unproductive-symbols.grl
    eliminate-unused-terminals.grl
    augment-start-symbol.grl
    isolate-terminals.grl
    left-binarize.grl
    right-binarize.grl
    chomsky-normal-form.grl
    right-factor.grl
    remove-right-recursion.grl
    reverse-productions.grl
    inline-single-production-nonterminals.grl
    merge-equivalent-nonterminals.grl
)

install(FILES ${GLR_REWRITE_LIBRARY_FILES}
        DESTINATION share/libglr/rewritelib)
