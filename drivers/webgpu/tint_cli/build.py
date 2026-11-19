"""Build the native shader compiler with the repository's vendored sources."""

import argparse
import ast
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


DRIVER = Path(__file__).resolve().parents[1]
ROOT = DRIVER.parents[1]
DEFAULT_CACHE = DRIVER / ".build" / "host"
JSONCPP = ROOT / "thirdparty/openxr/src/external/jsoncpp"
MINGW = Path("C:/Programs/WinLibs/mingw64/bin/g++.exe")
COMPILER_SOURCES = json.loads((DRIVER / "compiler_sources.json").read_text(encoding="utf-8"))
TINT_DEFINES = COMPILER_SOURCES["tint_defines"]


def compiler_path(override=None):
    name = override or os.environ.get("CXX")
    if name:
        found = shutil.which(name) or (name if Path(name).is_file() else None)
    elif os.name == "nt" and MINGW.is_file():
        found = MINGW
    else:
        found = next((p for c in ("g++", "clang++", "cl", "c++") if (p := shutil.which(c))), None)
    if not found:
        raise RuntimeError("No host C++ compiler found (set --cxx or CXX)")
    return Path(found).resolve()


def source_groups(platform):
    tree = ast.parse((ROOT / "modules/glslang/SCsub").read_text(encoding="utf-8"))
    glslang = next(ast.literal_eval(node.value) for node in ast.walk(tree)
                   if isinstance(node, ast.Assign) and isinstance(node.value, ast.List)
                   and any(isinstance(t, ast.Name) and t.id == "thirdparty_sources" for t in node.targets))
    glslang.append(f"glslang/OSDependent/{'Windows' if platform == 'windows' else 'Unix'}/ossource.cpp")
    return {"spirv-tools": COMPILER_SOURCES["spirv_tools"],
            "tint": COMPILER_SOURCES["tint"] + COMPILER_SOURCES["tint_platform"][platform],
            "glslang": glslang}


def includes(group):
    third = ROOT / "thirdparty"
    headers = third / "spirv-headers/include"
    tools = third / "spirv-tools"
    if group == "glslang":
        return [headers / "spirv/unified1", third / "glslang", third]
    if group == "json":
        return [JSONCPP / "include"]
    if group == "cli":
        return [third / "glslang", third, third / "mbedtls/include", JSONCPP / "include", DRIVER]
    if group == "sha":
        return [third / "mbedtls/include"]
    return [third / "tint", tools, tools / "include", tools / "source", tools / "generated",
            headers, headers / "spirv/unified1", DRIVER]


def build_host(cache=DEFAULT_CACHE, cxx=None, jobs=None, deps_only=False):
    compiler = compiler_path(cxx)
    msvc = compiler.stem.lower() in {"cl", "clang-cl"}
    platform = "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"
    cache = Path(cache).resolve()
    tmp = cache / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, **{name: str(tmp) for name in ("TMP", "TEMP", "TMPDIR")}}
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    tasks = [(group, ROOT / "thirdparty" / group / name)
             for group, files in source_groups(platform).items() for name in files]
    tasks.extend(("json", source) for source in sorted((JSONCPP / "src/lib_json").glob("*.cpp")))
    tasks.extend(("sha", ROOT / "thirdparty/mbedtls/library" / name) for name in ("sha256.c", "platform_util.c"))
    if not deps_only:
        tasks.extend(("spirv", DRIVER / name) for name in COMPILER_SOURCES["godot_spirv"])
        tasks.extend(("tint", source) for source in sorted(DRIVER.glob("tint_*.cpp")))
        tasks.extend(("cli", source) for source in sorted((DRIVER / "tint_cli").glob("*.cpp")))

    def run(argv):
        p = subprocess.run([str(compiler), *argv], cwd=cache, env=env,
                           capture_output=True, encoding="utf-8", errors="replace")
        if p.returncode:
            raise RuntimeError(f"{compiler.name} {argv[-1]} ({p.returncode})\n{p.stdout}{p.stderr}")

    def compile_one(task):
        group, source = task
        inc = includes(group)
        defines = TINT_DEFINES if group == "tint" else {"ENABLE_OPT": 0} if group == "glslang" else {}
        standard = "c++20" if group in {"tint", "glslang"} else "c++17"
        key = hashlib.sha256(str(source).encode()).hexdigest()[:12]
        obj = cache / (source.stem[:28] + "-" + key + (".obj" if msvc else ".o"))
        stamp, dep = obj.with_suffix(".sig"), obj.with_suffix(".d")
        if msvc:
            flags = ["/nologo", "/Od", "/bigobj", "/utf-8"]
            flags += ["/std:c11", "/TC"] if group == "sha" else [f"/std:{standard}", "/EHsc", "/Zc:__cplusplus"]
            if group not in {"spirv", "cli"}:
                flags.append("/DNDEBUG")
            flags += [f"/D{k}={v}" for k, v in defines.items()] + [f"/I{p}" for p in inc]
            cmd = [*flags, "/c", f"/Fo{obj}", "/sourceDependencies", str(dep), str(source)]
        else:
            flags = ["-O0", "-g0", "-pthread"]
            flags += ["-x", "c", "-std=c11"] if group == "sha" else [f"-std={standard}"]
            if group not in {"spirv", "cli"}:
                flags.append("-DNDEBUG")
            if platform == "windows":
                flags.append("-Wa,-mbig-obj")
            flags += [f"-D{k}={v}" for k, v in defines.items()] + [f"-I{p.as_posix()}" for p in inc]
            cmd = [*flags, "-MMD", "-MF", dep.as_posix(), "-MT", obj.name, "-c", "-o", obj.as_posix(), source.as_posix()]
        sig = hashlib.sha256(repr((str(compiler), compiler.stat().st_mtime_ns, cmd)).encode()).hexdigest()
        if obj.is_file() and dep.is_file() and stamp.is_file() and stamp.read_text(encoding="ascii") == sig:
            if msvc:
                paths = json.loads(dep.read_text(encoding="utf-8-sig"))["Data"]["Includes"]
            else:
                text = dep.read_text(encoding="utf-8").replace("\\\n", " ").split(":", 1)[1]
                paths = [p.replace("\\ ", " ").replace("\\#", "#").replace("$$", "$")
                         for p in re.findall(r"(?:\\[ #]|[^\s])+", text)]
            age = obj.stat().st_mtime_ns
            if all((p := cache / path).is_file() and p.stat().st_mtime_ns <= age for path in [str(source), *paths]):
                return obj, False
        run(cmd)
        stamp.write_text(sig, encoding="ascii")
        return obj, True

    print(f"{compiler}: {len(tasks)} translation units", flush=True)
    with ThreadPoolExecutor(max_workers=jobs or min(12, os.cpu_count() or 1)) as pool:
        items = list(pool.map(compile_one, tasks))
    print(f"Compiled {sum(fresh for _, fresh in items)} / {len(items)} translation units", flush=True)
    if deps_only:
        return None

    exe = cache / ("tint_convert_cli.exe" if platform == "windows" else "tint_convert_cli")
    response = cache / "link.rsp"
    args = [obj.as_posix() for obj, _ in items]
    if msvc:
        args = ["/nologo", f"/Fe{exe.as_posix()}", *args]
    else:
        args = ["-pthread", "-o", exe.as_posix(), *args]
        if platform == "windows":
            args += ["-static", "-static-libgcc", "-static-libstdc++"]
    content = "\n".join('"' + a.replace('"', '\\"') + '"' for a in args) + "\n"
    relink = not response.is_file() or response.read_text(encoding="utf-8") != content
    if relink:
        response.write_text(content, encoding="utf-8")
    if relink or not exe.is_file() or any(obj.stat().st_mtime_ns > exe.stat().st_mtime_ns for obj, _ in items):
        run([f"@{response.as_posix()}"])
    return exe


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    parser.add_argument("--cxx")
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--deps-only", action="store_true")
    args = parser.parse_args()
    if result := build_host(args.cache, args.cxx, args.jobs, args.deps_only):
        print(result)
