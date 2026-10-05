/*
 * The relaunch handshake between a launcher and the lxe it starts (plain C, so
 * launcher/launcher.c and src/lua/relaunch.cpp share it).
 *
 * A launcher (the few-KB <name>.exe in front of a .lef, or an lxe in "nested"
 * mode, see relaunch.h) starts its child with two environment variables:
 *
 *     LXE_RELAUNCH_FILE   a fresh path the child may write
 *     LXE_RELAUNCH_NONCE  a fresh token naming this one child
 *
 * The child reads both before any Lua runs and REMOVES them from its own
 * environment, so nothing it starts inherits them (a nested girl started from
 * a girl's shell must not be able to talk to the outer launcher). To ask for a
 * relaunch the child writes, UTF-8:
 *
 *     <nonce>\n<command-line tail>
 *
 * to that file and exits with LXE_RELAUNCH_EXIT. The launcher relaunches only
 * when all three hold - that exit code, the file exists, and it starts with the
 * nonce it gave THAT child; anything else is an ordinary exit and its code is
 * passed through unchanged.
 */
#ifndef LUAXE_RELAUNCH_PROTOCOL_H
#define LUAXE_RELAUNCH_PROTOCOL_H

#define LXE_RELAUNCH_EXIT 0x4C58u /* "LX" */
#define LXE_RELAUNCH_FILE_VAR L"LXE_RELAUNCH_FILE"
#define LXE_RELAUNCH_NONCE_VAR L"LXE_RELAUNCH_NONCE"
/* A tail longer than this is refused (the command line itself is capped at 32K). */
#define LXE_RELAUNCH_MAX_BYTES 32768
/* More relaunches than this inside LXE_RELAUNCH_WINDOW_MS is a loop, not an update. */
#define LXE_RELAUNCH_MAX_BURST 5
#define LXE_RELAUNCH_WINDOW_MS 30000

#endif
