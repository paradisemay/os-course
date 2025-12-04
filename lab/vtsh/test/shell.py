import os
import subprocess


class Shell:
    def __init__(self, path: str):
        base_dir = os.path.dirname(os.path.abspath(__file__))
        self._path = os.path.abspath(os.path.join(base_dir, path))

    def execute(self, cmd: str):
        shell = subprocess.Popen(
            self._path,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf8",
        )

        stdout, _ = shell.communicate(cmd + "\n", timeout=2)
        return shell.returncode, stdout.replace("vtsh> ", "").strip()