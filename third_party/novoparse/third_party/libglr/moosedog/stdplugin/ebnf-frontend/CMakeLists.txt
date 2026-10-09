# EBNF frontend plugin: alternate Moosedog syntax.
# Builds the loadable module moosedog_ebnf alongside the core tool.
# The translator itself (EBNF-Frontend.c) is also linked into
# libmoosedog so `moosedog file.ebnf` works without dlopen.
cmake_minimum_required(VERSION 3.16)
