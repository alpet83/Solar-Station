# Ревизия сборки -> -DGS_REV="<git short>[x]" ("x" — есть незакоммиченные изменения).
# GS_VERSION = "0.1v-<ревизия>" — версия в BOOT-сообщении и в ${fw_version} вебхука.
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
FW_PREFIX = "0.1v"        # меняется вручную при смене мажорной/минорной версии
env.Append(CPPDEFINES=[("GS_REV", env.StringifyMacro(rev)),
                       ("GS_VERSION", env.StringifyMacro(FW_PREFIX + "-" + rev))])
