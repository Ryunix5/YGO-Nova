#pragma once

// Minimal Discord Rich Presence over the local IPC named pipe — no SDK, no
// extra DLLs. Shows "Playing YGO: Nova — <details> / <state>" on the user's
// Discord profile. Requires a Discord Application ID (create one free at
// discord.com/developers, any name); an empty id disables everything.
//
// All calls are cheap and safe when Discord isn't running: connection is
// attempted lazily and failures back off. Windows-only (no-op elsewhere).
namespace discordrpc {
// Update (or clear with empty details) the presence. Reconnects as needed.
void update(const char* clientId, const char* details, const char* state);
// Drop the connection (app exit).
void shutdown();
}
