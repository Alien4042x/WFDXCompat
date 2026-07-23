# WineForge DXCompat Provenance

WineForge DXCompat is an independent interoperability implementation. Its
source was developed from:

- public Direct3D and COM API definitions;
- public headers supplied by llvm-mingw;
- application logs and documented HRESULT behavior;
- focused test programs that call public Direct3D interfaces;
- black-box interoperability testing against a separately supplied graphics
  backend; and
- Windows application and Unreal Engine diagnostic symbols used to identify
  the public API contract required by the application.

The implementation provides its own COM objects, resource lifetime handling,
format translation, barriers, and copy behavior. Functional observations are
verified with standalone probes and fake-backend tests before application
testing.

This repository does not contain:

- Apple source code or private Apple headers;
- decompiled or translated Apple implementation code;
- Apple frameworks, libraries, binaries, symbols, or assets;
- modified Apple runtime files; or
- source copied from the selected graphics backend.

The selected graphics backend is not part of WineForge DXCompat. It must be
obtained and used separately under its own applicable terms. WineForge
DXCompat communicates with it only through the public Direct3D ABI exposed to
Windows applications.

The Windows application PDB analysis referenced in the architecture document
concerns the application's Unreal Engine media path. It is not an analysis or
disassembly of Apple D3DMetal implementation code.
