# 当前 examples 的下游构建方式

该目录提供 **第二套构建方式**：

- `examples/`：仓库内（in-tree）构建，直接依赖本仓库 target
- `examples/artifact_examples/`：下游/产物消费式构建，依赖已安装的 OmniBinder 构建产物

它复用了当前 `example_cpp/`、`example_c/` 的源码与 `examples/*.bidl`，不再维护另一套独立示例逻辑。

## 构建方式

先在仓库根目录完成构建和安装：

```bash
cmake -S . -B build
cmake --build build -j4
cmake --install build
```

然后分别构建 C++ 和 C 下游示例（本目录本身没有聚合 `CMakeLists.txt`）：

```bash
cmake -S examples/artifact_examples/cpp -B build/example_artifacts_cpp \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_cpp -j4

cmake -S examples/artifact_examples/c -B build/example_artifacts_c \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_c -j4
```

生成的可执行文件位于：

```text
build/example_artifacts_cpp/bin/
build/example_artifacts_c/bin/
```

包含：

- `example_cpp_sensor_server`
- `example_cpp_sensor_client`
- `example_c_sensor_server`
- `example_c_sensor_client`

## 说明

该模式适合验证：

- 安装产物中的 `find_package(OmniBinder)`
- 安装产物中的 `omnic_generate()`
- 当前 examples 在“真实下游工程模式”下能否独立构建
