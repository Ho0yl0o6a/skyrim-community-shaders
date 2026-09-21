"""Decode an existing NVIDIA Aftermath dump; never attaches to the game.

ABI/constants follow D:/dev/aftermath/include (Aftermath API 2.27).
Shader lookup callbacks are absent: output cannot establish source-line blame.
"""
import argparse
import ctypes as c
import json
from pathlib import Path


def shader_hashes(directory, library):
    class SpirvCode(c.Structure):
        _fields_ = [('data', c.c_void_p), ('size', c.c_uint32)]

    sdk = c.CDLL(str(library.resolve()))
    function = sdk.GFSDK_Aftermath_GetShaderHashSpirv
    function.argtypes = [c.c_uint32, c.POINTER(SpirvCode), c.POINTER(c.c_uint64)]
    function.restype = c.c_uint32
    hashes = {}
    for path in sorted(directory.rglob('*.spv')):
        data = path.read_bytes()
        buffer = c.create_string_buffer(data)
        code = SpirvCode(c.cast(buffer, c.c_void_p), len(data))
        value = c.c_uint64()
        result = function(0x21B, c.byref(code), c.byref(value))
        if result != 1:
            raise RuntimeError(f'{path}: hash result 0x{result:08x}')
        hashes[str(path)] = value.value
    return hashes


def decode(dump, library):
    class ShaderInfo(c.Structure):
        _fields_ = [('shaderHash', c.c_uint64), ('debugUid', c.c_uint64),
                    ('internal', c.c_uint32), ('shaderType', c.c_uint32)]

    sdk = c.CDLL(str(library.resolve()))
    pointer, uint = c.c_void_p, c.c_uint32
    signatures = {
        'CreateDecoder': [uint, pointer, uint, c.POINTER(pointer)],
        'DestroyDecoder': [pointer],
        'GenerateJSON': [pointer, uint, uint, pointer, pointer, pointer, pointer, c.POINTER(uint)],
        'GetJSON': [pointer, uint, pointer],
        'GetActiveShadersInfoCount': [pointer, c.POINTER(uint)],
        'GetActiveShadersInfo': [pointer, uint, c.POINTER(ShaderInfo)],
    }
    calls = {}
    for name, args in signatures.items():
        function = getattr(sdk, 'GFSDK_Aftermath_GpuCrashDump_' + name)
        function.argtypes, function.restype = args, uint
        calls[name] = function

    def checked(name, *args):
        result = calls[name](*args)
        if result != 1:
            raise RuntimeError(f'{name}: Aftermath result 0x{result:08x}')

    data = dump.read_bytes()
    buffer = c.create_string_buffer(data)
    decoder = pointer()
    checked('CreateDecoder', 0x21B, buffer, len(data), c.byref(decoder))
    try:
        size = uint()
        checked('GenerateJSON', decoder, 0x3FFF, 2, None, None, None, None, c.byref(size))
        output = c.create_string_buffer(size.value)
        checked('GetJSON', decoder, size, output)
        decoded = json.loads(output.value.decode('utf-8'))
        count = uint()
        checked('GetActiveShadersInfoCount', decoder, c.byref(count))
        infos = (ShaderInfo * count.value)()
        checked('GetActiveShadersInfo', decoder, count, infos)
        get_hash = sdk.GFSDK_Aftermath_GetShaderHashForShaderInfo
        get_hash.argtypes = [pointer, c.POINTER(ShaderInfo), c.POINTER(c.c_uint64)]
        get_hash.restype = uint
        binary_hashes = []
        for info in infos:
            binary_hash = c.c_uint64()
            result = get_hash(decoder, c.byref(info), c.byref(binary_hash))
            if result != 1:
                raise RuntimeError(f'GetShaderHashForShaderInfo: 0x{result:08x}')
            binary_hashes.append(dict(shaderInfoHash=info.shaderHash,
                                      shaderBinaryHash=binary_hash.value,
                                      internal=bool(info.internal), shaderType=info.shaderType))
        # ShaderInfo.shaderHash and ShaderBinaryHash are different hash domains.
        decoded.append({'Comparable shader binary hashes': binary_hashes})
        return json.dumps(decoded, indent=2)
    finally:
        checked('DestroyDecoder', decoder)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--library', type=Path, default=Path('D:/dev/aftermath/lib/x64/GFSDK_Aftermath_Lib.x64.dll'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--shader-directory', type=Path)
    args = parser.parse_args()
    decoded = decode(args.dump, args.library)
    with args.output.open('x', encoding='utf-8') as target:
        target.write(decoded)
    if args.shader_directory:
        hashes = shader_hashes(args.shader_directory, args.library)
        with args.output.with_suffix('.shader-hashes.json').open('x', encoding='utf-8') as target:
            json.dump(hashes, target, indent=2)
        print(f'Hashed {len(hashes)} SPIR-V files; compare against decoded shader hashes.')
    print(args.output)
