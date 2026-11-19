"""Precompile Godot RD shader variants with the same compiler used by the WebGPU driver."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from tint_cli.build import DRIVER, ROOT, build_host
from rd_shader_capture import captured_jobs, key, probe_job


@contextmanager
def engine_directory():
    previous = Path.cwd()
    try:
        os.chdir(ROOT)
        yield
    finally:
        os.chdir(previous)


def shader_stages(path):
    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    from glsl_builders import RDHeaderStruct, include_file_in_rd_header

    # The generator resolves thirdparty/ includes relative to the engine root.
    with engine_directory():
        parsed = include_file_in_rd_header(str(path), RDHeaderStruct(), 0)
    return {name: getattr(parsed, name + "_lines") for name in ("vertex", "fragment", "compute")
            if getattr(parsed, name + "_lines")}


def defines(names):
    return "".join(f"\n#define {name}\n" for name in names)


def assemble(lines, general="", variant="", globals_code="", uniforms="", sections=None, custom=""):
    """ShaderRD::_add_stage / _build_variant_code, for the generated RD stages."""
    sections = sections or {}
    out = []
    # methods.to_raw_cstring(list) adds one final newline; String::split in
    # _add_stage retains the trailing empty line.
    for line in [*lines, ""]:
        if line.startswith("#VERSION_DEFINES"):
            out.extend(("\n", general, variant, custom, "\n"))
            if uniforms:
                out.append("#define MATERIAL_UNIFORMS_USED\n")
            out.extend(f"#define {key}_CODE_USED\n" for key in sections)
            out.append("#define RENDER_DRIVER_WEBGPU\n#define samplerExternalOES sampler2D\n#define textureExternalOES texture2D\n")
        elif line.startswith("#MATERIAL_UNIFORMS"):
            out.append(uniforms)
        elif line.startswith("#GLOBALS"):
            out.append(globals_code)
        elif line.startswith("#CODE"):
            key = line.removeprefix("#CODE").replace(":", "").strip().upper()
            out.append(sections.get(key, ""))
        else:
            out.append(line + "\n")
    return "".join(out)


def builtin_jobs():
    """Named non-XR FP32 profile: includes dynamic lightmaps and clustered GI."""
    base = Path("servers/rendering/renderer_rd/shaders")
    jobs = []

    def add(path, variants, general=(), *, options=None, sections=None, globals_code=None):
        stages = shader_stages(base / (path + ".glsl"))
        for name, flags in variants.items():
            jobs.append({"id": f"{path}/{name}",
                         "options": {"non_uniform_derivatives": True, **(options or {})},
                         "stages": [{"stage": stage, "source": assemble(lines, defines(general), defines(flags),
                                                                     (globals_code or {}).get(stage, ""), sections=sections)}
                                    for stage, lines in stages.items()]})

    def default():
        return {"default": ()}

    scene = ("MAX_ROUGHNESS_LOD 7.0", "USE_RADIANCE_OCTMAP_ARRAY", "MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8",
             "MAX_LIGHTMAP_TEXTURES 8", "MAX_LIGHTMAPS 8", "MATERIAL_UNIFORM_SET 3")
    mobile = {"color": (), "lightmaps": ("USE_LIGHTMAP",), "shadow": ("MODE_RENDER_DEPTH", "SHADOW_PASS"),
              "shadow_dp": ("MODE_RENDER_DEPTH", "MODE_DUAL_PARABOLOID", "SHADOW_PASS"),
              "depth_material": ("MODE_RENDER_DEPTH", "MODE_RENDER_MATERIAL")}
    clustered = {"color": (), "lightmaps": ("USE_LIGHTMAP",), "separate_specular": ("MODE_SEPARATE_SPECULAR",),
                 "velocity": ("MOTION_VECTORS",), "depth": ("MODE_RENDER_DEPTH",),
                 "depth_normal": ("MODE_RENDER_DEPTH", "MODE_RENDER_NORMAL_ROUGHNESS"),
                 "depth_voxel_gi": ("MODE_RENDER_DEPTH", "MODE_RENDER_NORMAL_ROUGHNESS", "MODE_RENDER_VOXEL_GI")}
    with_uber = lambda variants: {**variants, **{f"uber_{name}": ("UBERSHADER", *flags) for name, flags in variants.items()}}
    add("forward_mobile/scene_forward_mobile", with_uber(mobile), scene)
    add("forward_clustered/scene_forward_clustered", with_uber(clustered),
        ("SDFGI_OCT_SIZE 5", *scene), options={"subgroup_lowering": "clustered_scene"})
    add("canvas", {"quad": (), "ninepatch": ("USE_NINEPATCH",), "primitive": ("USE_PRIMITIVE",),
                   "primitive_points": ("USE_PRIMITIVE", "USE_POINT_SIZE"), "attributes": ("USE_ATTRIBUTES",),
                   "attributes_points": ("USE_ATTRIBUTES", "USE_POINT_SIZE")},
        ("MAX_LIGHTS 256", "SAMPLERS_BINDING_FIRST_INDEX 10"))
    add("cluster_render", default(), options={"subgroup_lowering": "cluster_raster", "specialization": [{"id": 0, "bits": 0}]})
    add("cluster_store", default())
    add("skeleton", {"3d": (), "2d": ("MODE_2D",)})
    add("particles", default(), ("SAMPLERS_BINDING_FIRST_INDEX 3",))
    add("particles_copy", {"fill": ("MODE_FILL_INSTANCES",), "sort": ("MODE_FILL_SORT_BUFFER", "USE_SORT_BUFFER")})
    add("environment/sky", {"full": (), "cube": ("USE_CUBEMAP_PASS",), "half": ("USE_HALF_RES_PASS",),
                            "quarter": ("USE_QUARTER_RES_PASS",)},
        ("MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 4", "SAMPLERS_BINDING_FIRST_INDEX 4"))
    add("effects/tonemap", {"normal": (), "lut": ("USE_1D_LUT",), "bicubic": ("USE_GLOW_FILTER_BICUBIC",),
                            "bicubic_lut": ("USE_GLOW_FILTER_BICUBIC", "USE_1D_LUT")})
    add("effects/tonemap_mobile", {"normal": (), "lut": ("USE_1D_LUT",)})
    add("effects/copy", {"color": ("MODE_SIMPLE_COPY",), "color_8bit": ("MODE_SIMPLE_COPY", "DST_IMAGE_8BIT"),
                         "mipmap": ("MODE_MIPMAP",), "set_color": ("MODE_SET_COLOR",)})
    add("effects/copy", {"depth": ("MODE_SIMPLE_COPY_DEPTH",), "linear_depth": ("MODE_LINEARIZE_DEPTH_COPY",)},
        options={"raw_depth": [key(0)]})
    add("effects/copy_to_fb", default())
    add("effects/octmap_downsampler_raster", default())
    add("effects/octmap_filter_raster", {"high": ("USE_HIGH_QUALITY",), "low": ("USE_LOW_QUALITY",)})
    add("effects/octmap_roughness_raster", default())
    add("effects/ss_effects_downsample", {
        "depth": (), "half_size": ("USE_HALF_SIZE",), "mips": ("GENERATE_MIPS",),
        "mips_half_size": ("GENERATE_MIPS", "USE_HALF_SIZE"), "half_buffers": ("USE_HALF_BUFFERS",),
        "half_buffers_half_size": ("USE_HALF_BUFFERS", "USE_HALF_SIZE"),
        "full_mips": ("GENERATE_MIPS", "GENERATE_FULL_MIPS"),
    }, options={"raw_depth": [key(0)]})
    add("effects/resolve", {"gi": ("MODE_RESOLVE_GI",), "voxel_gi": ("MODE_RESOLVE_GI", "VOXEL_GI_RESOLVE"),
                            "depth": ("MODE_RESOLVE_DEPTH",)}, options={"raw_depth": [key(0)]})
    add("effects/resolve_raster", default(), options={"raw_depth": [key(0)]})
    add("effects/sort", {"block": ("MODE_SORT_BLOCK",), "step": ("MODE_SORT_STEP",), "inner": ("MODE_SORT_INNER",)})
    add("effects/luminance_reduce", {"first": ("READ_TEXTURE",), "reduce": (), "last": ("WRITE_LUMINANCE",)})
    add("effects/luminance_reduce_raster", {"first": ("FIRST_PASS",), "reduce": (), "last": ("FINAL_PASS",)})
    add("effects/ssil", {"normal": (), "base": ("SSIL_BASE",), "adaptive": ("ADAPTIVE",)})
    for suffix in ("", "_downsample", "_hiz", "_resolve"):
        add("effects/screen_space_reflection" + suffix, default())
    # SPIR-V Rgba16f. SSR filter's formatless dest aliases the SSR rgba16f color pyramid.
    add("effects/screen_space_reflection_filter", default(), options={"storage_formats": [key(1, format=2)]})
    return jobs


def dumps(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def run_shards(executable, jobs, directory, workers, shard_size=8):
    if not jobs:
        raise ValueError("No shader variants selected")
    if len({job["id"] for job in jobs}) != len(jobs):
        raise ValueError("Duplicate variant ID")
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="shards-", dir=directory) as temp:
        scratch = Path(temp)

        def invoke(batch, tag):
            inp, out = scratch / f"{tag}.json", scratch / f"{tag}.out.json"
            inp.write_text(dumps({"jobs": batch}), encoding="utf-8")
            proc = subprocess.run([str(executable), "--manifest", str(inp), "--out", str(out)],
                                  cwd=scratch, shell=False, capture_output=True, encoding="utf-8", errors="replace")
            if proc.returncode in (0, 2) and out.is_file():
                payload = json.loads(out.read_text(encoding="utf-8"))
                if [entry["id"] for entry in payload["jobs"]] != [entry["id"] for entry in batch]:
                    raise RuntimeError(f"CLI result IDs differ for shard {tag}")
                return payload["jobs"], 0
            if len(batch) == 1:
                return [{"id": batch[0]["id"], "ok": False,
                         "diagnostic": f"Compiler terminated ({proc.returncode}): {proc.stdout}{proc.stderr}"}], 1
            pivot = len(batch) // 2
            left, lc = invoke(batch[:pivot], tag + "L")
            right, rc = invoke(batch[pivot:], tag + "R")
            return left + right, lc + rc + 1

        batches = [(jobs[start:start + shard_size], str(start)) for start in range(0, len(jobs), shard_size)]
        with ThreadPoolExecutor(max_workers=workers) as pool:
            shards = list(pool.map(lambda work: invoke(*work), batches))
    return [r for results, _ in shards for r in results], sum(count for _, count in shards)


def cache_payload(jobs, records):
    """Persist complete plans: SPIR-V alone does not key specialization or remapping."""
    h = hashlib.sha256()
    for folder in (DRIVER, ROOT / "thirdparty/tint", ROOT / "thirdparty/spirv-tools", ROOT / "thirdparty/glslang"):
        # Compiler build fingerprint; include source contents (not binary timestamps or host paths).
        for parent, directories, names in os.walk(folder):
            directories[:] = sorted(name for name in directories if name != ".build")
            for name in sorted(names):
                path = Path(parent) / name
                if path.suffix not in {".cpp", ".cc", ".h", ".hpp", ".inc", ".json"} or name.endswith(".gen.h"):
                    continue
                h.update(path.relative_to(ROOT).as_posix().encode() + b"\0" + path.read_bytes() + b"\0")
    fingerprint = h.hexdigest()
    entries = {}
    for job, result in zip(jobs, records, strict=True):
        inputs = {"compiler": fingerprint, "options": job.get("options", {}),
                  "stages": {name: stage["sha256"] for name, stage in result["stages"].items()}}
        digest = hashlib.sha256(dumps(inputs).encode()).hexdigest()
        value = {k: result[k] for k in ("plan", "stages")}
        if digest in entries and entries[digest] != value:
            raise RuntimeError(f"Non-deterministic compilation: {result['id']}")
        entries[digest] = value
    return {"schema": 1, "compiler": fingerprint, "entries": entries}


def atomic_write(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=path.parent, delete=False) as file:
        temp = Path(file.name)
        file.write(contents)
    try:
        temp.replace(path)
    finally:
        temp.unlink(missing_ok=True)


def print_peaks(records):
    for renderer in ("forward_mobile", "forward_clustered"):
        variants = [record for record in records if record["ok"] and record["id"].startswith(renderer + "/")]
        for stage in ("vertex", "fragment"):
            if variants:
                peaks = {name: max(v["plan"]["limits"][stage][name] for v in variants) for name in ("T", "S", "B", "I", "U")}
                print(renderer, stage, peaks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DRIVER / ".build/wgsl_cache.json")
    parser.add_argument("--embed", type=Path, help="also generate a typed C++ plan table")
    parser.add_argument("--report", type=Path, default=DRIVER / ".build/shader_report.json")
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--cxx")
    parser.add_argument("--match", default="")
    parser.add_argument("--capture", nargs=2, metavar=("LABEL", "CACHE"), action="append",
                        help="test assembled GLSL from a fresh Vulkan ShaderRD cache (report only; repeatable)")
    parser.add_argument("--probe-script", type=Path, help="also test the raw RD compute source in main.gd")
    args = parser.parse_args()
    if args.embed and (args.capture or args.probe_script):
        parser.error("--embed requires the built-in inputs, not captures")
    cli = args.cli.resolve() if args.cli else build_host(cxx=args.cxx, jobs=args.jobs)
    jobs = ([job for label, cache in args.capture for job in captured_jobs(label, Path(cache))]
            if args.capture else builtin_jobs())
    if args.probe_script:
        jobs.append(probe_job(args.probe_script))
    jobs = [job for job in jobs if args.match in job["id"]]
    records, crashes = run_shards(cli, jobs, DRIVER / ".build", args.jobs)
    for job, record in zip(jobs, records, strict=True):
        if "disabled_capabilities" in job:
            record["disabled_capabilities"] = job["disabled_capabilities"]
    atomic_write(args.report, dumps({"crashes": crashes, "jobs": records}) + "\n")
    failures = [record for record in records if not record["ok"]]
    for record in failures:
        print(f"{record['id']}: {record['diagnostic']}", file=sys.stderr)
    print(f"{len(records) - len(failures)}/{len(records)} variants translated, {crashes} abnormal CLI exits")
    print_peaks(records)
    if failures or crashes:
        return 1
    if args.capture or args.probe_script:
        return 0  # A capture is evidence, not a portable precompile cache input.
    payload = cache_payload(jobs, records)
    atomic_write(args.output, dumps(payload) + "\n")
    if args.embed:
        from wgsl_embed import embedded_header

        atomic_write(args.embed, embedded_header(jobs, records, payload["compiler"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
