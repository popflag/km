# km

[English](README.md) | [中文](README_zh.md)

`km` 是一个使用原生 C17 编写、采用 Emacs 风格命令的终端文本编辑器。项目重点是
在 POSIX 和 Windows 上提供可靠的 UTF-8 编辑、可预测的终端渲染与安全的文件更新。

项目当前已经可以使用，但仍处于开发阶段。它不提供 GNU Emacs Lisp 运行时，也不以
完整兼容 GNU Emacs 为目标。

## 功能

- 使用 vendored `utf8proc` 进行 UTF-8 校验与 Unicode 终端布局。
- 支持字素簇渲染、CJK 与 Tab cell 宽度、软换行、行号和区域高亮。
- 支持多 Buffer、等高 Window 和 Emacs 风格 minibuffer。
- 支持 mark/region 编辑、kill ring、撤销/重做、增量搜索和字面量 query replace。
- 支持矩形标记、剪切和粘贴，并处理 Tab、宽字符与短行。
- 安全替换文件，检查外部修改，并保留 UTF-8 BOM 和统一的换行格式。
- 提供原生 POSIX 终端和 Windows Console 后端。

## 构建

构建只需要一个支持 C17 的编译器。编辑器需要的依赖已经包含在仓库中。

### POSIX

```sh
cc -std=c17 nob.c -o nob
./nob build
./build/km [file]
```

支持 GCC 和 Clang。

### Windows

在 MSVC Developer Command Prompt 中运行：

```bat
cl /nologo /std:c17 nob.c /Fe:nob.exe
nob.exe build
build\km.exe [file]
```

不指定文件时，`km` 会打开 scratch Buffer。启动时可以传入一个文件路径；需要打开
更多文件时使用 `C-x C-f`。

## 快捷键

`C-` 表示 Control，`M-` 表示 Alt/Meta。编辑器也支持通过 `C-u` 以及
`M--`/`M-0` 到 `M-9` 输入数字前缀参数。

| 操作 | 快捷键 |
| --- | --- |
| 打开文件 | `C-x C-f` |
| 保存 Buffer | `C-x C-s` |
| 切换 Buffer | `C-x b` |
| 列出 Buffer | `C-x C-b` |
| 关闭 Buffer | `C-x k` |
| 退出 | `C-x C-c` |
| 执行命名命令 | `M-x` |
| 设置 mark | `C-SPC` |
| 剪切/复制 region | `C-w` / `M-w` |
| 粘贴/轮换 kill ring | `C-y` / `M-y` |
| 撤销/重做 | `C-/` 或 `C-x u` / `C-x C-r` |
| 向前/向后搜索 | `C-s` / `C-r` |
| 交互替换 | `M-%` |
| 分割/选择 Window | `C-x 2` / `C-x o` |
| 删除当前/其他 Window | `C-x 0` / `C-x 1` |
| 矩形标记模式 | `C-x SPC` |
| 剪切/粘贴矩形 | `C-x r k` / `C-x r y` |
| 取消当前操作 | `C-g` |

同时支持 `C-f`、`C-b`、`C-n`、`C-p`、`M-f`、`M-b`、`C-v`、`M-v` 等
标准 Emacs 移动快捷键，也支持方向键、Home、End、Delete 和 Backspace。

执行 query replace 时，使用 `y` 替换、`n` 跳过、`!` 替换剩余全部匹配，使用
`q` 或 `C-g` 停止。

## 配置

编译期偏好位于 [`config.h`](config.h)，包括终端样式、Tab 与宽度策略、行号、
ring 容量、输入限制和自定义快捷键。

修改后重新构建编辑器：

```sh
./nob build
```

当前不提供运行时配置解析或重载机制。

## 验证

```sh
./nob test
./nob sanitize
./nob bench-layout
```

`sanitize` 需要 GCC 或 Clang。如果 LeakSanitizer 在限制 ptrace 的环境中无法启动，
可以运行：

```sh
ASAN_OPTIONS=detect_leaks=0 ./nob sanitize
```

CI 使用 GCC、Clang 和 MSVC 运行测试套件。

## 当前限制

- 仅支持 UTF-8 文本。非法 UTF-8 与混合换行会被拒绝，不会静默转换。
- 搜索和 query replace 只支持区分大小写的字面量，不支持正则表达式。
- 仅支持水平等高 Window 分割和核心矩形命令。
- 不提供 Elisp 运行时、package 生态、GUI、双向文本、shaping engine 或完整的
  IME preedit UI。
- 当前兼容性实验以 GNU Emacs 31.0.90 开发版本为参考；兼容范围仅限已记录的命令。

设计基线和详细行为契约参见
[`docs/architecture-research.md`](docs/architecture-research.md)。
