#ifndef FIRMWARE_INFO_H
#define FIRMWARE_INFO_H

// PlatformIO injects this from tools/git_version.py. Arduino IDE / arduino-cli
// builds that do not run the script still compile and show "unknown".
#ifndef FIRMWARE_GIT_HASH
#define FIRMWARE_GIT_HASH "unknown"
#endif

inline const char *firmwareGitHash()
{
    return FIRMWARE_GIT_HASH;
}

#endif // FIRMWARE_INFO_H
