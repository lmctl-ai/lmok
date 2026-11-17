# Standard `lmcode` REST Endpoints & Stateless CLI Interfaces

## Summary

`lmok` upgrades its vendored `lmcode-cpp` SDK (`cpp/dslvm.hpp`, `cpp/stdlib/rest.hpp`, `cpp/stdlib/std.hpp`, `cpp/std/lmcode-std.schema.json`, `sdk.lock`) to the 226-signature `lmcode_std_v3` standard catalog and `@lmctl-ai/lmctl@0.2.482` (`compiler.lock`):
- `schema/lmok.dsl.json` merges the 10 standard REST/CLI interfaces (`RestGet`..`RestOptions`, `RestHandler`, `CliCommand`, `CliOption`) and `RestExchange` methods via `scripts/merge-std.py`.
- Verified in `test.sh` with `std-rest-cli.lms`, exercising `CliOption`, `CliCommand`, `RestGet`, and `rest_cli` driving `ts.parse_c(...)` and `ts.query_c(...)`.
