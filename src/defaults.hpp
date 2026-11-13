// Defaults for the rig this was built against, in one place.
//
// They exist so `./build/pibridge diag` works with no arguments on the machine
// it was developed on. Every value can be overridden — by flag on the CLI, or
// in the app's settings — and `pibridge diag` prints what it is about to use.
//
// If you are pointing this at your own Pi, either edit these four lines or pass
// --host/--user/--key/--device. The app has the same defaults in
// app/Engine.swift (PiDefaults); keep the two in step.

#pragma once

#include <cstdlib>
#include <string>

namespace pib {

inline constexpr const char* kDefaultHost = "192.168.86.20";
inline constexpr const char* kDefaultUser = "up";
inline constexpr const char* kDefaultDevice = "hw:CARD=R2R,DEV=0";
inline constexpr const char* kDefaultKeyName = "hermes_pi";   // ~/.ssh/<name>

// ssh wants an absolute path, so the tilde form is expanded from $HOME here
// rather than being passed through the shell.
inline std::string defaultKeyPath() {
    const char* home = std::getenv("HOME");
    const std::string base = (home && *home) ? home : ".";
    return base + "/.ssh/" + kDefaultKeyName;
}

}  // namespace pib
