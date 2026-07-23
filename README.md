# WineForge DXCompat

WineForge DXCompat is an x86_64 PE compatibility frontend for the D3D12
backend selected by WineForge. Calls outside its compatibility scope are
forwarded to that backend.

The project provides focused compatibility for missing D3DMetal behavior until
equivalent support becomes available in GPTK. Individual compatibility paths
can then be retired in favor of the native backend implementation.

## Current implementation

The current D3D12 frontend addresses a verified Unreal Engine media path where
missing NV12 and resource-lifetime behavior caused black video output or
application stalls.

The D3D12 frontend currently provides:

- an NV12 facade backed by `R8` and `RG8` textures
- NV12 format and copyable-footprint handling
- SRV, copy, legacy barrier, and enhanced barrier translation
- resource lifetime and destruction notification for the verified upload
  resource contract
- passthrough behavior for unrelated D3D12 calls

## Runtime files

```text
lib/wfdxcompat/
  x86_64-windows/
    d3d12.dll
    wfdxbackend-d3d12.dll
```

`d3d12.dll` is the compatibility frontend. `wfdxbackend-d3d12.dll` is a Wine
loader marker mapped to the active D3D12 backend; it does not contain a
graphics implementation.

## Build

Required tools:

- `x86_64-w64-mingw32-clang`;
- `winebuild`.

WineForge builds use the macOS Universal UCRT package available from the
[llvm-mingw releases](https://github.com/mstorsjo/llvm-mingw/releases) page.

Set `TOOLCHAIN` and `WINEBUILD` when these tools are not available in `PATH`.

```sh
./build.sh
```

The build stages the runtime files under
`lib/wfdxcompat/x86_64-windows`.

## Documentation

- [Developer guide and source map](docs/architecture.md)
- [Development provenance](PROVENANCE.md)

## Licensing

WineForge DXCompat uses the GNU Lesser General Public License version 2.1 or
later (`LGPL-2.1-or-later`). See the full [license text](LICENSE).
