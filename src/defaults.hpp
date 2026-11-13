// Where to look for the Pi, in one place.
//
// The defaults are deliberately generic: they assume a Raspberry Pi on your LAN
// that answers to its mDNS name, with the standard `pi` account and a normal
// id_ed25519 key. Nothing here knows about anyone's network.
//
// Resolution order, for every value:
//   1. the command-line flag (--host, --user, --key, --device)
//   2. the matching environment variable: PIBRIDGE_HOST, PIBRIDGE_USER,
//      PIBRIDGE_KEY, PIBRIDGE_DEVICE
//   3. the generic default below
//
// The app has the same three steps in app/Engine.swift (PiDefaults) and reads
// the same variables. Keep the two in step.

#pragma once

#include <cstdlib>
#include <string>

namespace pib {

// `raspberrypi.local` resolves over mDNS/Bonjour on a stock Raspberry Pi OS
// install. If your Pi has a different name (or the NAA image, which does not
// advertise one), use its address: --host 10.0.0.42.
inline constexpr const char* kDefaultHost = "raspberrypi.local";
inline constexpr const char* kDefaultUser = "pi";
// ALSA's first card, first device: the usual single-USB-DAC case. The receiver
// drives the device directly, so it must be an hw: device, not `default`.
inline constexpr const char* kDefaultDevice = "hw:0,0";
inline constexpr const char* kDefaultKeyPath = "~/.ssh/id_ed25519";

inline const char* envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

inline std::string defaultHost() { return envOr("PIBRIDGE_HOST", kDefaultHost); }
inline std::string defaultUser() { return envOr("PIBRIDGE_USER", kDefaultUser); }
inline std::string defaultDevice() { return envOr("PIBRIDGE_DEVICE", kDefaultDevice); }

// The form to show in help: the tilde path, not the expanded one.
inline const char* defaultKeyPathDisplay() { return envOr("PIBRIDGE_KEY", kDefaultKeyPath); }

// ssh wants an absolute path, so the tilde form is expanded from $HOME here
// rather than being passed through the shell.
inline std::string defaultKeyPath() {
    const std::string path = envOr("PIBRIDGE_KEY", kDefaultKeyPath);
    if (path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        return std::string((home && *home) ? home : ".") + path.substr(1);
    }
    return path;
}

}  // namespace pib
