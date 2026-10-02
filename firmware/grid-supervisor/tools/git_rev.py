# Ревизия сборки -> -DGS_REV="<git short>[x]" ("x" — есть незакоммиченные изменения).
# Пароль точки доступа по умолчанию: gs-<ревизия> (пока не задан свой в /setup).
Import("env")
import subprocess

def _git(*a):
    return subprocess.check_output(("git",) + a, cwd=env["PROJECT_DIR"], stderr=subprocess.DEVNULL).decode().strip()

try:
    rev = _git("rev-parse", "--short=7", "HEAD")
    if _git("status", "--porcelain", "--", "."):
        rev += "x"
except Exception:
    rev = "nogit"
env.Append(CPPDEFINES=[("GS_REV", env.StringifyMacro(rev))])
