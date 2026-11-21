# Third-party notices

CppPilot's original source is distributed under the root [MIT License](LICENSE).
Third-party components retain their own copyright notices and licenses.

## nlohmann/json 3.12.0

- Source: https://github.com/nlohmann/json/tree/v3.12.0
- Vendored file: `third_party/nlohmann/json.hpp`
- Copyright: 2013–2025 Niels Lohmann; additional notices remain in the header.
- License: [MIT, original text](third_party/nlohmann/LICENSE.MIT).
- Header SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.

No external dependency download is needed to compile the vendored JSON library.

## Separately installed software

Ollama, model weights, CMake, Python and compilers are not included in this
source distribution. They retain their own licenses. The default model is
[Qwen3 4B Instruct](https://ollama.com/library/qwen3:4b-instruct-2507-q4_K_M);
check its upstream terms before redistribution.

## Architectural references

[Aider](https://github.com/Aider-AI/aider),
[mini-swe-agent](https://github.com/SWE-agent/mini-swe-agent), and
[Continue](https://github.com/continuedev/continue), and
[OpenCode](https://github.com/anomalyco/opencode) informed the workflow.
Their implementation files are not vendored here. Specific references and
adopted ideas are listed in [docs/开源参考.md](docs/开源参考.md).

## User-provided project icon

The files in `assets/` are cutouts and downsampled variants of a user-provided image, not AI-generated artwork. Image rights remain with the original rights holder; the code MIT license does not grant additional image rights. See [assets/README.md](assets/README.md).
