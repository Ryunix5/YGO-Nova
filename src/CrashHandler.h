#pragma once

// Fatal-error reporter (Windows). Installs an unhandled-exception filter that
// writes a minidump + a plain-text report to crash_dumps\, then offers to open
// a pre-filled GitHub issue so testers can report crashes with one click
// instead of "it just crashed" over chat. No-op on non-Windows builds.
namespace crash {
// `version` is baked into the report; `repo` is "owner/name" for the issue
// link (blank disables the issue prompt but still writes the dump).
void install(const char* version, const char* repo);
}
