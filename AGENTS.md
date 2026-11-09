# Agent guide

link-c implements rewrite-bridge's passthrough link protocol for C and C++ game-plugin developers.
The single header supplies both host and guest roles for protocol v1, contract revision 7.

## Toolchain and CI

- The header and Unix Makefile use C99; the C++ check uses C++11.
- CMake 3.10+ configures its test build as C11/C++11; Windows CI uses MSVC x64.
- Native macOS builds require macOS 11.0+; set `MACOSX_DEPLOYMENT_TARGET=11.0`.
- Parent CMake projects must set their macOS deployment target before the first `project()` call.
- Linux/macOS CI commands:

```sh
make
make test
make asan
make cxx
RBL_DEMO=build/demo tests/interop.sh c
```

Windows CI uses:

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Gate/workflow regression tests in CI:

```sh
python3 -m venv .workflow-tests
.workflow-tests/bin/python -m pip install -r tools/requirements-test.txt
.workflow-tests/bin/python -m unittest discover -s tools -p 'test_*.py'
```

## Code map

- `rewrite_link.h`: public API and implementation, including wire, control, sessions and pixel I/O.
- `examples/`: demo plus host/guest integration examples and their game stub.
- `tests/`: unit, C++, pixel-safety and interop programs; `tests/vectors/link/` contains shared vectors.
- `PIXEL-IO.md`: presentation and reader lifetime rules.
- `tools/`: public-tree and workflow regression checks.

## Project rules

- Define `REWRITE_LINK_IMPLEMENTATION` in exactly one C or C++ translation unit.
- Follow `docs/CONTRACT.md` in rewrite-bridge; the other language implementations share its vectors.
- Bind and connect only to `127.0.0.1`; the library creates no threads or global state.
- Present frames with the ordered reader and drop old pixel-channel state when the session changes.
- Keep the vector files and their pinned digest synchronized as described in the README.
- Tests generate protocol traffic and pixel files; they need no game installation.

## Public files

- Never commit game files, extracted assets, renders, decompiled code or secrets.
- The public-tree gate needs Python 3.9+ and Git; run it from the repository root:
  `python3 tools/check_public_tree.py` (checks Git's index, including staged new files).
- CI checks committed changes with `python3 tools/check_public_tree.py --range BASE..HEAD`.

## Contributing, license and AI

- Open an issue to discuss a problem or send a pull request with the change.
- Before a pull request, run the applicable CI commands above and the public-tree gate.
- License: MIT OR Apache-2.0, at your option; see `LICENSE-MIT` and `LICENSE-APACHE`.

The code was written with AI coding agents (Anthropic's Claude and OpenAI's GPT models) under the maintainer's direction.
AI-assisted contributions are welcome when their use is disclosed and the changes are reviewed.
