// lmok serve / lmok remote: a small REST front end for the lmok CLI.
//
// `lmok serve` keeps one process alive and runs lmok commands in-process on
// behalf of HTTP clients. It owns settings (chosen from presets at start and
// editable over REST), a result history, and a content-addressed result cache
// whose keys include the bytes of every input file, so a cached answer is only
// reused while its inputs are unchanged. It stops on POST /v1/shutdown or after
// the configured idle timeout.
//
// `lmok remote` is the matching command-line client. It forwards one lmok
// command to a server and replays the captured stdout, stderr and exit code, so
// it can stand in for the host binary (for example as run_lmcode's `command`).
#pragma once

#include <string>
#include <vector>

namespace lmok {

// The in-process CLI entry point the server dispatches to (main.cpp's run_cli).
using CliEntry = int (*)(int argc, char **argv);

int serve_main(const std::vector<std::string> &args, CliEntry cli);
int remote_main(const std::vector<std::string> &args);

} // namespace lmok
