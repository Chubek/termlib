" moosedog.vim -- Vim syntax for Moosedog .grm specifications.
if exists("b:current_syntax") | finish | endif
syn keyword moosedogBlock Entrypoint AST Lexical Syntactic
syn keyword moosedogSection LanguageInfo GrammarInfo Config Optparse ATN
syn keyword moosedogRule Rule Alt Node Field Skip Token Option
syn region moosedogString start=+"+ skip=+\\\\\|\\"+ end=+"+
syn region moosedogComment start="/\*" end="\*/"
hi def link moosedogBlock Structure
hi def link moosedogSection Identifier
hi def link moosedogRule Statement
hi def link moosedogString String
hi def link moosedogComment Comment
let b:current_syntax = "moosedog"
