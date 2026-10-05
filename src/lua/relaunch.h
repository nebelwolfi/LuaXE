//
// env.relaunch(args): end this program and start it again as a FRESH process
// with a new argument tail - a new lxe.exe, lua51.dll and module versions are
// picked up, and no thread of the old state outlives it (env.reload re-runs in
// the same process, which keeps both).
//
// Two ways, decided once at start-up (read_handshake):
//   launcher  a launcher started this process (src/relaunch_protocol.h): the
//             tail goes to its file and this process exits with
//             LXE_RELAUNCH_EXIT; the launcher starts the next one in the same
//             console.
//   nested    nobody is listening (`lxe app.lef`, `lxe run main.lua`, a service
//             manager): this process becomes that launcher itself for the rest
//             of its life - it starts the next run as its child, with the same
//             handshake, and waits. Further relaunches go through it, so N
//             restarts leave ONE waiting parent, never a chain.
//
#ifndef LUAXE_RELAUNCH_H
#define LUAXE_RELAUNCH_H

#include <string>
#include <vector>

namespace relaunch {

/// Reads (and removes from this process's environment) the launcher handshake.
/// Call first thing in main(), before any Lua runs.
void read_handshake();

/// "launcher" when a launcher started this process, otherwise "nested".
const char* mode();

/// Records a relaunch request; `args` are the new program arguments (UTF-8).
void request(const std::vector<std::string>& args);
bool requested();

/// Quotes one argument the way CommandLineToArgvW / the CRT split it back.
std::wstring quote(const std::wstring& arg);

/// Performs a recorded request after the state has closed, and never returns.
/// `prefix` is what goes between the executable and the program arguments:
/// the quoted .lef or script path, or empty for a compiled payload.
[[noreturn]] void perform(const std::wstring& prefix);

} // namespace relaunch

#endif // LUAXE_RELAUNCH_H
