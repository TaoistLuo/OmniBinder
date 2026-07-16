# Artifact-consumer build mode for the current examples

This directory provides the **second build mode** for the current examples:

- `examples/`: in-tree build, directly using this repository's targets
- `examples/artifact_examples/`: downstream/artifact-consumer build, using installed OmniBinder outputs

It reuses the current `example_cpp/`, `example_c/`, and `examples/*.bidl` files, so there is only one set of example logic to maintain.

## Build steps

First build and install OmniBinder from the repository root:

```bash
cmake -S . -B build
cmake --build build -j4
cmake --install build
```

Then configure the C++ and C downstream projects separately (this directory has no aggregate `CMakeLists.txt`):

```bash
cmake -S examples/artifact_examples/cpp -B build/example_artifacts_cpp \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_cpp -j4

cmake -S examples/artifact_examples/c -B build/example_artifacts_c \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_c -j4
```

Generated executables will be placed under:

```text
build/example_artifacts_cpp/bin/
build/example_artifacts_c/bin/
```

Including:

- `example_cpp_sensor_server`
- `example_cpp_sensor_client`
- `example_c_sensor_server`
- `example_c_sensor_client`

## Purpose

This mode validates that the current examples can also be built like a real downstream project using:

- installed `find_package(OmniBinder)` metadata
- installed `omnic_generate()` support
- the current example sources and IDL files without maintaining a separate example implementation
