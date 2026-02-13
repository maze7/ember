# Ember

Ember is a C++20 game framework built as a static library. It provides a lightweight application scaffold with a fixed-timestep game loop, 2D sprite batching, an ECS powered by [EnTT](https://github.com/skypjack/entt), unified input handling (keyboard, mouse, and gamepad), asset management with hot-reloading, and an integrated [Dear ImGui](https://github.com/ocornut/imgui) debug UI — all on top of [SDL3](https://github.com/libsdl-org/SDL)'s GPU API.

## Features

- **Application lifecycle** — Subclass `Application` and override a handful of callbacks (`init`, `update_fixed`, `update_variable`, `render`, `imgui`, `cleanup`). Ember manages the window, input polling, fixed/variable timestep loop, and frame presentation.
- **Graphics** — An abstract `RenderDevice` with an SDL3 GPU backend handles textures, shaders, buffers, and render targets. A built-in `Batcher` provides batched 2D drawing (quads, rects, lines, images/sprites) with push/pop stacks for matrices, blend modes, scissor rects, samplers, and materials.
- **Entity Component System** — Thin wrapper around EnTT exposing `Scene` and `Entity` classes with ergonomic `add`, `get`, `has`, `remove`, and `view` helpers.
- **Input** — Unified `Input` system tracking keyboard, mouse, and up to 4 game controllers across frames. `VirtualAxis` and `VirtualStick` let you bind multiple physical inputs to a single logical axis with configurable overlap behaviour.
- **Asset management** — Type-safe `AssetManager` with templated `AssetCollection`s, lazy loading, preloading, and filesystem-watch-based hot-reloading.
- **Debug UI** — Integrated Dear ImGui and ImPlot rendering, with a dedicated `imgui()` callback on the application class.
- **Logging** — Thread-safe, coloured console logging via `EMBER_TRACE`, `EMBER_INFO`, `EMBER_WARN`, and `EMBER_ERROR` macros (powered by [{fmt}](https://github.com/fmtlib/fmt)).
- **Steam Deck support** — Optional `EMBER_PLATFORM_STEAM_DECK` compile flag.

## Dependencies

All dependencies are vendored under `lib/` and built from source via CMake:

| Library | Purpose |
|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Windowing, input, GPU rendering backend |
| [SDL_shadercross](https://github.com/libsdl-org/SDL_shadercross) | Cross-platform shader compilation (SPIRV-Cross) |
| [EnTT](https://github.com/skypjack/entt) | Entity Component System |
| [GLM](https://github.com/g-truc/glm) | Math (vectors, matrices, transforms) |
| [{fmt}](https://github.com/fmtlib/fmt) | String formatting / logging |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing |
| [Dear ImGui](https://github.com/ocornut/imgui) | Debug UI |
| [ImPlot](https://github.com/epezent/implot) | Plot widgets for ImGui |
| [stb_image](https://github.com/nothings/stb) | Image loading |
| [efsw](https://github.com/SpartanJ/efsw) | Filesystem watcher (asset hot-reload) |

## Building

Ember uses **CMake** (≥ 3.5) and requires a **C++20** compiler.

```bash
# Clone the repository (with submodules if applicable)
git clone https://github.com/<you>/ember.git
cd ember

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### CMake Options

| Option | Default | Description |
|---|---|---|
| `EMBER_USE_SDL3` | `ON` | Build with the SDL3 platform/graphics backend |
| `EMBER_PLATFORM_STEAM_DECK` | `OFF` | Enable Steam Deck–specific configuration |

### Build Configurations

| Config | Preprocessor Define | Optimisation |
|---|---|---|
| Debug | `EMBER_DEBUG` | `-O1` |
| Profile | `EMBER_PROFILE` | `-O2` |
| Release | `EMBER_RELEASE` | `-O2` |

## Usage

Ember is designed to be consumed as a static library by a game project. Add it as a CMake subdirectory and link against the `Ember` target:

```cmake
add_subdirectory(path/to/ember)
target_link_libraries(MyGame PRIVATE ${EMBER_LIBS})
target_include_directories(MyGame PRIVATE ${EMBER_INCLUDE_DIRS})
```

Then subclass `Ember::Application`:

```cpp
#include "platform/application.h"

class MyGame : public Ember::Application
{
public:
    bool init() override
    {
        // Load assets, set up scenes, etc.
        return true;
    }

    void update_fixed(double dt) override
    {
        // Physics, AI, networking — called at a fixed rate (default 60 Hz)
    }

    void update_variable(double dt, double accumulator) override
    {
        // Per-frame logic (animation, interpolation)
    }

    void render() override
    {
        // Draw your game using m_batcher, RenderDevice, etc.
    }

    void imgui() override
    {
        // Draw debug UI with ImGui calls
    }
};

int main()
{
    MyGame game;
    game.run();
    return 0;
}
```

## Project Structure

```
src/
├── assets/        # Asset management & hot-reload
├── core/          # Common types, logging, handles, UUIDs, time
├── ecs/           # Entity Component System (Scene, Entity)
├── graphics/      # RenderDevice, Batcher, textures, shaders, targets
├── input/         # Keyboard, mouse, controller, virtual axes
├── math/          # Rects, quads, math helpers
└── platform/      # Application lifecycle, window management
lib/               # Vendored third-party dependencies
```

