"""Reproduce the scannerless IDENT terminal without PCRE lookahead.

Print a POSIX ERE matching identifiers except exact reserved words. --check
verifies that the committed grammar matches; generation is not a build step.
"""
import pathlib
import re
import string
import sys

KEYWORDS = "or do then end else while if const config lifetime not and let var fn return use".split()
CHARS = string.ascii_letters + string.digits + "_"
INITIAL = string.ascii_letters + "_"


def character_class(chars):
    numbers = sorted(map(ord, chars))
    out, i = "", 0
    while i < len(numbers):
        j = i
        while j + 1 < len(numbers) and numbers[j + 1] == numbers[j] + 1:
            j += 1
        if j - i >= 2:
            out += chr(numbers[i]) + "-" + chr(numbers[j])
        else:
            out += "".join(map(chr, numbers[i:j + 1]))
        i = j + 1
    return "[" + out + "]"


def pattern(node, prefix=""):
    choices = [""] if prefix and "" not in node else []
    others = [c for c in (CHARS if prefix else INITIAL) if c not in node]
    if others:
        choices.append(character_class(others) + "[A-Za-z0-9_]*")
    for char, child in node.items():
        if char:
            choices.append(char + pattern(child, prefix + char))
    return choices[0] if len(choices) == 1 else "(" + "|".join(choices) + ")"


def generate():
    trie = {}
    for word in KEYWORDS:
        node = trie
        for char in word:
            node = node.setdefault(char, {})
        node[""] = True
    return pattern(trie)


if __name__ == "__main__":
    result = generate()
    for word in KEYWORDS:
        assert re.fullmatch(result, word) is None
        assert re.fullmatch(result, word + "_suffix") is not None
    if "--check" in sys.argv:
        grammar = pathlib.Path(__file__).resolve().parents[1] / "Termscript.ng"
        assert "%token IDENT /" + result + "/;" in grammar.read_text()
    else:
        print(result)
