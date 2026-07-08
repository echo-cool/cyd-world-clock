import subprocess

Import("env")


def git_output(project_dir, *args):
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return ""


project_dir = env.subst("$PROJECT_DIR")
short_hash = git_output(project_dir, "rev-parse", "--short=8", "HEAD") or "unknown"
status = git_output(project_dir, "status", "--porcelain", "--untracked-files=all")
version = short_hash + ("-dirty" if status else "")

env.Append(CPPDEFINES=[("FIRMWARE_GIT_HASH", '\\"{}\\"'.format(version))])
print("Firmware git hash: {}".format(version))
