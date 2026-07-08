#ifndef TEXT_SANITIZE_H
#define TEXT_SANITIZE_H

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// String cleaners shared by the config and network code, kept Arduino-free so
// they are unit-tested on the host (test/test_textsanitize). The firmware's
// sanitizeHostname (projectConfig) and normalizeMac/parseMac (netCheck) are
// thin String wrappers over these.
// ---------------------------------------------------------------------------

namespace puretext
{
// Lowercase, keep only [a-z0-9-], trim leading/trailing dashes, cap at 32
// chars; falls back to "esp32worldclock" when nothing usable is left.
std::string sanitizeHostname(const std::string &raw);

// Parse "AA:BB:CC:DD:EE:FF" or "AA-BB-CC-DD-EE-FF" into 6 bytes. Returns false
// (and leaves out[] untouched) if the string isn't six hex octets.
bool parseMac(const std::string &s, uint8_t out[6]);

// Trim, then canonicalise a MAC to uppercase colon form. An empty string
// returns empty; an unparseable string is returned trimmed-but-unchanged so
// the user can see and fix their typo.
std::string normalizeMac(const std::string &s);
} // namespace puretext

#endif // TEXT_SANITIZE_H
