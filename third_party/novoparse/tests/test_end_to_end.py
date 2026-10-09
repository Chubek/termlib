import argparse
import importlib.util
import itertools
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "novoparse"
LIBRARY = ROOT / "build" / "libnovoparse.so"
WORKDIR = Path("/tmp/opencode") if Path("/tmp/opencode").is_dir() else None
sys.path.insert(0, str(ROOT / "runtime"))
from NovoParse import GrammarError, ParseError, Parser


class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="novoparse-tests-", dir=WORKDIR)
        cls.directory = Path(cls.temporary.name)
        cls.counter = itertools.count()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def command(self, *arguments, success=True):
        result = subprocess.run([str(CLI), *map(str, arguments)], capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def generate(self, source=None, example=None):
        identifier = next(self.counter)
        path = self.directory / f"grammar_{identifier}.ng"
        path.write_text(source if source is not None else (ROOT / "examples" / example).read_text())
        output = self.directory / f"generated_{identifier}.py"
        self.command("generate", path, "--profile", "Python", "-o", output)
        module_spec = importlib.util.spec_from_file_location(f"generated_{identifier}", output)
        module = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(module)
        return module

    def parser(self, source):
        return Parser(self.generate(source).SPEC, library=LIBRARY)

    def test_json_and_input_lifetime(self):
        module = self.generate(example="JSON.ng")
        texts = ['{}', '[]', 'true', 'null', '-0.25e+2', '"café 😀"',
                 '{"escaped":"a\\n\\\"b", "nested": [false, null, {"x":42}]}',
                 '\n\t { "a" : [1, 2, 3] } \r\n']
        with module.JSONParser(library=LIBRARY) as parser:
            for text in texts:
                json.loads(text)
                tree = parser.parse(text)
                self.assertFalse(parser.ambiguous)
                self.assertIn("name", tree)
            saved = parser.parse('"first"')
            parser.parse('"second"')
            self.assertEqual(saved["children"][0]["text"], '"first"')
            for text in ['[1,]', '{"x":}', '01', '+1', '1.', '"\\q"', '"raw\nnewline"', 'true false']:
                with self.assertRaises(ParseError, msg=text):
                    parser.parse(text)
            self.assertEqual(parser.parse('42')["name"], "Number")
        with self.assertRaisesRegex(RuntimeError, "closed"):
            parser.parse("0")

    def test_precedence_and_associativity(self):
        module = self.generate(example="Arithmetic.ng")
        with module.ArithmeticParser(library=LIBRARY) as parser:
            tree = parser.parse("1+2*3")
            self.assertEqual(tree["name"], "Add")
            self.assertEqual(tree["children"][2]["name"], "Multiply")
            self.assertEqual(parser.parse("1-2-3")["children"][0]["name"], "Subtract")
            self.assertEqual(parser.parse("2^3^4")["children"][2]["name"], "Power")
            self.assertEqual(parser.parse("1/2")["name"], "Divide")
            self.assertEqual(parser.conflicts, 0)
            self.assertFalse(parser.ambiguous)

    def test_ambiguity_and_lexical_alternatives(self):
        source = '%grammar Test; E: E "+" E => Add | "n" => Name;'
        with self.parser(source) as parser:
            with self.assertRaises(ParseError) as raised:
                parser.parse("n+n+n")
            self.assertEqual(raised.exception.status, 4)
            self.assertTrue(parser.ambiguous)
            self.assertEqual(parser.parse("n")["name"], "Name")
        with self.parser("%disambiguate first; " + source) as parser:
            tree = parser.parse("n+n+n")
            self.assertTrue(parser.ambiguous)
            self.assertEqual(tree["children"][0]["name"], "Add")
            self.assertEqual(parser.parse("n+n+n"), tree)
        with self.parser('%token A /ab|a/; S: A "b";') as parser:
            self.assertEqual(parser.parse("ab")["children"][0]["text"], "a")
            self.assertEqual(parser.parse("abb")["children"][0]["text"], "ab")
        with self.parser('%token WORD /[a-z]+/; S: WORD WORD;') as parser:
            with self.assertRaises(ParseError):
                parser.parse("abc")

    def test_nonassoc_and_precedence_override(self):
        with self.parser('%nonassoc "<"; E: E "<" E | "n";') as parser:
            parser.parse("n<n")
            with self.assertRaises(ParseError):
                parser.parse("n<n<n")
        source = '%left "+"; %right "-"; E: E "+" E => Add | "-" E %prec "-" => Negate | "n" => Name;'
        with self.parser(source) as parser:
            self.assertEqual(parser.parse("-n+n")["children"][0]["name"], "Negate")

    def test_ebnf_skips_and_empty_input(self):
        with self.parser('%skip /[ \t\r\n]+/; S: "a" ("," "a")* "!"?;') as parser:
            tree = parser.parse(" a, a ,a! \n")
            self.assertEqual([child["text"] for child in tree["children"]], ["a", ",", "a", ",", "a", "!"])
            self.assertEqual(tree["start"], 1)
            self.assertEqual(tree["end"], 9)
        with self.parser('%skip " "; S: "a"+;') as parser:
            self.assertEqual(len(parser.parse("   a  a   ")["children"]), 2)
        with self.parser('%skip /[ \t]+/; S: %empty => Empty;') as parser:
            self.assertEqual(parser.parse("")["name"], "Empty")
            self.assertEqual(parser.parse(" \t")["children"], [])

    def test_rewriters(self):
        with self.parser('%rewrite remove-duplicate-productions; S: "x" | "x";') as parser:
            parser.parse("x")
            self.assertFalse(parser.ambiguous)
        with self.parser('%rewrite remove-unit-productions; S: A; A: "x";') as parser:
            self.assertEqual(parser.parse("x")["children"][0]["text"], "x")
        with self.parser('%rewrite eliminate-unused-terminals; %token UNUSED "unused"; S: "x" => Kept;') as parser:
            self.assertEqual(parser.parse("x")["name"], "Kept")

    def test_invalid_patterns(self):
        for source in ['%token X /(/; S: X;', '%token X /a*/; S: X;', '%skip /a*/; S: "x";']:
            with self.assertRaises(GrammarError, msg=source):
                self.parser(source)

    def test_ir_and_custom_profile(self):
        grammar = ROOT / "examples" / "JSON.ng"
        intermediate = self.directory / "JSON.sexp"
        self.command("compile", grammar, "-o", intermediate)
        direct = self.command("generate", grammar, "--profile", "Python").stdout
        from_ir = self.command("generate", intermediate, "--profile", "Python").stdout
        self.assertEqual(direct, from_ir)
        self.assertEqual(self.command("compile", intermediate).stdout, intermediate.read_text())
        custom = self.command("generate", grammar, "--profile", ROOT / "profiles" / "C.sexp").stdout
        self.assertEqual(custom, self.command("generate", grammar, "--profile", "C").stdout)
        result = self.command("check", intermediate)
        self.assertIn("valid", result.stdout)
        intermediate.write_text(intermediate.read_text().replace("(start 0)", "(start 99999)"))
        self.assertIn("start symbol", self.command("generate", intermediate, "--profile", "C", success=False).stderr)

    def test_cli_diagnostics(self):
        path = self.directory / "bad.ng"
        path.write_text('S: missing;')
        result = self.command("check", path, success=False)
        self.assertIn("bad.ng:1:4: undefined symbol", result.stderr)
        self.assertNotIn("core.exception", result.stderr)
        self.command("generate", path, success=False)
        self.command("--unknown", success=False)
        self.assertIn("Python", self.command("--list-profiles").stdout)


if __name__ == "__main__":
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--cli", type=Path, default=CLI)
    arguments.add_argument("--library", type=Path, default=LIBRARY)
    arguments.add_argument("--workdir", type=Path, default=WORKDIR)
    config = arguments.parse_args()
    CLI, LIBRARY, WORKDIR = config.cli.resolve(), config.library.resolve(), config.workdir
    unittest.main(argv=[sys.argv[0]], verbosity=2)
