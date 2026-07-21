# 类 GNU Emacs 文本编辑器架构调研记录

> 状态：架构基线；Phase 0/1 已完成，Phase 2/3 的单 View 多 Buffer TUI 与
> Phase 4 的安全保存纵切已实现
> 调研日期：2026-07-20（UTC-08:00）
> 目标语言：C17。原需求中的“C20”不是 ISO C 标准版本；若实际指 C++20，需要重新评估本文的类型、构建和插件 ABI 结论。
> 决策标记：`采用`、`拒绝`、`待验证`、`未来`。

## 1. 目标与范围

项目目标是实现一个原生 C 文本编辑器，交互方式尽可能兼容 GNU Emacs，具备可靠的 UTF-8/Unicode 支持、region、矩形编辑和跨行批量编辑，并同时支持 POSIX 与原生 Windows。

“完全复刻 GNU Emacs”不能直接作为验收条件。GNU Emacs 的大量行为来自 Elisp，包括 keymap、major/minor mode、hook、search、rectangle 命令和部分 undo 逻辑。本项目明确不实现 Elisp，因此可执行的兼容定义应为：

> 在一个固定的 GNU Emacs 版本、固定的终端能力集合和明确列出的命令集合上，对可观察状态进行差分测试。

每个兼容用例至少记录：

- 初始 UTF-8 文本、point、mark、narrowing、prefix argument。
- 输入的规范化 key sequence 或直接调用的 command。
- 最终文本、point、mark、region active、kill ring、modified 状态和消息。
- undo/redo 后的相同状态。

明确不在第一版承诺：

- Elisp 运行时、现有 Emacs package 生态和任意 Lisp 可观察内部状态。
- GUI frame、daemon/client、TRAMP、字体管理。
- Unicode bidi、OpenType shaping 和完整 IME preedit UI。
- 非 UTF-8 编码的自动探测与无损编辑。
- 二进制插件 ABI、插件热卸载和不可信插件隔离。
- 所有历史终端、旧 Windows Console 和任意 `$TERM` 的兼容。

### 1.1 当前可运行纵切

当前 `km [file]` 实现一个单 Frame/单 View、多 Buffer 的终端编辑器：

- 严格 UTF-8 文件加载，保留 UTF-8 BOM 和统一 LF/CRLF/CR policy；非法
  UTF-8、mixed EOL、symlink/reparse point 和多 hard-link 目标拒绝写入。
- `C-x C-s` 使用同目录排他临时文件、数据 flush 和原子 replace；保存前
  检查文件 identity，成功后才更新 `saved_state_id`。
- 支持 code-point 左右移动/删除、按 cell column 的上下移动、行首/行尾、
  mark/region、`C-w`、`M-w`、`C-k`、`C-y`、undo/redo 和大小写敏感的
  UTF-8 增量 `C-s`。
- `C-x C-f` 通过 UTF-8 minibuffer 打开或创建文件；`C-x b` 按完整显示名
  切换 Buffer，空输入循环到下一个；`C-x k` 关闭当前 Buffer。关闭最后一个
  Buffer 会立即创建新的 `*scratch*`。
- dirty Buffer 的 `C-x k` 和存在任意 dirty Buffer 时的 `C-x C-c` 使用显式
  y/n 确认；`C-g` 取消。重复访问现有文件时按平台 file identity 复用
  Buffer；同名但不同目标使用 `<2>` 起的唯一 Buffer 名。
- `M-x` 可执行当前静态 registry 中的编辑命令，以及 `find-file`、
  `switch-to-buffer`、`kill-buffer`、`save-buffer` 和
  `save-buffers-kill-terminal`；后者先保存全部 modified visited Buffer，任一
  保存失败保持会话打开。当前尚无补全、历史或动态命令注册。
- 当前 kill ring 只有一个 entry；kill coalescing、`yank-pop` 和完整 Emacs
  undo 遍历仍需差分测试后扩展。
- POSIX 输入识别 ESC Meta、常见 CSI/SS3 方向键和 bracketed paste；Win32
  record 输入识别对应 named keys，但按平台限制不声称有 paste boundary。
- Renderer 当前仍执行完整 CellGrid frame 输出；front/back row diff 是下一
  个纯性能步骤，不影响上述编辑和数据安全契约。

当前纵切不等于“完整 GNU Emacs”。多 Window、minibuffer 补全/历史、
rectangle、完整 search/replace、keyboard macro、mode/keymap 扩展和插件仍按
后续 Phase 分别冻结行为与实现。

尚待产品层冻结的两项：

1. 兼容目标究竟是哪个 GNU Emacs 正式版本。
2. “多行编辑”是否只指普通跨行 region 和 rectangle，还是还要求多个独立 cursor/selection。后者不是 GNU Emacs 核心能力。

## 2. 调研方法

### 2.1 证据优先级

本记录按以下顺序采用证据：

1. Unicode、POSIX、Microsoft 等规范或官方 API 文档。
2. 固定 commit/tag 的 GNU Emacs 与 `utf8proc` 源码。
3. GNU Emacs 官方用户手册和 Elisp Reference Manual。
4. 本机 GNU Emacs 的最小行为实验。
5. 文本数据结构原始论文。
6. 博客和论坛仅用于发现问题，不用于冻结契约。

### 2.2 本机行为实验

实验环境为 `GNU Emacs 31.0.90` 开发版，因此实验只用于发现和交叉验证，不能替代最终选定的正式兼容版本。

#### 组合字符的 point 与 column

文本为 `e + U+0301 COMBINING ACUTE ACCENT + x`。从 `point-min` 连续执行 `forward-char`，记录 `(point, current-column, char-after)`：

```text
((1 0 101) (2 1 769) (3 1 120) (4 2 nil))
```

结论：Emacs 的逻辑 point 可以位于一个 extended grapheme cluster（EGC）内部；`forward-char` 按 code point 移动，组合字符前后可能映射到同一个终端列。

在 point 3 执行一次 `delete-backward-char`：

```text
("ex" 2)
```

结论：默认 Emacs 删除命令删除一个 code point，而不是整个 EGC。为了 Emacs 兼容，不能把默认字符命令悄悄改成 grapheme 原子。

#### Buffer、Window 与 indirect buffer

同一 Buffer 显示在两个 Window 中时，两个 window point 可以分别为 2 和 5；在 Buffer 上 narrow 到 `[2,5)` 后，两个 Window 观察到相同的 `(point-min, point-max) = (2,5)`。

Base buffer narrow 到 `[2,5)` 后，其 indirect buffer 仍观察到完整范围 `[1,7)`。两者的文本和 `buffer-undo-list` 为共享对象。

结论：需要三层所有权，而不是简单的“一个 Document 对多个完全独立 View”：

- 共享文本与 undo 历史。
- 每个 Emacs Buffer 有自己的 narrowing、mark 和保存 point。
- 每个 Window/View 有自己的 window point、scroll start 和布局缓存。

### 2.3 调研中的冲突与纠偏

| 初始建议或常见假设                   | 证据或推敲                                                           | 最终结论                                                 |
|--------------------------------------|----------------------------------------------------------------------|----------------------------------------------------------|
| 所有移动和删除都按 EGC               | Emacs 实验表明 `forward-char` 和删除按 code point                    | 默认 Emacs 命令按 code point；EGC 负责渲染、宽度和命中   |
| narrowing 属于 View                  | 同一 Buffer 的多个 Window 共享 narrowing；indirect Buffer 才独立     | narrowing 属于 Buffer                                    |
| 多点 batch 应按原坐标倒序执行        | 倒序插入会让 gap 反复跨过刚插入的 payload                            | 按原坐标升序执行，并用累计 byte delta 转换当前坐标       |
| 首版做 branching undo tree           | 不是 Emacs 最小兼容路径，也没有当前产品需求                          | 使用线性 transaction journal，具体 Emacs undo 遍历待差分 |
| 同时依赖 `utf8proc` 与 `libgrapheme` | `utf8proc` 已提供严格解码、UAX #29 stateful grapheme、属性和宽度基线 | 只锁一个 `utf8proc` 版本                                 |
| 现在抽象 TextStore 以便未来换 rope   | 当前只有一个实现，会形成单实现 vtable                                | 使用 opaque 的具体 gap 实现；API 不泄漏物理指针即可      |
| POSIX 和 Windows 路径都存 UTF-8      | POSIX 文件名是字节序列；Windows 原生路径是 UTF-16 code units         | Path 是平台层 opaque 对象；文档内容仍固定 UTF-8          |
| `ReadConsoleInputW` 能识别粘贴       | Console records 没有结构化 Paste 边界                                | Win32 record 模式不能保证一次粘贴等于一个 transaction    |

## 3. 决策摘要

| 编号 | 状态   | 决策                                                            |
|------|--------|-----------------------------------------------------------------|
| A01  | 采用   | C17、64 位平台优先，`nob.h` 构建，GCC/Clang/MSVC                |
| A02  | 采用   | 固定 Emacs 版本和命令矩阵上的行为兼容                           |
| A03  | 采用   | `Document -> Buffer -> View` 三层所有权                         |
| A04  | 采用   | Document 内部使用 UTF-8 byte gap buffer                         |
| A05  | 采用   | 持久位置统一为 UTF-8 byte offset，且必须在 code point boundary  |
| A06  | 采用   | 原子 splice、registered anchors、命令级 transaction             |
| A07  | 采用   | 线性 undo journal；不做 branching tree                          |
| A08  | 采用   | vendoring `utf8proc v2.11.3`，Unicode 17.0.0                    |
| A09  | 采用   | CellGrid + row diff；不依赖 ncurses                             |
| A10  | 采用   | POSIX `termios + poll + VT`                                     |
| A11  | 采用   | Windows VT 输出 + `ReadConsoleInputW` 结构化输入                |
| A12  | 采用   | 同目录临时文件 + flush + replace 的同步安全保存                 |
| A13  | 拒绝   | 首版 rope、piece tree、通用存储 factory                         |
| A14  | 拒绝   | ICU、系统 `wcwidth()` 作为统一 Unicode 方案                     |
| A15  | 拒绝   | ConPTY 作为本地编辑器终端后端                                   |
| A16  | 拒绝   | 首版线程池、原生 file watcher、VFS、通用事件总线                |
| A17  | 未来   | 稳定 command/transaction API 后再设计 C 插件 ABI                |
| A18  | 待验证 | rectangle 在 tab、组合字符和宽 EGC 边缘的逐命令 Emacs 语义      |
| A19  | 待验证 | GNU Emacs `undo`、`undo-only`、`undo-redo` 的精确遍历与合并行为 |
| A20  | 待验证 | Windows VT input 下 bracketed paste、鼠标、resize 和修饰键组合  |
| A21  | 采用   | QEmacs 作为大文件/mode/TTY 参考，不改变首版 gap/transaction/CellGrid；详见 [QEmacs 专项调研](qemacs-architecture-research.md) |

## 4. 总体架构

```text
Platform bytes / Win32 INPUT_RECORD
                 |
                 v
        Input decoder/backend
                 |
                 v
        Normalized KmEvent queue
                 |
                 v
   Key resolver -> Command loop -> Command transaction
                                  |
                                  v
Editor -> Document -> Buffer(s) -> View(s)
             |                       |
             |                       v
             |                 Unicode layout
             |                       |
             v                       v
      gap + anchors + undo       back CellGrid
                                      |
                                      v
                              front/back row diff
                                      |
                                      v
                              VT output backend
```

依赖方向必须单向：

```text
base <- document/text <- editor/commands <- layout/render <- app/platform
     <- file/path -----^
```

`file/path` 是低于 Buffer 的窄服务：公开类型保持 opaque，平台实现分别持有
POSIX raw path 或 Windows UTF-16 path/identity。Buffer 可以拥有并调用该服务，
但核心编辑代码仍不知道 `termios`、Win32 handle、VT sequence 或具体文件系统
API。平台终端代码不直接修改 Document，只产生事件或返回显式结果。

首版为单线程。Editor、Document、Buffer、View、undo 和 command callback 全部只由主线程访问。不建立锁、worker pool 或跨线程 observer。

## 5. 对象与所有权

### 5.1 Document

`KmDocument` 是共享文本事实，拥有：

- 一个具体的 `KmGap`。
- 单调递增的 `revision`，用于 cache 失效和过期结果检测。
- 当前 `history_state_id`，用于判断是否回到保存状态。
- 全部 registered anchor。
- 共享线性 undo journal。
- 共享 modified/history 状态。文件访问 metadata 由唯一 base Buffer 持有，其中 `saved_state_id` 指向 Document history state。

`revision` 和 `history_state_id` 不能合并：undo/redo 也会产生新的 revision，但当 undo 回到保存节点时，`history_state_id == saved_state_id`，modified 状态应恢复为 false。

### 5.2 Buffer

`KmBuffer` 对应 Emacs 语义中的 buffer facade，拥有：

- 指向 `KmDocument` 的引用。
- Buffer 名称。
- narrowing anchors：`begv`、`zv`。
- mark、mark active、保存 point。
- read-only 和未来 mode-local 状态。
- base/indirect 类型和指向 base Buffer 的关系。
- 仅 base Buffer 可选拥有 visited-file metadata：opaque path、BOM、EOL style、文件身份和 `saved_state_id`。

普通多个 View 指向同一个 Buffer，因此共享 narrowing 和 mark。Indirect buffer 是另一个 `KmBuffer`，指向同一个 `KmDocument`，但拥有独立 narrowing、mark 和保存 point。Indirect buffer 不暴露 visited filename，v1 的 `save-buffer` 在 indirect buffer 中报错并要求切换到 base Buffer；不能因为共享 Document 就隐式覆盖 base 文件。Base/indirect 的 modified 状态仍由共享 history state 得出。

### 5.3 View

`KmView` 对应 Emacs Window，拥有：

- 当前显示的 `KmBuffer *`。
- window-local point anchor。
- scroll/start anchor。
- preferred cell column。
- viewport 尺寸、水平滚动和 wrap 设置。
- 当前可见 hard lines 的 EGC/layout cache。
- front/back CellGrid 或指向所属 frame grid 的区域。

Buffer 不被显示时使用 `saved_point`。View 第一次显示 Buffer 时从 saved point 初始化；窗口切换和 Buffer 切换的精确保存规则进入 Emacs 差分测试。

当前单 View 应用层 registry 为每个 Buffer 保存一个 visual `scroll_row`，切换
时同时恢复该 Buffer 的 point 和 scroll。它是现有 layout 显式接收 scroll
参数下的最小实现；增加多 Window 时 scroll/start anchor 必须迁回各 View，
不能继续挂在 Buffer registry 上。

### 5.4 最小 C 结构草图

这些结构放在私有头文件中。公共头只前置声明 opaque 类型。

```c
typedef struct { size_t v; } KmBytePos;
typedef struct { size_t v; } KmCellCol;
typedef uint64_t KmRevision;
typedef uint64_t KmStateId;
typedef uint64_t KmAnchorId;

typedef struct {
    uint8_t *mem;
    size_t cap;
    size_t gap_lo;
    size_t gap_hi;
} KmGap;

typedef enum {
    KM_ANCHOR_BEFORE,
    KM_ANCHOR_AFTER
} KmAnchorAffinity;

typedef struct KmAnchor {
    KmAnchorId id;
    KmBytePos pos;
    KmAnchorAffinity affinity;
    struct KmAnchor *next;
    struct KmAnchor **prev_next;
} KmAnchor;

typedef struct {
    KmBytePos start;
    KmBytePos end;
    const uint8_t *insert;
    size_t insert_len;
    uint32_t ordinal;
} KmSplice;
```

不保存 gap 的冗余 `text_len` 或 `gap_len` 字段：

```text
gap_len = gap_hi - gap_lo
text_len = cap - gap_len
```

这减少不变量数量。所有长度运算必须先检查 `SIZE_MAX` 溢出。

为简化 batch 中的正负长度 delta，首版显式限制 Document 长度不超过 `PTRDIFF_MAX`；这仍远高于可在进程地址空间中完整装入 gap 的实际文件大小。任何 size/delta 转换都必须检查，不能依赖 unsigned wrap。

## 6. Gap buffer 实现

### 6.1 不变量

任意对外可见时刻都必须满足：

1. `gap_lo <= gap_hi <= cap`。
2. 有效文本位于 `[0,gap_lo)` 和 `[gap_hi,cap)`。
3. logical text length 为 `cap - (gap_hi-gap_lo)`。
4. gap 必须位于合法 UTF-8 code point boundary。
5. 所有 anchor 位于 `[0,text_len]` 的合法 code point boundary。
6. 任何对象不得长期保存 `mem` 指针、物理 offset 或 gap 两侧 slice。

逻辑位置到物理位置：

```text
phys(p) = p                         if p < gap_lo
          p + (gap_hi - gap_lo)     otherwise
```

### 6.2 移动 gap

```text
move_gap(p):
    if p < gap_lo:
        n = gap_lo - p
        memmove(mem + gap_hi - n, mem + p, n)
        gap_lo = p
        gap_hi -= n
    else if p > gap_lo:
        n = p - gap_lo
        memmove(mem + gap_lo, mem + gap_hi, n)
        gap_lo += n
        gap_hi += n
```

插入：移动到 `p`，确保 gap 足够，将 bytes 写到 `gap_lo`，增加 `gap_lo`。

删除 `[a,b)`：移动到 `a`，然后 `gap_hi += b-a`。

替换 `[a,b) -> s`：移动到 `a`，扩大 gap 吃掉旧范围，再从 gap 左端写入新 bytes。对外仍是一条 splice 和一次 anchor transform。

### 6.3 扩容

需要的容量在 transaction prepare 阶段计算。建议几何增长：

```text
new_cap = max(required_cap, max(4096, cap + cap / 2))
```

使用临时指针接收 `realloc`。成功后将 suffix 移到新 allocation 末端，扩大 gap；失败时旧 Document 完全不变。

不在首版实现可中断 gap move。大 `memmove` 期间不能响应 `C-g`，但状态简单且不会留下半移动结构；出现实测交互延迟后再考虑分块搬移。

### 6.4 逻辑 iterator

Unicode、搜索、保存和渲染只能通过 logical iterator 或范围复制 API 读取文本。Iterator 先遍历 prefix，再跳过物理 gap 遍历 suffix，对调用者呈现一个连续 byte stream。

首版不建立存储 vtable。建议 API：

```c
size_t km_document_len(const KmDocument *doc);
KmStatus km_document_copy(const KmDocument *doc,
                          KmBytePos start, size_t len,
                          uint8_t *dst, KmError *err);
KmTextIter km_document_iter(const KmDocument *doc, KmBytePos start);
KmStatus km_document_apply(KmDocument *doc,
                           const KmSplice *splices, size_t count,
                           KmTxnMeta meta, KmError *err);
```

`KmTxnMeta` 至少包含 `expected_revision`。Command 层先按 active `KmBuffer` 的 narrowing 验证 edit scope，再交给 Document；Document 自身只验证 revision、文档范围、UTF-8 boundary 和 splice 关系。`KmTextIter` 捕获创建时 revision，Document 一旦变化，后续 iterator 操作返回 conflict；iterator 不暴露可跨 mutation 保存的物理指针。

只要 command、layout 和 undo 不接触物理 gap，将来仍可改实现；现在不为假设性替换付出 vtable/factory 成本。

## 7. Anchor 与 splice 语义

Anchor 保存逻辑 byte offset 和 affinity。Document 维护侵入式无序链表。创建/销毁为 O(1)，每条 splice 的朴素更新为 O(M)，其中 M 是该 Document 的 live anchor 数。

Decoration 不在首版进入 anchor 链。若未来语法诊断产生海量 range，应使用按 revision 重建的独立区间集合，而不是给每个装饰端点注册 marker。

### 7.1 插入

在 `p` 插入 `n` bytes：

```text
if anchor.pos > p:
    anchor.pos += n
else if anchor.pos == p and affinity == AFTER:
    anchor.pos += n
```

`BEFORE` 对应 Emacs 默认 insertion type nil，留在插入文本前；`AFTER` 随右侧文本移动到插入文本后。

### 7.2 删除

删除 `[a,b)`，`d = b-a`：

```text
if anchor.pos > b:
    anchor.pos -= d
else if anchor.pos > a:
    anchor.pos = a
```

原来位于 `b` 的 anchor 不需要单独修改，因为删除后旧 `b` 和新 `a` 是同一逻辑边界。严格位于删除区内部的 anchor 收敛到 `a`。

### 7.3 替换

替换 `[a,b)` 为 `n` bytes，且 `a < b`：

```text
if anchor.pos >= b:
    anchor.pos += n - (b-a)
else if anchor.pos > a:
    anchor.pos = a
else:
    unchanged
```

这里 `anchor.pos == a` 不因 affinity 穿过 replacement；`anchor.pos == b` 移到 replacement 末端。该规则与抽样 GNU Emacs `adjust_markers_for_replace` 一致。纯插入 `a == b` 必须走上一节的 insertion affinity 规则。

### 7.4 Undo 所需 marker adjustment

普通 offset 平移可由 inverse splice 自动逆转，但删除/替换区内部 anchor 全部收敛到 `a`，信息已经丢失。Undo record 必须额外保存：

```c
typedef struct {
    KmAnchorId id;
    KmBytePos old_pos;
    KmBytePos expected_after_inverse;
} KmAnchorRestore;
```

删除的 inverse 是 insertion，`a` 处的 `AFTER` anchor 会被自动推到旧 `b`；因此不能只记录严格位于删除区内部的 anchor。对 delete/replace，Prepare 至少检查闭区间 `[a,b]` 内的全部 registered anchor，并为不能由 inverse splice 自动恢复的项目记录上述结构。

Undo 先执行 inverse splice。若该 ID 仍存活且当前位置等于 `expected_after_inverse`，再恢复 `old_pos`；ID 已销毁或 anchor 被其他非文本命令主动移动时跳过，避免旧 undo record 覆盖新的显式位置。Anchor ID 单调生成且不复用。

对于 batch，Prepare/模拟阶段以整个 transaction 为单位计算每个 anchor 的最终位置和 inverse 后预期位置；不要把多个 primitive splice 的 restore record 互相覆盖。

必须覆盖删除区两端 BEFORE/AFTER 四种组合。该设计对应 GNU Emacs `record_marker_adjustments` 对闭区间 `from <= marker <= to` 记录 adjustment 的原因。

## 8. Batch edit 与 transaction

Rectangle 和未来多 selection 都不能逐次读取并修改 Document。它们先基于同一 revision 生成完整 splice 列表，再一次提交。

### 8.1 Prepare

1. 记录并检查 expected revision。
2. 验证每条 `[start,end)` 自身满足 `start <= end`，位于 active Buffer 的 narrowing 内，并落在 code point boundary。
3. 严格验证 insert payload 为 UTF-8；NUL 是合法文本字节，不能使用 C string API。
4. 复制 descriptor 后稳定升序排序 `(start,end,ordinal)`。
5. 拒绝非空范围重叠，以及插入点满足 `delete.start <= p < delete.end` 的组合；调用者应先合并成一条 replace。插入点位于 delete 的右端 `p == delete.end` 可以接受。
6. 复制所有 insert payload，避免 payload 指向当前 gap allocation。
7. 复制所有将被删除的 bytes，生成 inverse splice。
8. 预分配 undo、anchor restore 和 apply 过程的峰值 gap 容量。峰值按升序执行时的最大 prefix delta 计算，不能只按最终长度；“先大插入、后大删除”的 batch 可能临时远大于最终文本。

Prepare 失败时 Document 不发生任何变化。

### 8.2 Apply

Splice 按原始 byte offset 从左向右执行。维护此前 edit 的累计长度变化 `delta`：

```text
current_start = original_start + delta
current_end   = original_end   + delta
delta        += insert_len - (original_end - original_start)
```

`delta` 是经过溢出检查的有符号 byte delta；转回 `KmBytePos` 前验证结果位于 `[0,PTRDIFF_MAX]`。

排序后的物理 gap 目标单调不减，不会反复跨过此前刚插入的 payload。相同位置的多个 insert 按 ordinal 正序 apply；每次更新 delta 后，下一条自然落在前一条插入文本之后，最终文本保持调用方顺序。

处理 forward splice 的当前 `[current_start,current_end)` 时，同时生成 final-state 坐标下的 inverse：`[current_start,current_start+insert_len) -> deleted_bytes`。后续 forward edit 均在其右侧，不再改变该 inverse 的起点。Undo 将这组 inverse 视为 final-state 原坐标，再使用同一升序+delta 引擎执行。

允许关系必须明确：

| 组合 | 结果 |
|---|---|
| 多个同点 insert | 接受，按 ordinal 连接 |
| insert 位于 replace/delete 左端或内部 | 拒绝，调用方合并 |
| insert 位于 replace/delete 右端 | 接受，结果在 replacement 后 |
| 两个非空范围仅端点相接 | 接受 |
| 两个非空范围相交 | 拒绝 |

总成本主要为：

```text
O(K log K) sorting
+ O(initial jump + original gaps between edits) gap movement
+ O(inserted + deleted bytes)
+ O(K * M) anchor updates
```

只有 `O(K*M)` 在大量 selection 与 anchor 下可能先成为问题。必须先 profile，再考虑按位置排序 anchor 或改用索引结构。模型测试必须包含多个 edit、每个 edit 都插入大 payload 的情况，并统计 moved bytes，防止重新引入倒序反复搬运。

### 8.3 Commit

Apply 阶段不得分配、不得调用插件/hook、不得执行可重入 callback，因此正常情况下不会失败。完成后：

1. `revision` 只递增一次。
2. 产生一个 history state 和一个 undo transaction。
3. 记录最小 changed byte range。
4. 失效相关 View 的布局 cache。
5. Commit 后才允许 renderer 或 observer 读取新状态。

首版不需要通用 rollback 框架。所有可预见失败都在 Prepare 前置处理；内部不变量失败属于程序缺陷，不尝试在未知损坏状态下继续运行。

## 9. Undo 设计

### 9.1 底层格式

使用线性 transaction journal。每个 outer command 最多产生一个 undo transaction：

```text
UndoTxn
  command_id
  state_before / state_after
  forward splices
  inverse splices
  anchor restores
  point metadata needed by compatibility layer
  explicit command boundary
```

Undo/redo 通过同一个 splice 引擎执行，但不能再作为普通用户 edit 重复记录。`revision` 每次实际修改都递增，history cursor 则移动到目标 state。

### 9.2 不提前冻结的行为

GNU Emacs 的 `undo`、`undo-only`、`undo-redo` 与常规双栈 undo/redo 不完全相同。以下行为必须对选定版本做差分测试：

- undo 命令本身是否作为后续 undo 的可撤销修改。
- undo 后执行非 undo 命令，再次 undo 时的遍历方向。
- 新编辑发生在历史中间时，哪些记录保留。
- 连续 self-insert、连续 delete、kill 命令如何合并。
- point、mark 和不同 Window point 的恢复方式。

首版底层只保证“可逆 splice + command boundary + marker restore”信息完整，不实现 branching tree、分支选择 UI、checkpoint 或基于墙钟时间的合并。

命令合并必须由 command loop 的明确规则触发，例如 `last_command == self_insert`，不能由一个模糊时间窗口猜测用户意图。

## 10. Unicode 与坐标模型

### 10.1 坐标职责

| 坐标 | 是否持久化 | 用途 |
|---|---|---|
| UTF-8 byte offset | 是 | gap、anchor、splice、undo、文件 I/O |
| code point boundary | 由 byte offset 表示 | Emacs 字符移动与删除 |
| EGC boundary | 否，布局派生 | glyph grouping、宽度、鼠标命中 |
| logical line | 否，扫描或缓存 | 垂直移动、rectangle、状态显示 |
| terminal cell column | 否，布局派生 | rectangle、wrap、hit-test、cursor |

不要创建一个含义模糊的 `position` 或 `column` API。

### 10.2 `utf8proc` 依赖

`采用` vendoring [`utf8proc v2.11.3`](https://github.com/JuliaStrings/utf8proc/releases/tag/v2.11.3)，其数据版本为 Unicode 17.0.0。构建为独立 target，不对第三方源码启用项目的 warnings-as-errors。

使用能力：

- `utf8proc_iterate`：严格 UTF-8 解码。
- `utf8proc_grapheme_break_stateful`：UAX #29 extended grapheme segmentation。
- category/property：控制字符与 combining 等分类。
- `utf8proc_charwidth` 和 ambiguous property：cell width 的 code point 基线。
- normalization/casefold：保留给显式搜索或转换命令，不自动修改 Document。

不使用：

- 系统 locale 驱动核心 Unicode 语义。
- POSIX `wcwidth()` 作为跨平台真值。
- 第二套 libgrapheme 或 ICU。

### 10.3 跨 gap 分段

Grapheme scanner 必须读取逻辑连续 byte stream。物理 gap、读缓冲分片或 viewport 起点都不是 grapheme boundary，不能在那里把 state 清零。

`utf8proc` 要求 stateful API 按顺序检查所有潜在断点，同时明确允许在已经确认的 grapheme break 后将 state 重置为 0。因此首版 cache 只需保存已确认的 EGC records，不需要保存每 64 个字符的内部 state。

首版缓存当前可见 hard lines：

```c
typedef struct {
    KmBytePos byte_start;
    KmBytePos byte_end;
    KmCellCol col_start;
    KmCellCol width;
    uint8_t flags;
} KmEgcRec;
```

Cache key 至少包含 Document revision、tab width、width policy 和 viewport width。编辑后直接重建受影响 hard line；超长单行成为实测瓶颈后，再在已确认 EGC boundary 增加稀疏 checkpoint。

### 10.4 Point 位于 EGC 内部

为了 Emacs 兼容，point 可位于任意 code point boundary。Renderer 必须允许多个逻辑 point 映射到同一 cell。初始映射策略：

- EGC 起点映射到 `col_start`。
- EGC 内部 boundary 使用已消费 prefix 中最大的正 code-point width，并钳制到整个 EGC width。
- Combining、variation selector、ZWJ 通常不增加 prefix width。
- EGC 结束 boundary 强制映射到 `col_start + egc_width`。这样 keycap、flag 等序列在结束前不提前取得完整 emoji width。
- 对 ZWJ/RI 等复杂序列建立 golden tests，并与目标 Emacs 做差分。

这只是稳定终端 cursor 策略，不声称一个终端 cell 能显示 grapheme 内部的真实插入位置。

### 10.5 EGC cell width policy

Unicode 不规定一个 EGC 在所有终端和字体中占多少 cell。Width 必须是显式 profile：

```text
tab: tabstop - (hard_line_col % tabstop)
ordinary EGC: max(positive utf8proc_charwidth(codepoint))
combining-only EGC: 1，并用 dotted-circle 或 replacement glyph 显示
contains ZWJ / VS16 / RI flag pair: 2
contains VS15: 优先 1
East Asian Ambiguous: 配置为 1 或 2，默认 1
C0/DEL/control: 转义为 ^X、^? 或 replacement，绝不原样输出到 terminal
```

Control 的替代文本和 width 必须由同一个函数同时返回：除 tab/newline 外，C0 与 DEL 使用两格的 `^X`/`^?`；其他不可打印字符若使用 replacement glyph，则 width 等于该 glyph 的实际 layout width。不能先按原 control 的 `utf8proc_charwidth` 记 0，再输出两格文本。

`tabstop` 使用 `KmCellCol`，配置入口要求 `tabstop >= 1` 并检查列加法溢出。不能把 EGC 内各 code point width 相加。该策略是产品约定，需要真实终端矩阵校准；不是 UAX #11 的直接结论。

### 10.6 Hard line 与 soft wrap

Document 内部 newline 统一为 LF。CRLF/CR 在加载边界转换并记录文件 EOL policy。

Soft wrap 只存在于 View，不插入字节。Wrap 只能发生在 EGC 边界；宽 2 EGC 在当前行只剩 1 cell 时整体移到下一 visual row。Tab 的列基准仍是 hard line cell column，不能因为 wrap 重新从 0 计算。

上下移动保存 preferred `KmCellCol`。短行暂时落到 EOL，但 preferred column 保留，直到发生横向移动或编辑。

首版可先使用“填满后在 EGC boundary 强制折行”的 greedy soft wrap，不实现 UAX #14 自然断词。

## 11. Rectangle 与多点编辑

Rectangle 是命令和布局语义，不是新的文本存储结构。

基本流程：

1. 从 point 和 mark 得到 logical line 范围。
2. 使用从 hard-line 起点计算的 cell column 得到 `[c0,c1)`。
3. 对每个 logical line，把 cell 边界映射为 byte range。
4. 按具体 command 决定短行 padding、tab 展开和空范围行为。
5. 生成同一 revision 上的 splice 列表。
6. 一次提交并产生一个 undo transaction。

不能把 soft-wrap visual row 当作 rectangle 的逻辑行。

存在一个必须差分验证的歧义：多个 code point boundary 可能映射到同一个 cell column，例如 combining mark；rectangle 边缘也可能落在 tab 或 width-2 EGC 覆盖的 cell 内。不能先用“绝不切 EGC”替代 Emacs 契约，因为 Emacs point 本身允许位于 EGC 内部。

每个 rectangle command 应分别冻结：

- 边界 bias：left、right、nearest 或保持 point 的 code point boundary。
- tab 是整体包含、局部 detab，还是向外扩展。
- `delete-rectangle` 对短行是否无操作。
- `clear/open/string-rectangle` 何时补 ASCII spaces。
- kill rectangle 中短行保存空串还是 padding。
- 宽 EGC 与 combining sequence 的结果。

如果未来加入多个 cursor，使用 `Selection[] -> splice batch` 复用本节机制；不要改变 Document 或 gap。

## 12. Command loop 与 keymap

### 12.1 规范事件

平台输入先归一化：

```c
typedef enum {
    KM_EVENT_KEY,
    KM_EVENT_TEXT,
    KM_EVENT_PASTE,
    KM_EVENT_MOUSE,
    KM_EVENT_RESIZE,
    KM_EVENT_FOCUS,
    KM_EVENT_EOF
} KmEventKind;

typedef struct {
    KmEventKind kind;
    uint32_t modifiers;
    uint32_t scalar_or_named_key;
    uint32_t repeat;
    /* kind-specific payload */
} KmEvent;
```

`TEXT` 是一个 Unicode scalar 或明确长度的 committed UTF-8。`PASTE` 是明确长度的 UTF-8 block。控制键和 named key 不走 self-insert。

传统终端无法区分 `C-i`/Tab、`C-m`/Return 等物理按键，兼容契约以 decoder 产生的事件为边界。

### 12.2 Keymap

首版只实现 global keymap 和一个小型 trie。每个节点使用 child/sibling 或小动态数组；没有证据前不引入 hash table、persistent trie 或任意 map stack。

```text
resolve_step(state, key):
    child found and child has descendants -> PREFIX
    child found and child has command only -> COMMAND
    child not found -> UNDEFINED
```

API 保留未来传入 ordered active maps 的空间，但首版不实现完整 minor/local/transient/remap 优先级。

### 12.3 Command loop 状态

Command context 至少包含：

- 当前 Editor、View、Buffer 和 Document。
- prefix argument：none、numeric、universal。
- `last_command`、`this_command`。
- 当前 key sequence。
- 本命令唯一 transaction builder。
- quit flag 和消息结果。

关键规则：

- `C-u` 与 numeric argument 属于 command loop，不属于 modifier。
- `C-g` 取消 prefix、参数或当前可取消操作，不由平台后端直接退出进程。
- self-insert 只处理未被更具体 binding 捕获的可插入文本。
- bracketed paste 一次进入批量插入 command，不逐键解释。
- 纯 prefix、resize、redraw 和未绑定键不污染 `last_command`。
- Keymap prefix 没有超时；只有 POSIX ESC 字节歧义使用 decoder deadline。

Keyboard macro、key translation、local/minor maps 和 command remap 按兼容矩阵后续加入，不进入首个纵切。

## 13. POSIX 终端后端

### 13.1 模式切换

启动时保存完整 `termios`，以副本进入 raw-like 模式。为了让 `C-c`、`C-z` 等进入 Emacs keymap，必须关闭 `ISIG`，不能让 tty driver 抢走这些字节。

典型设置：

```text
iflag: clear IGNBRK, BRKINT, PARMRK, ICRNL, INLCR, IGNCR, INPCK, ISTRIP, IXON
oflag: clear OPOST
cflag: set CS8
lflag: clear ECHO, ICANON, IEXTEN, ISIG
cc:    VMIN=0, VTIME=0
```

使用 `poll()` 等待 tty fd、signal self-pipe 和 ESC deadline。所有 read/write 都处理 partial I/O、`EINTR` 和 EOF。

`SIGWINCH` handler 只写 self-pipe 或设置 `sig_atomic_t`；主循环读取新尺寸并生成 Resize。正常退出、外部 SIGTERM/SIGHUP 和显式 suspend 路径恢复 termios、cursor、mouse、paste mode 与 alternate screen。

真正的 SIGSEGV 等进程损坏状态无法可靠保证恢复 tty；文档不能宣称 crash handler 提供绝对恢复。

### 13.2 VT parser

Parser 是有上限、可增量喂入的状态机：

```text
Ground -> Utf8
       -> EscPending -> CSI / SS3 / Meta fallback
       -> Paste
       -> OSCDiscard
```

要求：

- 任意输入分片方式产生相同事件。
- CSI/SS3 参数和缓存有硬上限。
- OSC 内容流式丢弃到 BEL/ST，不无限缓存。
- ESC deadline 可配置，初始值通过本地与 SSH 实测选择。
- Bracketed paste 识别 `CSI 200~` 和 `CSI 201~`，内部 ESC 不解释为命令。
- 鼠标仅在明确启用时解析 SGR 1006。
- 未知序列安全转为 unknown/escape 事件，不能执行或无限等待。

## 14. Windows 原生后端

### 14.1 输出

当 stdout 的 `GetConsoleMode` 成功时，保留旧 flags 并启用 `ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING`。成功后复用同一个 VT renderer。Renderer 仍生成 UTF-8；Win32 console adapter 将每批完整输出严格转为 UTF-16，再用 `WriteConsoleW` 写入，因此不依赖当前 ACP。重定向 handle 则直接写 UTF-8 bytes。

若 stdout 不是 Console handle，则不输出 alternate screen、cursor movement 或 SGR，除非用户显式要求 ANSI stream。VT 启用失败时第一版报告不支持，不实现旧 `WriteConsoleOutputW` renderer。

### 14.2 输入

真实 Console 默认使用 `ReadConsoleInputW`：

- 保存原 input mode；关闭 `ENABLE_PROCESSED_INPUT`、`ENABLE_LINE_INPUT`、`ENABLE_ECHO_INPUT`。
- 启用 `ENABLE_WINDOW_INPUT`；需要鼠标时同时启用 `ENABLE_MOUSE_INPUT` 和 `ENABLE_EXTENDED_FLAGS`，并显式关闭 Quick Edit。
- `KEY_EVENT_RECORD` -> key/text，过滤 key-up，展开 `wRepeatCount`。
- 合并 UTF-16 surrogate pair，拒绝孤立 surrogate。
- `dwControlKeyState` -> Ctrl/Alt/Shift。若 `RIGHT_ALT_PRESSED + LEFT_CTRL_PRESSED` 同时产生可打印 `UnicodeChar`，优先解释为 AltGr committed text，不产生 Ctrl+Meta binding；无可打印字符时才保留修饰键事件。
- named key 结合 virtual key 与 UnicodeChar 归一化。
- `MOUSE_EVENT_RECORD` -> cell mouse event。
- `WINDOW_BUFFER_SIZE_RECORD` -> Resize。
- dead key 和 IME 只接受宿主最终提交的文本，不实现 composition UI。

正常退出必须恢复原 input/output mode。安装 Console control handler 处理 close/logoff/shutdown 等外部终止，并执行 best-effort 恢复；进程或宿主强制终止时仍不能承诺绝对恢复。AltGr、dead key 和输入法行为必须在至少英文、中文及一个使用 AltGr 的键盘布局上做平台测试。

`ReadConsoleInputW` 没有 Paste 事件。剪贴板粘贴通常与快速键入不可可靠区分，因此 record 模式不能保证“每次粘贴一个 undo transaction”，也不能按一次 API read batch 猜测。

`ENABLE_VIRTUAL_TERMINAL_INPUT` 可作为未来 profile，用共享 VT parser 获取 bracketed-paste delimiter；在 Windows Terminal 与 conhost 的目标版本矩阵验证 mouse、resize、AltGr 和修饰键之前，不作为默认路径。

ConPTY 只在本程序未来需要托管子终端时使用。普通本地 TUI 不创建 pseudo console。

## 15. CellGrid 与渲染

### 15.1 Cell 表示

```c
typedef struct {
    size_t glyph_off;
    size_t glyph_len;
    uint16_t style_id;
    uint8_t width;       /* lead: 1 or 2; continuation: 0 */
    uint8_t flags;
} KmCell;
```

每个 `KmGrid` 独占自己的 glyph arena，Cell 的 offset/length 只相对于所属 grid。宽字符写一个 lead cell 和一个 continuation cell。Combining bytes 属于 lead glyph，不能成为独立 continuation cell。

EGC 长度在 Unicode 中没有 64 KiB 上限，因此 offset/length 使用 `size_t` 并做溢出检查。为了避免恶意超长 combining sequence 阻塞输出，renderer 将来可以设置显示资源上限并以 placeholder 代替该 EGC 的画面，但必须保留原 Document bytes，不能截断字段或改写文本；相应测试至少包含一个超过 64 KiB 的合法 EGC。

### 15.2 Diff

维护 `front`（已知终端画面）和 `back`（目标画面），两者各自拥有 cells 与 glyph arena：

1. 重置 back 自己的 arena，Layout 填充完整 back grid；front arena 保持不变。
2. 对 dirty rows 找第一个和最后一个不同 cell；glyph 比较读取各自 grid 的 arena bytes，不能只比较 offset 数值。
3. 绝对定位到 row span 起点。
4. 按 style 生成连续 glyph run，跳过 continuation。
5. 覆盖或清除旧宽字符尾部和旧行尾残留。
6. 最后设置 cursor 可见性、位置和形状。
7. 输出成功后连同 arena ownership 整体交换 front/back；下一帧只重置已经成为 back 的旧 grid。输出失败则保留 front，丢弃 back，并将 front 标记 unknown 以便下次全屏重绘。

首版不做 scroll-region、insert/delete-line、终端查询驱动优化。Resize、width policy 变化、重新进入 alternate screen 和输出错误都触发 full redraw。

### 15.3 不选择 ncurses

ncurses 能提供 terminfo 和虚拟/物理屏幕 diff，但不能消除以下工作：

- Windows 原生输入后端。
- Emacs 风格规范事件模型和 ESC 时序。
- Bracketed paste、现代鼠标/键盘扩展。
- 本项目自己的 EGC 与 cell width policy。
- Headless CellGrid golden test。

当前目标是现代 VT terminal，因此薄后端更少。只有明确要求支持大量老旧、异构 `$TERM`，并愿意接受 ncurses/terminfo 作为运行时契约时，才重新评估 `ncursesw`。

## 16. 文件与数据安全

### 16.1 Path

`KmPath` 是 platform 层 opaque 类型：

- POSIX 保存原始 filename bytes；不能假定它们是 UTF-8。
- Windows 保存 UTF-16 code units，文件操作只调用 `...W` API。
- UI 使用可逆 UTF-8/escape 表示显示非法路径字节。
- 不对文件名自动做 NFC/NFD normalization。

Document 的 UTF-8 内容模型与 Path 编码完全分离。

### 16.2 加载策略

1. 读取 bytes，处理短读和文件大小变化。
2. 记录文件 identity、size 和 mtime。
3. 检测 UTF-8 BOM；Document 不保存 BOM bytes，base Buffer 的 FileState 保存 flag。
4. 严格验证 UTF-8。
5. 识别统一 LF、CRLF 或 CR，并转换为内部 LF。
6. 创建 Document、base Buffer 和初始 history state。

非法 UTF-8 或 mixed EOL 在 v1 不进入可写文本模式：只读打开并要求用户明确选择转换/规范化，或者直接拒绝。绝不静默插入 U+FFFD 后覆盖原文件，也不声称 mixed EOL 可无损 round-trip。

NUL 是合法 Unicode scalar U+0000 的 UTF-8 表示。内部 API 必须始终使用 pointer+length；renderer 将其显示为 `^@` 或 control glyph。

### 16.3 同步安全保存

首版同步执行，步骤如下：

1. 记录当前 history state 和目标 file identity。
2. 保存前再次 stat/GetFileInformation，按下述 identity 字段比较；发现外部变化则停止并请求用户决策。该检查只能缩小竞态窗口，不能消除随后 pathname replace 前的 TOCTOU。
3. 在目标同目录排他创建唯一临时文件。POSIX 使用 `mkstemp` 或 `openat(O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600)`；Windows 使用 `CreateFileW(..., CREATE_NEW, ...)`。不得先生成可预测名称再普通 open。
4. 写 BOM、按目标 EOL policy 编码后的完整内容，循环处理 short write。
5. 保留可支持的基础 mode/attributes；ACL、xattr、owner 的完整策略单独测试。
6. POSIX `fsync(temp)`；Windows `FlushFileBuffers(temp)`。
7. 关闭临时文件。
8. POSIX `rename(temp,target)`；Windows 对已存在目标使用 `ReplaceFileW`，新目标使用同卷 move。
9. POSIX 尽力打开并 `fsync` parent directory。
10. 更新 file identity；只有仍处于刚保存的 history state 才更新 `saved_state_id`。

任何失败保持 Document dirty，原目标文件不得先删除。跨文件系统替换失败不能降级为“删目标再 copy”。临时文件是否保留取决于失败阶段，但必须在消息中报告其路径。

V1 对 symbolic link、Windows reparse point 和多 hard-link 目标采用保守策略：检测后拒绝安全替换并要求用户显式选择未来提供的跟随/原地写策略。直接 rename 会替换 symlink 本身并可能打断 hard-link 关系，不能静默执行。

File identity 至少包含：POSIX `(device, inode, ctime, mtime, size, link_count)`；Windows volume serial、file ID、last-write time、size、link count 和 reparse attributes。最终 replace 仍存在外部进程竞争，因此文档只承诺 best-effort conflict detection。

该协议只能表述为“尽力 crash-safe”。POSIX `rename` 的名称切换原子性不等于所有设备断电后绝对持久；Windows、网络盘、驱动缓存也没有统一的绝对保证。

首版不实现 native file watcher。打开文件、保存前和显式 focus/revert 检查 file identity 已覆盖最关键的误覆盖风险。

## 17. 错误与内存策略

C API 使用状态码和 out parameter，不构建泛型 `Result<T>` 宏体系：

```c
typedef enum {
    KM_OK,
    KM_ERR_OOM,
    KM_ERR_INVALID,
    KM_ERR_IO,
    KM_ERR_PERMISSION,
    KM_ERR_CONFLICT,
    KM_ERR_UNSUPPORTED,
    KM_ERR_CANCELLED
} KmStatus;

typedef struct {
    KmStatus code;
    uint64_t os_code;
    const char *operation;
} KmError;
```

规则：

- 长度加法/乘法先检查溢出。
- `realloc` 使用临时指针。
- 库层不 `exit()`，由 app 层显示错误并决定是否继续。
- 修改 Document 前完成所有可能失败的分配。
- 保存失败绝不清除 dirty。
- Renderer OOM 时保留旧 front，恢复终端后可受控退出。
- 第三方 `utf8proc` 的 allocation 使用其配套 free API。

不建立自定义 allocator framework。未来插件 ABI 才引入 host allocator function table。

## 18. 构建与源码布局

首版不为每个逻辑模块创建一个 library。建议保持两个生产 target：可 headless 测试的 `km_core` 和最终可执行文件 `km`。

```text
nob.c
nob.h
src/
  base.c/.h
  document.c/.h       gap, anchors, transaction, undo
  unicode.c/.h        utf8proc adapter, codepoint/EGC/width
  editor.c/.h         Buffer, View, command loop, keymap
  layout.c/.h         visible-line layout, rectangle projection
  render.c/.h         CellGrid and VT output generation
  input_vt.c/.h       POSIX/pipe byte-stream parser
  platform.h          narrow platform contract
  platform_posix.c
  platform_win32.c
  main.c
third_party/
  utf8proc/
tests/
  test_document.c
  test_unicode.c
  test_commands.c
  test_layout.c
  test_input_vt.c
  test_save.c
  fuzz_input_vt.c
  fuzz_document.c
```

这只是起始布局。某个文件明显变大后再拆分，不提前为每个 struct 创建目录和 target。

`nob.h` 构建要求：

- 只依赖可用的 C 编译器即可 bootstrap `nob.c`；构建程序支持 GCC、Clang 和 MSVC。
- 构建程序按 target 组装 include、defines 和 warnings，不依赖全局环境 flags。
- 构建程序只选择一个 platform `.c`；核心不得散布 `_WIN32`。
- `utf8proc v2.11.3` 作为 vendored 第三方源码单独编译，项目的 warnings-as-errors 不施加到第三方源码。
- GCC/Clang 启用严格 warnings；MSVC 使用对应 `/W4`。
- `test` 目标运行 headless tests；Debug sanitizer 由明确的构建选项启用。

当前 bootstrap 与验证入口为：

```text
cc -std=c17 nob.c -o nob
./nob build
./nob test
./nob sanitize
./nob clean
```

`clean` 只能删除 `build/`，并且不得跟随 POSIX symlink 或 Windows reparse
point。构建程序自重建时必须保留 bootstrap 所用编译器和 C17 模式；切换
编译器或 sanitizer 配置时必须使第三方对象与最终目标失效，不能跨 ABI
复用缓存。

## 19. 测试与验证

### 19.1 Document 模型测试

使用普通连续 byte array 作为参考模型，随机生成合法 UTF-8 splice 和 anchors。每步比较：

- 最终 bytes。
- gap 不变量。
- anchor offset/affinity。
- transaction abort 前后完全一致。
- undo/redo round-trip。
- 一个 batch 的结果等于在原坐标语义下的参考结果。

覆盖 anchor 在 `a`、`b`、删除内部、两端 BEFORE/AFTER 四种组合、replace 长度增减和多个同点 insert。Batch 测试包含每个位置都插入大 payload 的情况，并断言 moved-bytes 不会反复跨越早先 payload。

### 19.2 Unicode conformance

- 使用 Unicode 17.0.0 对应的 `GraphemeBreakTest.txt`。
- 在每一个可能物理 gap 位置切开同一 logical string，分段结果必须相同。
- 测试 combining、长 RI run、ZWJ emoji、VS15/16、CJK、ambiguous、孤立 combining、NUL、control 和 tab。
- Width golden 同时断言 EGC、cell span、wrap、cursor mapping 和 hit-test。
- 控制字符 golden 同时比较替代 glyph bytes 和声明 width，覆盖 `^@`、`^G`、`^?`。
- 配置测试拒绝 `tabstop=0`，并覆盖 `tabstop=256`，确保 span width 不发生 8-bit 截断。

### 19.3 Command 差分

为选定 GNU Emacs 正式版本建立 batch harness。每个 transcript 输出机器可比较的状态，不比较内部 Lisp object 或原始 VT bytes。

Harness 外部使用 Emacs 的 1-based character position；适配层通过严格 UTF-8 code point scan 与内部 0-based `KmBytePos` 双向转换。禁止直接把 byte offset 与 Emacs point 数值比较。

首批命令建议：

```text
self-insert-command
forward-char / backward-char
delete-char / delete-backward-char
set-mark-command / exchange-point-and-mark
kill-region / kill-line / yank / yank-pop
universal-argument and numeric arguments
undo / undo-only / undo-redo
rectangle-mark-mode and C-x r commands
```

### 19.4 Parser 与 renderer

- 同一 VT byte stream 按任意 chunk 切分，事件序列必须相同。
- 覆盖 ESC timeout、非法/超长 CSI、bracketed paste、unknown sequence。
- CellGrid golden 不依赖真实 terminal。
- 连续构建两帧并交换 grid/arena，断言 front glyph 在 back arena 重置后仍可比较。
- 覆盖宽字符从 2 cell 变为 1 cell、行缩短、resize 和输出失败后的 full redraw。

### 19.5 平台与文件

- Linux、macOS、Windows 构建与 headless tests。
- POSIX PTY 和 Windows Console smoke test：进入/恢复模式、UTF-8、resize、mouse。
- 保存故障注入：short write、ENOSPC、permission、sharing violation、外部替换、flush/rename 失败。
- 保存路径覆盖 symlink/reparse point、hard link、identity race 和排他临时文件创建。
- ASan、UBSan 和针对 VT parser/document splice 的 libFuzzer。

## 20. 实施顺序与退出条件

### Phase 0：契约与双平台探针

- 冻结 C17、目标 Emacs 版本、Windows 最低环境和 Unicode 版本。
- `nob.h` 构建程序在 GCC/Clang/MSVC bootstrap 并构建项目。
- POSIX 与 Windows 都完成：进入终端、读一个事件、画 CellGrid、resize、恢复退出。
- Vendoring `utf8proc v2.11.3` 并运行版本检查。

退出条件：两个平台显示 ASCII、组合字符和 CJK，退出后终端模式恢复。

### Phase 1：Headless Document

- Gap、logical iterator、UTF-8 validation。
- Anchor 与 atomic splice transaction。
- Linear undo journal 基础。
- Phase 1 只完成 Document；文件路径、identity、BOM、EOL metadata 由 Phase 2 base Buffer 的 FileState 拥有。

退出条件：随机参考模型、anchor 和 undo round-trip 全部通过 sanitizer。

### Phase 2：Buffer/View 与命令

- Base/indirect Buffer、narrowing、Window point。
- Base/indirect 的 visited filename、save/revert、modified 和 kill 行为差分。
- Global key trie、command registry、prefix argument。
- 基础移动、mark/region、kill/yank、undo boundary。
- Emacs batch transcript harness。

退出条件：首批 headless command 在两平台结果相同，声明兼容项通过差分。

### Phase 3：Unicode layout 与 TUI

- Visible hard-line EGC cache、width policy、CellGrid。
- POSIX VT parser 和 Win32 input records。
- Row diff、scroll、status/message line、bracketed paste（POSIX）。

退出条件：Unicode conformance、CellGrid golden、PTY/Console smoke tests 通过。

### Phase 4：安全文件编辑

- 同步安全保存、外部 identity 检查、BOM/EOL policy。
- 保存故障注入和平台集成测试。

退出条件：任何注入失败都不破坏原文件且保持 dirty；正常保存可回读一致。

### Phase 5：Rectangle 与兼容扩面

- 逐命令冻结 rectangle 的 column/bias/tab/short-line 行为。
- 一次 rectangle 一个 batch transaction。
- 根据兼容清单增加 keymaps、search、keyboard macro 等。

退出条件：rectangle 的 UTF-8、布局、undo 和 Emacs transcript 矩阵通过。

### Phase 6：基于测量升级

只有出现数据后才考虑：

- Piece tree/rope。
- 稀疏 line/EGC index。
- Anchor 索引和 decoration interval tree。
- Scroll-region 渲染优化。
- 后台任务和 file watcher。
- Windows VT input profile。

QEmacs 专项调研给出的升级指标包括：大文件 load time/RSS、`gap_bytes_moved / inserted_bytes`、splice p95/p99、transaction K 和 live anchor M。没有这些数据时，不用 QEmacs page array 替换 gap。

### Phase 7：插件

至少等 command、Document iterator 和 transaction API 经历一个稳定版本。先做随主程序源码重编译的内置模块，再决定是否冻结动态 ABI。

未来动态 C ABI 的最低要求：版本与 struct size、opaque handles、pointer+length UTF-8、host allocator、主线程调用、默认不热卸载。Windows DLL 边界禁止一侧 `malloc` 另一侧 `free`。

## 21. 被拒绝方案与重新评估条件

| 方案 | 当前拒绝原因 | 重新评估条件 |
|---|---|---|
| Piece table / rope | 实现和验证成本高，现无性能数据 | gap moved-bytes、延迟或 snapshot 需求持续超标 |
| ncurses/PDCurses | Windows 与自有事件模型仍需另做；不解决 Unicode width | 明确支持大量旧/异构 terminfo 终端 |
| ICU | 体积和部署成本过高 | 要求 locale dictionary word break、collation 或完整 i18n |
| libgrapheme + utf8proc | 能力重叠 | `utf8proc` 的 UAX #29 被实证不足且无法上游修复 |
| 系统 `wcwidth` | locale 与平台相关，不支持 EGC | 仅作为某个已校准 terminal profile 的对照 |
| ConPTY | 它是子终端宿主 API，不是本地 TUI 必需品 | 编辑器需要嵌入 shell/terminal session |
| Branching undo tree | 不属于 Emacs 最小兼容路径 | 产品明确要求分支历史 UI |
| 全文 line/EGC index | 首版没有热点证据 | 超长行、goto-line 或大文件 profile 失败 |
| 原生 file watcher | 事件并非可靠真相，首版 stat 足够 | 外部变更响应延迟成为真实问题 |
| 线程池 | 增加同步与生命周期复杂度 | 保存、索引或语法分析实测阻塞 UI |
| 动态插件 ABI | 过早冻结内部模型 | 核心 API 稳定并有真实第三方插件需求 |

## 22. 剩余风险

1. 目标 GNU Emacs 版本未冻结，本机开发版实验不能作为永久契约。
2. Terminal emoji/ambiguous width 没有统一标准，只能配置和实测。
3. Point 位于 ZWJ/complex EGC 内部时，逻辑位置无法由 terminal cursor 精确表达。
4. Rectangle 的精确 Unicode 边缘行为仍需逐命令差分。
5. Windows record input 无可靠 paste boundary；VT input 的替代方案尚未验证。
6. Mixed EOL、非法 UTF-8、非 UTF-8 编码暂时只能拒绝或显式转换。
7. 同目录 replace 不能保证所有网络文件系统和断电场景绝对持久。
8. Gap 在超大文件、跨文档两端反复编辑和大量 anchors 下可能退化，但当前应测量而不是预先换树。
9. Terminal-only 前端无法提供完整 bidi、shaping 和 IME composition；若这些成为硬需求，需要独立 GUI frontend，而不是继续堆 VT workaround。

## 23. 主要参考资料

### GNU Emacs

- [`buffer.h` 固定提交：gap、共享文本和 marker](https://github.com/emacs-mirror/emacs/blob/78ec68e18f07a90a9ad400683b973ff51baa80e1/src/buffer.h#L319-L374)
- [`insdel.c` 固定提交：delete marker adjustment](https://github.com/emacs-mirror/emacs/blob/78ec68e18f07a90a9ad400683b973ff51baa80e1/src/insdel.c#L229-L263)
- [`insdel.c` 固定提交：insert marker adjustment](https://github.com/emacs-mirror/emacs/blob/78ec68e18f07a90a9ad400683b973ff51baa80e1/src/insdel.c#L266-L314)
- [`insdel.c` 固定提交：replace marker adjustment](https://github.com/emacs-mirror/emacs/blob/78ec68e18f07a90a9ad400683b973ff51baa80e1/src/insdel.c#L334-L365)
- [`undo.c` 固定提交：insert coalescing 和 marker restore](https://github.com/emacs-mirror/emacs/blob/78ec68e18f07a90a9ad400683b973ff51baa80e1/src/undo.c#L81-L160)
- [`keyboard.c`，Emacs 30.1 tag](https://git.savannah.gnu.org/cgit/emacs.git/tree/src/keyboard.c?h=emacs-30.1)
- [GNU Emacs Manual: Rectangles](https://www.gnu.org/software/emacs/manual/html_node/emacs/Rectangles.html)
- [GNU Emacs Lisp Manual: Columns](https://www.gnu.org/software/emacs/manual/html_node/elisp/Columns.html)
- [GNU Emacs Lisp Manual: Markers](https://www.gnu.org/software/emacs/manual/html_node/elisp/Overview-of-Markers.html)
- [GNU Emacs Lisp Manual: Command Loop](https://www.gnu.org/software/emacs/manual/html_node/elisp/Command-Loop-Overview.html)
- [GNU Emacs Manual: Arguments](https://www.gnu.org/software/emacs/manual/html_node/emacs/Arguments.html)

### QEmacs

- [QEmacs 架构专项调研](qemacs-architecture-research.md)
- [QEmacs 6.5.2 固定源码提交](https://github.com/qemacs/qemacs/tree/b1f189c924c36c8074b54042f70e53eb785e3010)
- [QEmacs `Page` / `EditBuffer`](https://github.com/qemacs/qemacs/blob/b1f189c924c36c8074b54042f70e53eb785e3010/qe.h#L303-L488)
- [QEmacs TTY row diff](https://github.com/qemacs/qemacs/blob/b1f189c924c36c8074b54042f70e53eb785e3010/tty.c#L1646-L1725)

### Unicode

- [UAX #29: Unicode Text Segmentation](https://www.unicode.org/reports/tr29/)
- [UAX #11: East Asian Width](https://www.unicode.org/reports/tr11/)
- [UAX #15: Unicode Normalization](https://unicode.org/reports/tr15/)
- [UAX #9: Unicode Bidirectional Algorithm](https://www.unicode.org/reports/tr9/)
- [`utf8proc v2.11.3`](https://github.com/JuliaStrings/utf8proc/releases/tag/v2.11.3)
- [`utf8proc_grapheme_break_stateful` 固定 tag](https://github.com/JuliaStrings/utf8proc/blob/v2.11.3/utf8proc.h#L647-L662)

### Terminal 与平台

- [POSIX `tcgetattr`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/tcgetattr.html)
- [POSIX `tcsetattr`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/tcsetattr.html)
- [POSIX `poll`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/poll.html)
- [xterm Control Sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)
- [Microsoft: Classic Console APIs versus VT](https://learn.microsoft.com/en-us/windows/console/classic-vs-vt)
- [Microsoft: Console Virtual Terminal Sequences](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences)
- [Microsoft: `ReadConsoleInput`](https://learn.microsoft.com/en-us/windows/console/readconsoleinput)
- [Microsoft: `KEY_EVENT_RECORD`](https://learn.microsoft.com/en-us/windows/console/key-event-record-str)
- [Microsoft: `CreatePseudoConsole`](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole)

### 文件、构建与算法

- [POSIX `rename`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html)
- [POSIX `fsync`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fsync.html)
- [Microsoft `ReplaceFileW`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)
- [Microsoft `FlushFileBuffers`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
- [`nob.h`](https://github.com/tsoding/nob.h)
- [Crowley: Data Structures for Text Sequences](https://www.cs.unm.edu/~crowley/papers/sds.pdf)
- [Boehm, Atkinson, Plass: Ropes, an Alternative to Strings](https://cs.rit.edu/usr/local/pub/jeh/courses/QUARTERS/FP/Labs/CedarRope/rope-paper.pdf)
- [LLVM libFuzzer](https://llvm.org/docs/LibFuzzer.html)

## 24. 当前结论

第一版应围绕一个具体、可验证的纵向链路实现：

```text
UTF-8 gap Document
-> atomic splice + anchors + linear undo
-> Emacs Buffer/View semantics
-> normalized events + command/key trie
-> utf8proc visible-line layout
-> CellGrid row diff
-> POSIX VT / Windows Console backend
-> crash-aware synchronous save
```

该路径保留了未来替换存储、增加 GUI frontend 和发布插件 ABI 的可能性，但没有为这些尚未发生的需求建立抽象框架。下一步不应先铺完整目录和空接口，而应从 Phase 0 双平台探针与 Phase 1 headless Document 的可运行测试开始。
