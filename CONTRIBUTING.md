# Contributing to fcitx5-shadowbar

Thank you for contributing to **fcitx5-shadowbar**!

## Development Workflow

1. Fork [ang-XWBWZ/fcitx5-shadowbar](https://github.com/ang-XWBWZ/fcitx5-shadowbar) and create a topic branch from `main`.
2. Configure and build with tests enabled:

   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
   cmake --build build -j"$(nproc)"
   ctest --test-dir build --output-on-failure -V
   ```

3. Keep changes focused, avoid duplicating logic, and add regression tests in `tests/` whenever modifying core parsing, translation routing, dictionary lookup, clipboard classification, or environment fixing.

## Repository Rules

- **Never commit secrets or user data**: do not commit API keys, personal `config.toml` files, large `.gguf` model weights, `.db`/`.sqlite`/`.ifo`/`.u8` dictionary files, or machine-specific home directory paths.
- **Keep CI & packaging in sync**: if you add a build dependency or source file, update `CMakeLists.txt`, `debian/control`, and `.github/workflows/ci-deb.yml`.
- **Keep docs synchronized**: document user-visible flags, TOML keys, or shortcut changes in `config/config.toml.example`, `README.md`, `README.zh-CN.md`, and `docs/INSTALL*.md`.

## Dual Non-Commercial Licensing Agreement

By submitting a pull request or patch to this project, you agree that your contributions are licensed under the repository's dual non-commercial terms:

- **Source code, build scripts, tests, and packaging (`PolyForm-Noncommercial-1.0.0`)**: [LICENSES/PolyForm-Noncommercial-1.0.0.txt](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
- **Documentation and example configurations (`CC-BY-NC-SA-4.0`)**: [LICENSES/CC-BY-NC-SA-4.0.txt](LICENSES/CC-BY-NC-SA-4.0.txt)
