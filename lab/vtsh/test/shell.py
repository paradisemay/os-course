import os
import subprocess


def _ensure_built(binary_path: str, project_root: str):
    if os.path.exists(binary_path):
        return

    build_dir = os.path.join(project_root, "build")
    os.makedirs(build_dir, exist_ok=True)

    subprocess.run([
        "cmake",
        "-S",
        project_root,
        "-B",
        build_dir,
    ], check=True)

    subprocess.run([
        "cmake",
        "--build",
        build_dir,
    ], check=True)

    if not os.path.exists(binary_path):
        raise FileNotFoundError(f"Failed to build vtsh at {binary_path}")


class Shell:
    def __init__(self, path: str):
        base_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.abspath(os.path.join(base_dir, ".."))
        self._path = os.path.abspath(os.path.join(base_dir, path))

        _ensure_built(self._path, project_root)

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
