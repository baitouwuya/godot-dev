"""Read GLSL from a Vulkan ShaderRD cache made with --generate-spirv-debug-info.

Captures preserve the actual ShaderCompiler output, including injected material GLSL.
They are not precompiled renderer cache inputs: driver capability defines can differ.
"""

import os
from pathlib import Path
import re
import struct


CACHE_VERSION = 4  # ShaderRD::save_shader_cache_bytes
VULKAN_CONTAINER = (0x43535247, 2, 0x43565053, 1)
STAGES = {0: "vertex", 1: "fragment", 4: "compute"}  # RenderingDeviceCommons::ShaderStage
SPIRV_MAGIC = 0x07230203
OP_STRING = 7
OP_EXT_INST_IMPORT = 11
OP_EXT_INST = 12
DEBUG_SOURCE = 35
DEBUG_SOURCE_CONTINUED = 102


def c_string(data):
    return data.split(b"\0", maxsplit=1)[0]


def source_text(data):
    words = tuple(word for word, in struct.iter_unpack("<I", data))
    if len(words) < 5 or words[0] != SPIRV_MAGIC:
        raise ValueError("Expected uncompressed Vulkan SPIR-V")
    strings, debug_imports, sources = {}, set(), []
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xFFFF
        if count == 0 or i + count > len(words):
            raise ValueError("Invalid instruction in captured SPIR-V")
        if opcode in (OP_STRING, OP_EXT_INST_IMPORT):
            text = c_string(data[(i + 2) * 4:(i + count) * 4])
            if opcode == OP_STRING:
                strings[words[i + 1]] = text
            elif text == b"NonSemantic.Shader.DebugInfo.100":
                debug_imports.add(words[i + 1])
        elif opcode == OP_EXT_INST and words[i + 3] in debug_imports:
            ext = words[i + 4]
            if ext == DEBUG_SOURCE and count == 7:
                sources.append([strings[words[i + 6]]])
            elif ext == DEBUG_SOURCE_CONTINUED:
                sources[-1].append(strings[words[i + 5]])
        i += count
    glsl_sources = [b"".join(chunks) for chunks in sources if b"#version" in chunks[0]]
    if len(glsl_sources) != 1:
        raise ValueError("Expected one GLSL DebugSource (use --generate-spirv-debug-info and a fresh cache)")
    return glsl_sources[0].decode("utf-8")


def stages_from_container(data):
    *header, shader_count = struct.unpack_from("<5I", data)
    if tuple(header) != VULKAN_CONTAINER:
        raise ValueError(f"Unsupported shader container {header}")
    reflection = struct.Struct("<Q13I4x")
    _, _, specs, _, _, _, _, _, _, sets, _, _, stages, name_bytes = reflection.unpack_from(data, 20)
    offset = (20 + reflection.size + name_bytes + 3) & ~3
    for _ in range(sets):
        uniforms, = struct.unpack_from("<I", data, offset)
        offset += 4 + uniforms * 20
    offset += specs * 16 + stages * 4
    result = []
    for _ in range(shader_count):
        stage, size, flags, _ = struct.unpack_from("<4I", data, offset)
        offset += 16
        if flags or stage not in STAGES or offset + size > len(data):
            raise ValueError("Expected uncompressed vertex, fragment or compute shader")
        result.append({"stage": STAGES[stage], "source": source_text(data[offset:offset + size])})
        offset = (offset + size + 3) & ~3
    if offset != len(data):
        raise ValueError("Shader container has trailing or missing data")
    return result


def captures(cache: Path):
    if os.name == "nt":
        # ShaderRD uses two hashes in the same path. Relative paths hit MAX_PATH.
        path = str(cache.resolve())
        if not path.startswith("\\\\?\\"):
            path = "\\\\?\\UNC\\" + path[2:] if path.startswith("\\\\") else "\\\\?\\" + path
        cache = Path(path)
    for file in sorted(cache.rglob("*.vulkan.cache")):
        data = file.read_bytes()
        if data[:4] != b"GDSC":
            continue  # Ignore the Vulkan pipeline cache.
        version, count = struct.unpack_from("<II", data, 4)
        if version != CACHE_VERSION:
            raise ValueError(f"{file}: unsupported ShaderRD cache version {version}")
        offset = 12
        for variant in range(count):
            size, = struct.unpack_from("<I", data, offset)
            offset += 4
            if size:
                try:
                    stages = stages_from_container(data[offset:offset + size])
                except (ValueError, struct.error) as error:
                    raise ValueError(f"{file}:{variant}: {error}") from error
                yield file.parent.parent.name, f"{file.parent.name}/{file.name.split('.')[0]}/{variant}", stages
            offset += size
        if offset != len(data):
            raise ValueError(f"{file}: invalid ShaderRD cache length")


def key(binding, set_=0, **parameters):
    return {"set": set_, "binding": binding, **parameters}


def captured_jobs(label, cache):
    """Recompile the assembled GLSL at the WebGPU driver's Vulkan 1.1 / SPIR-V 1.3 target."""
    found = False
    for shader, variant, stages in captures(cache):
        found = True
        options = {"non_uniform_derivatives": True}
        if shader == "SceneForwardClusteredShaderRD":
            options["subgroup_lowering"] = "clustered_scene"
            options["raw_depth"] = [key(5, 1), key(6, 1)]
        elif shader == "SceneForwardMobileShaderRD":
            options["raw_depth"] = [key(4, 1), key(5, 1)]
        elif shader == "ClusterRenderShaderRD":
            options.update(subgroup_lowering="cluster_raster", specialization=[{"id": 0, "bits": 0}])
        elif shader == "SdfgiPreprocessShaderRD":
            # Translate the optional extension path too, even on adapters where
            # SDFGI cannot be enabled. Translation is not a device limit check.
            options["texture_barrier"] = True
        elif shader in {"ResolveShaderRD", "ResolveRasterShaderRD", "SsEffectsDownsampleShaderRD",
                        "ScreenSpaceReflectionDownsampleShaderRD"}:
            options["raw_depth"] = [key(0)]
        elif shader == "ScreenSpaceReflectionFilterShaderRD":
            options["storage_formats"] = [key(1, format=2)]  # SPIR-V Rgba16f, SSR color pyramid.
        elif shader == "CopyShaderRD" and any(re.search(r"^#define MODE_(SIMPLE_COPY_DEPTH|LINEARIZE_DEPTH_COPY)\b",
                                                       stage["source"], re.M) for stage in stages):
            options["raw_depth"] = [key(0)]

        adjustments = set()
        for stage in stages:
            source = stage["source"]
            source = source.replace("#define RENDER_DRIVER_VULKAN\n", "#define RENDER_DRIVER_WEBGPU\n", 1)
            for macro in ("USE_MULTIVIEW", "USE_VRS", "USE_VULKAN_MEMORY_MODEL", "SUBPASS"):
                line = f"#define {macro}\n"
                if line in source:
                    source = source.replace(line, "", 1)
                    adjustments.add(macro)
            if shader == "BlurRasterShaderRD":
                source = source.replace("#define RENDER_DRIVER_WEBGPU\n",
                                        "#define EMULATE_TRANSPARENT_BORDER\n#define RENDER_DRIVER_WEBGPU\n", 1)
            stage["source"] = source
        yield {"id": f"{label}/{shader}/{variant}", "options": options, "stages": stages,
               "disabled_capabilities": sorted(adjustments)}
    if not found:
        raise ValueError(f"No ShaderRD variants in {cache}")


def probe_job(path):
    sources = re.findall(r'\bsrc\.source_compute\s*=\s*"""(.*?)"""', path.read_text(encoding="utf-8"), re.S)
    if len(sources) != 1:
        raise ValueError(f"Expected one source_compute string in {path}")
    return {"id": "probe/rgba8-image2D", "stages": [{"stage": "compute", "source": sources[0]}]}
