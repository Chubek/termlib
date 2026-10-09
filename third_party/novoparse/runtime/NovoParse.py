"""Python support for generated NovoParse parsers (standard library only)."""

import ctypes as C
import ctypes.util
import json
import os
from pathlib import Path


class _Symbol(C.Structure):
    _fields_ = [("kind", C.c_int), ("name", C.c_char_p), ("value", C.c_char_p),
                ("precedence", C.c_int), ("associativity", C.c_int)]


class _Production(C.Structure):
    _fields_ = [("head", C.c_int), ("body", C.POINTER(C.c_int)), ("body_length", C.c_size_t),
                ("label", C.c_char_p), ("precedence", C.c_int), ("associativity", C.c_int)]


class _Grammar(C.Structure):
    _fields_ = [("name", C.c_char_p), ("symbols", C.POINTER(_Symbol)), ("symbol_count", C.c_size_t),
                ("productions", C.POINTER(_Production)), ("production_count", C.c_size_t),
                ("start", C.c_int), ("skip_kind", C.c_int), ("skip", C.c_char_p),
                ("disambiguation", C.c_int), ("rewrites", C.c_char_p)]


class GrammarError(ValueError):
    """The native runtime rejected a grammar or regular expression."""


class ParseError(ValueError):
    def __init__(self, message, status, position, line, column):
        super().__init__(message)
        self.status = status
        self.position = position
        self.line = line
        self.column = column


def _load_library(path):
    if isinstance(path, C.CDLL):
        library = path
    else:
        path = path or os.environ.get("NOVOPARSE_LIBRARY")
        if not path:
            path = ctypes.util.find_library("novoparse")
        if not path:
            root = Path(__file__).resolve().parent.parent
            for name in ("libnovoparse.so", "libnovoparse.dylib", "novoparse.dll"):
                for directory in (root / "build", root / "lib", Path(__file__).parent):
                    candidate = directory / name
                    if candidate.is_file():
                        path = str(candidate)
                        break
                if path:
                    break
        if not path:
            raise OSError("NovoParse runtime not found; set NOVOPARSE_LIBRARY or pass library=PATH")
        library = C.CDLL(os.fspath(path))
    signatures = {
        "np_parser_create": ([C.POINTER(_Grammar), C.c_void_p, C.c_size_t], C.c_void_p),
        "np_parser_destroy": ([C.c_void_p], None),
        "np_parser_parse": ([C.c_void_p, C.c_char_p, C.c_size_t], C.c_int),
        "np_parser_json": ([C.c_void_p], C.c_char_p),
        "np_parser_error": ([C.c_void_p], C.c_char_p),
        "np_parser_position": ([C.c_void_p], C.c_size_t),
        "np_parser_line": ([C.c_void_p], C.c_size_t),
        "np_parser_column": ([C.c_void_p], C.c_size_t),
        "np_parser_ambiguous": ([C.c_void_p], C.c_int),
        "np_parser_conflicts": ([C.c_void_p], C.c_size_t),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(library, name)
        function.argtypes, function.restype = arguments, result
    return library


class Parser:
    """Reusable libglr-backed parser. parse() returns an independent tree dict."""

    def __init__(self, spec, library=None):
        self._parser = None
        self._library = _load_library(library)
        symbols = (_Symbol * len(spec["symbols"]))(*[
            _Symbol(kind, name.encode(), value.encode(), precedence, associativity)
            for kind, name, value, precedence, associativity in spec["symbols"]
        ])
        bodies = [(C.c_int * len(body))(*body) for _, body, _, _, _ in spec["productions"]]
        productions = (_Production * len(bodies))(*[
            _Production(head, bodies[i], len(body), label.encode(), precedence, associativity)
            for i, (head, body, label, precedence, associativity) in enumerate(spec["productions"])
        ])
        grammar = _Grammar(spec["name"].encode(), symbols, len(symbols), productions, len(productions),
                           spec["start"], spec["skip_kind"], spec["skip"].encode(),
                           spec["disambiguation"], spec["rewrites"].encode())
        # The native runtime copies descriptors during creation, but keep the
        # ctypes backing stores alive with the parser as defense in depth.
        self._owned = (symbols, bodies, productions, grammar)
        error = C.create_string_buffer(512)
        self._parser = self._library.np_parser_create(C.byref(grammar), error, len(error))
        if not self._parser:
            raise GrammarError(error.value.decode("utf-8", errors="replace"))

    def _require_open(self):
        if not self._parser:
            raise RuntimeError("parser is closed")

    def close(self):
        if self._parser:
            self._library.np_parser_destroy(self._parser)
            self._parser = None

    def __enter__(self):
        self._require_open()
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            if getattr(self, "_parser", None):
                self.close()
        except Exception:
            pass

    def parse(self, text):
        self._require_open()
        if isinstance(text, str):
            text = text.encode("utf-8")
        if not isinstance(text, bytes):
            raise TypeError("input must be str or bytes")
        status = self._library.np_parser_parse(self._parser, text, len(text))
        if status:
            raise ParseError(self._library.np_parser_error(self._parser).decode("utf-8", errors="replace"), status,
                             self._library.np_parser_position(self._parser),
                             self._library.np_parser_line(self._parser),
                             self._library.np_parser_column(self._parser))
        encoded = self._library.np_parser_json(self._parser)
        if encoded is None:
            raise MemoryError(self._library.np_parser_error(self._parser).decode("utf-8", errors="replace"))
        return json.loads(encoded)

    @property
    def ambiguous(self):
        self._require_open()
        return bool(self._library.np_parser_ambiguous(self._parser))

    @property
    def conflicts(self):
        self._require_open()
        return self._library.np_parser_conflicts(self._parser)
