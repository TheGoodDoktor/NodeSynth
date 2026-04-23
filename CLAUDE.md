# NodeSynth — Claude guidance

## Coding style

All C++ in this project follows **Unreal Engine coding conventions**, applied to a standalone C++ codebase. We adopt the **style**, not the framework — see the caveat below.

### Naming

- **PascalCase** for types, variables, functions, methods, parameters, locals. No `camelCase`, no `snake_case`.
- **Type prefixes:**
  - `F` for plain structs/classes: `FAudioState`, `FNodeGraph`.
  - `E` for enums: `EOscillatorShape`.
  - `I` for interfaces / abstract bases: `INode`.
  - `T` for templates: `TRingBuffer<T>`.
  - No `U` or `A` prefixes — we are not using UObjects or AActors.
- **Booleans** prefix with `b`: `bMuted`, `bIsPlaying`.
- **Acronyms stay uppercase:** `IO`, `URL`, `MIDI`, `DSP`. So `GetID()`, not `GetId()`.

### Formatting

- **Allman braces** — opening brace on its own line for functions, classes, control blocks.
- **Tabs** for indentation (Epic standard, 4-wide display).
- **Space after control keywords:** `if (Cond)`, `for (int32_t I = 0; I < N; ++I)`.
- **Pointer/reference glued to the type:** `float* Samples`, `const std::string& Name`.
- **Always brace conditionals and loops**, even single-statement bodies.
- **`const`** on the left: `const int32_t Count` (west-const).

### Headers

- `#pragma once` (no include guards).
- Include order, each group separated by a blank line:
  1. Own header (for a `.cpp`).
  2. Project headers.
  3. Third-party headers.
  4. System / standard-library headers.
- No `using namespace` in headers.

### Important caveat — style only, not framework

This is a **standalone C++ project**, not a UE plugin or module. Do **not** use any of the following:

- UE types: `FString`, `TArray`, `TSharedPtr`, `TMap`, `int32` typedef, etc.
- UE headers: `CoreMinimal.h`, `Engine.h`.
- UE macros: `UCLASS`, `UPROPERTY`, `UFUNCTION`, `GENERATED_BODY`.

Use standard C++ instead: `std::` containers, `<cstdint>` fixed-width integers (`int32_t`, `uint32_t`), `std::string`, `std::unique_ptr` / `std::shared_ptr`. Apply Unreal naming and bracing **on top of** standard C++.

### Third-party code

External library identifiers (ImGui, GLFW, miniaudio) stay in their upstream style — we don't rename `ma_device` to `FMaDevice` or `glfwInit` to `GlfwInit`. Only our own code follows UE style.
