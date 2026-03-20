# OmniBinder ARM 交叉编译指南

## 1. 文档目标

本文档说明如何将 OmniBinder 交叉编译到嵌入式 ARM Linux 目标环境，并说明哪些产物应该在主机上构建，哪些产物应该交叉编译到目标板上。

本文档适用于：

- `arm-linux-gnueabihf`
- `aarch64-linux-gnu`
- 其他 GNU/Linux ARM 交叉工具链（只需调整 toolchain 文件中的编译器前缀）

## 2. 先明确 Host 与 Target 的分工

### 2.1 Host 侧（开发机）构建的内容

这些工具通常运行在开发机上，不应依赖目标板：

- `omni-idlc`
- 可选：主机版 `omni-cli`

推荐用途：

- 在 PC 上将 `.bidl` 生成 `.h/.cpp`
- 在 PC 上直接调试远端板子上的服务

### 2.2 Target 侧（ARM 板）交叉编译的内容

这些产物会在 ARM 板上运行，应使用交叉工具链构建：

- `libomnibinder.so`
- `libomnibinder.a`
- `service_manager`
- 你的业务服务进程
- examples
- 可选：目标板本机使用的 `omni-cli`

### 2.3 一句话总结

通常可以这样理解：

- `omni-idlc`：主机构建
- 其余运行在板子上的 runtime / service / manager：交叉编译
- `omni-cli`：取决于你希望在哪台机器上运行它

## 3. 当前项目的构建特点

当前顶层 CMake 统一控制这些内容：

- `OMNIBINDER_BUILD_TESTS`
- `OMNIBINDER_BUILD_EXAMPLES`
- `OMNIBINDER_BUILD_TOOLS`

这意味着如果直接拿 ARM toolchain 去构建整个项目：

- `omni-idlc` 也会被编译成 ARM 版本
- 这通常对开发机上的代码生成流程没有意义

因此，推荐采用**两阶段构建**：

1. 主机构建 host tools
2. 交叉编译 target runtime

## 4. 推荐目录与构建方式

### 4.1 Host 构建目录

建议使用：

```bash
build-host/
```

### 4.2 ARM 交叉编译目录

建议使用：

```bash
build-arm/
```

这样能明确区分：

- 哪些二进制是给主机运行的
- 哪些二进制是给目标板运行的

## 5. Toolchain 文件

项目中已新增一个示例 toolchain 文件：

- `cmake/toolchains/arm-linux-gnueabihf.cmake`

它默认使用：

- `arm-linux-gnueabihf-gcc`
- `arm-linux-gnueabihf-g++`

工具链前缀不同的情况下，例如：

- `aarch64-linux-gnu-`
- `arm-none-linux-gnueabihf-`

只需要修改其中的：

```cmake
set(TOOLCHAIN_PREFIX ...)
```

如果存在 sysroot，也可以在 toolchain 文件中设置：

```cmake
set(CMAKE_SYSROOT /path/to/sysroot)
```

## 5.1 交叉编译真正需要哪些变量

最小建议集如下：

```bash
export TOOLCHAIN_ROOT=/usr/local/arm_linux_4.8/bin

export PATH="${TOOLCHAIN_ROOT}:$PATH"
export CC="${TOOLCHAIN_ROOT}/arm-linux-gcc"
export CXX="${TOOLCHAIN_ROOT}/arm-linux-g++"

# 强烈建议补齐，避免静态库/链接/符号工具误用主机版本
export AR="${TOOLCHAIN_ROOT}/arm-linux-ar"
export RANLIB="${TOOLCHAIN_ROOT}/arm-linux-ranlib"
export STRIP="${TOOLCHAIN_ROOT}/arm-linux-strip"
export LD="${TOOLCHAIN_ROOT}/arm-linux-ld"
```

如果你的 sysroot 与第三方依赖通过 `pkg-config` 暴露，还应补：

```bash
export PKG_CONFIG_SYSROOT_DIR=/path/to/sysroot
export PKG_CONFIG_PATH=/path/to/sysroot/usr/lib/pkgconfig:/path/to/sysroot/usr/share/pkgconfig
```

如果工具链本身已经把 `--sysroot` 编进 `CC/CXX`，上面的变量通常已经够用；如果没有，更推荐通过 `CMAKE_TOOLCHAIN_FILE` 或 `CMAKE_SYSROOT` 传给 CMake。

对当前项目而言，交叉编译的最终约束是：

- host stage 使用主机编译器，并产出 `bin_host/omni-idlc`
- cross stage 使用交叉编译器，并产出 `bin_cross/*` 运行时程序
- cross stage 至少要明确提供 `CC`、`CXX`
- 为避免工具落回主机版本，建议同时提供 `AR`、`RANLIB`、`STRIP`、`LD`
- 如果依赖 sysroot 下的 `.pc` 文件，再提供 `PKG_CONFIG_SYSROOT_DIR` 与 `PKG_CONFIG_PATH`
- 若工具链约束较复杂，优先使用 `CMAKE_TOOLCHAIN_FILE`
## 6. 主机构建 omni-idlc

如果只在开发机生成 IDL 代码，推荐这样构建：

```bash
mkdir -p build-host
cd build-host

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../install \
  -DOMNIBINDER_BUILD_TESTS=OFF \
  -DOMNIBINDER_BUILD_EXAMPLES=OFF

cmake --build . -j$(nproc)
cmake --install .
```

构建完成后，主机版 `omni-idlc` 通常位于：

```bash
build-host/target/bin/omni-idlc
# 安装后位于 build-host/install/bin_host/omni-idlc
```

使用示例：

```bash
cd examples
../build-host/target/bin/omni-idlc --lang=cpp --output=/tmp/omni-idlc_out sensor_service.bidl
```

## 7. ARM 交叉编译运行时

ARM 运行时阶段的有效输入是：

- 一组有效的交叉工具链变量，或
- 一个有效的 `CMAKE_TOOLCHAIN_FILE`

典型命令形式如下：

```bash
mkdir -p build-arm
cd build-arm

cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../install \
  -DOMNIBINDER_BUILD_TESTS=OFF

cmake --build . -j$(nproc)
cmake --install .
```

构建完成后，ARM 目标产物通常位于：

- `build-arm/target/lib/libomnibinder.so`
- `build-arm/target/lib/libomnibinder.a`
- `build-arm/target/bin/service_manager`
- `build-arm/target/example/...`
- 可选：`build-arm/target/bin/omni-cli`

安装后目录建议理解为：

- `install/bin_host/`：主机可执行工具，至少包含 `omni-idlc`
- `install/bin_cross/`：目标板可执行文件，如 `service_manager`、`omni-cli`
- `install/lib/`：库与 CMake package

## 8. 只构建最小运行时的建议

如果目标板只需要最小运行集，可以考虑：

```bash
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DOMNIBINDER_BUILD_TESTS=OFF \
  -DOMNIBINDER_BUILD_EXAMPLES=OFF
```

如果目标板侧也不需要工具，可进一步考虑：

```bash
  -DOMNIBINDER_BUILD_TOOLS=OFF
```

这样会减少目标板侧的构建时间和部署体积。

## 9. 交叉编译推荐流程

双阶段构建的最终状态如下：

### 方案 A：手工双阶段构建

```bash
mkdir -p build-host build-arm

cd build-host
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../install \
  -DOMNIBINDER_BUILD_TESTS=OFF \
  -DOMNIBINDER_BUILD_EXAMPLES=OFF
cmake --build . -j$(nproc)
cmake --install .

cd ../build-arm
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../install \
  -DOMNIBINDER_BUILD_TESTS=OFF \
  -DOMNIBINDER_HOST_IDLC=../install/bin_host/omni-idlc
cmake --build . -j$(nproc)
cmake --install .
```

### 方案 B：使用仓库内置脚本

```bash
TOOLCHAIN_FILE=/path/to/toolchain.cmake ./scripts/build_dual_stage_install.sh
```

或者：

```bash
CROSS_CC=/path/to/target-gcc \
CROSS_CXX=/path/to/target-g++ \
CROSS_AR=/path/to/target-ar \
CROSS_RANLIB=/path/to/target-ranlib \
CROSS_STRIP=/path/to/target-strip \
CROSS_LD=/path/to/target-ld \
CROSS_PKG_CONFIG_SYSROOT_DIR=/path/to/sysroot \
CROSS_PKG_CONFIG_PATH=/path/to/sysroot/usr/lib/pkgconfig:/path/to/sysroot/usr/share/pkgconfig \
./scripts/build_dual_stage_install.sh
```

这个脚本的最终产出要求是：

- 产出一个同时包含 `bin_host/` 和 `bin_cross/` 的发布安装目录
- `bin_host/` 中必须包含可在主机执行的 `omni-idlc`
- `bin_cross/` 中包含目标板部署所需程序

它不是“主机下游业务工程”的默认 `find_package()` 输入目录。

注意：

- host stage 必须在**干净的主机环境**里执行；
- 如果一开始就把 `CC` / `CXX` 指到了交叉编译器，host stage 会被识别为 cross context；
- 这种情况下不会生成 `install/bin_host/omni-idlc`，dual-stage 流程会直接失效。

### 步骤 1：在主机上生成 IDL 代码

```bash
cd examples
../install/bin_host/omni-idlc --lang=cpp --output=generated sensor_service.bidl
```

### 步骤 2：将生成代码纳入业务工程

把 `.h/.cpp` 生成文件编进你的 ARM 服务工程。

### 步骤 3：在 ARM toolchain 下构建 runtime 与服务

```bash
cd build-arm
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/arm-linux-gnueabihf.cmake ...
cmake --build . -j$(nproc)
```

### 步骤 4：部署到目标板

部署这些文件：

- `service_manager`
- `libomnibinder.so` 或静态链接产物
- 你的服务程序
- 可选：`omni-cli`

## 10. 交叉编译时的系统要求

目标系统需要具备这些能力：

- Linux
- `epoll`
- Unix Domain Socket
- `SCM_RIGHTS`
- `eventfd`
- POSIX SHM

如果目标系统裁掉了这些能力，则当前 SHM 路径和部分控制路径无法正常工作。

## 11. 是否需要交叉编译测试

一般不建议把完整测试套件都交叉编译到 ARM 目标板上运行。

更推荐：

- 在 x86 开发机上跑完整测试
- 在 ARM 目标板上做 smoke test 和关键链路验证

### 推荐的 ARM 板上验证项

- `service_manager` 能正常启动
- 一个本地 service 能正常注册
- 一个 client 能正常 `lookupService()`
- 一个简单的 RPC 调用能成功返回
- 一个 topic 能成功发布与订阅

## 12. omni-cli 是否要交叉编译

这取决于使用方式：

### 12.1 不需要交叉编译的情况

在开发机上直接连接远端板子调试服务时：

- 主机版 `omni-cli` 就够了

### 12.2 需要交叉编译的情况

在 ARM 板子本机执行时：

- `omni-cli list`
- `omni-cli info`
- `omni-cli call`

那就需要把 `omni-cli` 一并交叉编译到目标板。

## 13. 推荐实践

基于当前工程结构，推荐采用：

- `build-host/`：主机构建并安装 `install/bin_host/`
- `build-arm/`：目标板交叉编译并安装 `install/bin_cross/`

也就是：

- **Host Tools 与 Target Runtime 分离构建**

这种方式最容易区分主机工具与目标运行时的职责边界。

## 14. 最推荐的一组命令

### 14.1 只构建主机版本

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build
```

此时不需要交叉编译，安装目录只需要关注：

- `install/bin_host/`
- `install/include/`
- `install/lib/`

### 14.2 做交叉编译时的推荐命令

```bash
./scripts/build_dual_stage_install.sh
```

## 15. 结论

OmniBinder 在 ARM 交叉编译上的推荐方式是：

- `omni-idlc` 主机构建
- runtime / `service_manager` / 业务服务 / examples 交叉编译
- `omni-cli` 是否交叉编译，取决于你要在主机还是目标板运行它

- **除了 `omni-idlc` 这类 Host Tool 之外，其余运行在 ARM 板上的内容都应交叉编译**
