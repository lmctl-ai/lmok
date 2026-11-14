# C++ grammar and tier-1 rules

`lmok` parses C++ with the vendored `tree-sitter-cpp` grammar (`v0.23.4`, ABI 14, external scanner in `src/scanner.c`, MIT license, archive hash in `deps.lock`).

## Scope and extensions

- **Canonical language ID:** `"cpp"` (strict; `"c++"` is rejected).
- **Unambiguous extensions:** `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hh`, `.hxx`.
- **Header (`.h`) policy:** `.h` defaults to `c` everywhere so C codebases and fixtures remain unchanged. Pass `--header-lang cpp` (or `"header_lang": "cpp"` in `.lmok/config.json`) to interpret `.h` headers as `cpp`; `index` invalidates cached extractions whenever resolved `lang` changes, and explicit single-file `lint`/`test` against a C++-only pack routes `.h` to `cpp`.
- **Typed `lmcode` host surface:** `ts.parse_cpp`, `ts.query_cpp`, and `ts.reparse_cpp` mirror `ts.*_java`. Code-shaped patterns (`--pattern` / `ts.match_cpp`), `rules/secrets-cpp.json`, `rules/cpp-autofix.json`, and `rules/lmcode/cpp-*.lms` remain Phase 2 items.

## Facts and metrics

- `def.namespace` is an additive public definition kind and opens lexical scopes (`namespace a::b` normalizes `"::"` to `"."` in nested scopes while keeping `"a::b"` as its own name; anonymous namespaces emit no fact).
- Unqualified in-class definitions (outside `friend_declaration`) promote from `def.function` to `def.method` (`exact`). Out-of-line qualified definitions (`Foo::bar`) emit `def.method` with `Foo` folded into `scope` (`exact` for destructors, conversion/overloaded operators, constructors matching the qualifier, member initializer lists, or trailing `const`/`ref`/`virtual` qualifiers; otherwise `candidate`).
- Named casts (`static_cast`, `dynamic_cast`, `const_cast`, `reinterpret_cast`) parse as `call_expression` over `template_function` in `tree-sitter-cpp` and are explicitly excluded from `ref.call`.
- `for_range_loop` and `nullptr` are handled in `cpp/metrics.cpp`.

## Tier-1 pack (`CPP001`–`CPP009`) and known parser limits

- `rules/cpp-tier1.json` provides nine syntactic checks: unbounded C buffer calls (`CPP001`), process execution (`CPP002`), empty `catch` blocks (`CPP003`), file/header-scope `using namespace` (`CPP004`), `reinterpret_cast` / `const_cast` (`CPP005`), assignment inside `if`/`while` conditions (`CPP006`), `NULL` macro (`CPP007`), `delete` / `delete[]` (`CPP008`), and value `#define` macros (`CPP009`).
- `tree-sitter-cpp` `v0.23.4`'s scanner checks `delimiter_length >= 16` before seeing `(`, so ISO-legal 16-character raw-string delimiters emit local `ERROR` recovery nodes without out-of-bounds memory writes (`tests/fixtures/cpp-known-limits.cpp`). `0`–`15` character delimiters parse and incrementally reparse cleanly across all five CI platforms (`tests/check-cpp-os.py`).
