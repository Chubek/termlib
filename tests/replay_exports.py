"""Execute generated replay sources; display/GPU APIs use isolated fixtures."""
import pathlib
import runpy
import subprocess
import sys
import types

generator, node, assembler, driver, work, native, jit = sys.argv[1:]
work = pathlib.Path(work)
work.mkdir(parents=True, exist_ok=True)


def run(*args):
    result = subprocess.run(args, capture_output=True, timeout=60)
    assert result.returncode == 0, (args, result.stderr.decode(errors="replace"))
    return result.stdout


run(generator, "--emit", str(work))
expected = (work / "expected.bin").read_bytes()
for prefix in ("", "empty-"):
    run(assembler, str(work / (prefix + "replay.wat")), "-o", str(work / (prefix + "replay.wasm")))
run(node, str(pathlib.Path(__file__).with_suffix(".js")), str(work))
assert run(node, str(work / "replay.js")) == expected
assert run(node, str(work / "empty-replay.js")) == b""

# Verify the emitted Manim program's event handling without requiring a display
# or claiming a real Manim render was tested on this host.
manim = types.ModuleType("manim")


class Text:
    def __init__(self, text):
        self.text = text


class Scene:
    def __init__(self):
        self.objects, self.delays = [], []

    def add(self, obj):
        self.objects.append(obj)

    def remove(self, obj):
        self.objects.remove(obj)

    def wait(self, duration):
        assert duration > 0
        self.delays.append(duration)


manim.Text, manim.Scene = Text, Scene
sys.modules["manim"] = manim
for prefix in ("", "empty-"):
    module = runpy.run_path(str(work / (prefix + "replay.py")))
    scene = module["Replay"]()
    scene.construct()
    if not prefix:
        events = module["events"]
        assert events[0][0] == 18446744073700000000
        assert b"".join(e[4] for e in events if e[1] == 2) == expected
        assert abs(sum(scene.delays) - 0.004) < 1e-9
        assert len(scene.objects) == 1
        assert scene.objects[0].text.endswith("€\ufffd\ufffd")
    else:
        assert not scene.objects

# Termscript export is deliberately an unpaced output-only byte stream, tested
# through every enabled backend; it must neither echo input nor add newlines.
for prefix in ("", "empty-"):
    source = str(work / (prefix + "replay.tsc"))
    wanted = b"" if prefix else expected
    assert run(driver, source) == wanted
    if jit == "ON":
        assert run(driver, "--backend", "jiterati", source) == wanted
    executable = str(work / (prefix + "replay-c"))
    run(driver, "-o", executable, source)
    assert run(executable) == wanted
    if native == "ON":
        executable = str(work / (prefix + "replay-qbe"))
        run(driver, "--backend", "qbe", "-o", executable, source)
        assert run(executable) == wanted

# Binary output validates before writing anything, including late invalid hex.
for text in ('"0"', '"00zz"', '123'):
    source = work / "invalid-stdio.tsc"
    source.write_text(f'const S = G:load("std.stdio"); S:write_hex({text});\n')
    result = subprocess.run([driver, str(source)], capture_output=True, timeout=10)
    assert result.returncode != 0 and not result.stdout
print("replay exporters OK")
