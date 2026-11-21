$ErrorActionPreference = 'Stop'
$clang='C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin/clang.exe'
$bundle='E:/Pokopia-PS4/gpu-probe/deps/orbis-sdk-v1'
Push-Location -LiteralPath $PSScriptRoot
try {
    $flags=@('--target=x86_64-pc-freebsd12-elf','-march=btver2','-std=c11','-O2','-fPIC','-funwind-tables',
        '-D__PS4__','-DPS4','-D__ORBIS__','-D_BSD_SOURCE=1','-isysroot',"$bundle/sdk",
        '-isystem',"$bundle/orbis-compat/include",'-isystem',"$bundle/sdk/include",'-isystem',"$bundle/mesa/include",
        '-include','orbis_prefix.h','-I.','-I../../gfx-bridge','-Wall','-Wextra','-Werror')
    foreach ($name in 'nvn_front','nvn_replay','vk_program','shader_cache') {
        & $clang @flags -c "$name.c" -o "$name.orbis.o"
        if ($LASTEXITCODE) { throw "Orbis compile failed ($LASTEXITCODE) for $name." }
    }
} finally {
    Pop-Location
}

