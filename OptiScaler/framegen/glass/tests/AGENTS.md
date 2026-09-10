# Standalone module tests

- Read the parent AGENTS.md and README.md before changes.
- These executables use an independent D3D12 device or a fresh test settings directory. Never attach to a game or point the settings test at an installed directory.
- GpuResources.cpp checks real GPU copies, cross-queue completion and timing. Its measured copy duration is not timer overhead or correction performance.
- Settings.cpp includes the production settings implementation with a test-only DLL-path provider. ImGui uses stb fonts in this headless test; production retains FreeType. Vertex generation is not visual acceptance of the game menu.
- All settings test directories must be new. Build artifacts belong outside source control.
- ComputeRecording.cpp checks missing observer coverage, stale Reset tickets and interleaved state rejection. It does not install hooks or prove live observer coverage.

## Document index

- [../README.md](../README.md): Active module design, validation limits and standalone test instructions.
