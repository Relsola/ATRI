# Compiler

Compiler 是 ATRI 大型个人项目的基础子项目，也是当前建设的第一步，目标是实现一门以 DOP（面向数据编程）为核心取向、偏底层的强类型语言及可持续自举的原生编译器。

首个目标平台为 Windows x86-64，使用 C23 实现 Stage 0。目前已有 i32、i64、bool 与 `*i32` 的编译链路：Lexer/Parser/AST、名称/类型检查、整数/布尔编译期常量求值、IR 与 Verifier、传播/折叠、小函数内联及 x64 汇编输出。支持整数指针取地址、显式读写、参数/返回，以及按元素缩放的加减和 i64 元素距离；已支持 if/else、while、do...while、for、loop、break/continue、比较和短路逻辑，以及常量求值中的分支/循环/递归；已支持相对 .ar 多模块导入、函数/标量全局导出及共享全局存储；已实现结构体定义、字段默认值/指定初始化、复制和按值传参/返回；已补充固定数组、u8/*u8/u64、整数转换、sizeof/alignof、三元条件表达式及编译期 run 声明/标量表达式；union/enum、defer、run 代码块/AST 编译期系统和浮点运算等尚未实现。

已增加 C 兼容 `*char`、静态零结尾字符串字面量和 `[[extern(C)]]` 外部调用，可以通过 puts/printf 打印。已支持当前类型范围内的 C 可变参数调用，并用 void 返回、*void、opaque FILE 和数据指针 cast 接通文件读写。稳定 ABI 目标与 C 一致，内置 string 暂缓；完整范围和限制见[外部调用子集](NEXT/parser_subset_v0_1.md#c-字符串与外部调用)。

## 运行整数编译原型

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
.\build\atri-parse.exe .\examples\main.ar
.\build\atri-cc.exe --emit-ir-before-opt .\examples\main.ar
.\build\atri-cc.exe --emit-ir .\examples\main.ar
.\build\atri-cc.exe -O0 --emit-ir .\examples\constants.ar
.\build\atri-cc.exe --emit-ir .\examples\pointers.ar
.\build\atri-cc.exe --emit-ir .\examples\pointer_arithmetic.ar
.\build\atri-cc.exe --emit-ir .\examples\control_flow.ar
.\build\atri-cc.exe -S -o .\build\main.s .\examples\main.ar
clang .\build\main.s -o .\build\main.exe
.\build\main.exe
$LASTEXITCODE # 3
```

普通变量写作 `a: i32 = 1;`，省略初始化器时默认零初始化；编译期常量写作 `n: i32 := add(1, 2);`，无法在编译期求值时必须报错。[常量示例](examples/constants.ar)在 -O0 下也直接返回 3，不依赖内联。全角分号 `；` 会报错。完整范围、暂用的 BD-06 候选规则和当前后端限制见[整数编译原型](NEXT/parser_subset_v0_1.md)。

打印示例见 [hello.ar](examples/hello.ar)：

```powershell
.\build\atri-cc.exe -S -o .\build\hello.s .\examples\hello.ar
clang .\build\hello.s -o .\build\hello.exe
.\build\hello.exe
# import "basic"
```

当前使用 Clang 默认 C 启动代码及运行库完成链接；编译器本身仍输出汇编，不自动执行链接器。`-o` 只指定文本输出文件名，不能直接生成 exe；指定 `.exe`（不区分大小写）会返回命令行错误且保留已有文件，必须先输出 `.s` 再使用 Clang 汇编/链接。`build.bat` 是不随仓库提交的本地试验脚本；上面的命令可直接用于构建示例。C 声明和字面量规则见[语言规范第 5.5/13 节](NEXT/language_spec_v0_1.md#13-c-互操作与执行环境)。

数组写作 `data: [4]u8 = {1, 2};`，当前省略元素按类型默认初始化，支持索引、嵌套聚合和按值传参/返回。编译期示例见 [run_constants.ar](examples/run_constants.ar)：`run a: i32 := max(1, 2);` 在 -O0/-O1 下均生成常量 2，不执行运行期调用。数组长度、编译期结果及当前限制见[实现子集](NEXT/parser_subset_v0_1.md#固定数组的当前实现)。

整数转换使用 `byte: u8 = cast(u8, rgba);`，显式缩窄保留低位，扩宽按源类型符号/零扩展。`sizeof(T)`、`alignof(T)` 返回 u64 编译期值，也可用于数组长度。例子见 [conversions.ar](examples/conversions.ar)，准确能力与限制见[整数转换与布局查询](NEXT/parser_subset_v0_1.md#整数转换与布局查询)。

文件读取示例使用 [c_stdio.ar](examples/c_stdio.ar) 显式绑定 C 标准库：[read_file.ar](examples/read_file.ar) 分块读取并打印 examples/main.ar；[variadic.ar](examples/variadic.ar) 演示 `printf(format: *char, ...)`。从 Compiler 目录执行：

```powershell
.\build\atri-cc.exe -S -o .\build\read_file.s .\examples\read_file.ar
clang .\build\read_file.s -o .\build\read_file.exe
.\build\read_file.exe
```

可变参数目前覆盖已支持整数、数据指针和 C 兼容普通结构体，u8/bool 尾参提升为 i32；浮点、va_list/va_arg 定义端及完整 C ABI 仍待实现。FILE 只作 opaque 指针使用，CRT 的分配、缓冲和 I/O 有实际成本，文件错误及关闭由调用方处理。准确边界见[实现子集](NEXT/parser_subset_v0_1.md#c-可变参数opaque-file-与文件读取)。

编译期结果目前可通过 `--emit-ir-before-opt` 查看：输出已完成 run/常量求值、尚未应用可选优化的 IR；AST Dump 只展示解析结果。`run a: i32 = max(1, 2);`（等价于 `a: i32 = run (max(1, 2));`）的初始值在编译期求得，a 仍可在运行期赋值，区别于 `:=` 常量。求值记录与 AST 生成代码视图仍待实现，参数未定，不承诺统一的“预处理后源码”或可再次编译的源码导出；详见[编译期观察与调试](NEXT/language_spec_v0_1.md#125-编译期执行的观察与调试)。

## 文档

- [当前进度与验证记录](NEXT/progress_v0_1.md)
- [测试用例与预期写法](tests/README.md)
- [开发约定：代码风格、工具与验证](docs/development.md)
- [语言规范 v0.1](NEXT/language_spec_v0_1.md)
- [已实现的基础编译子集](NEXT/parser_subset_v0_1.md)
- [已实现的整数指针子集](NEXT/pointer_subset_v0_1.md)
- [IR 内部设计](NEXT/ir_design_v0_1.md)
- [基础默认行为与设计取舍：审查稿 v0.1](NEXT/language_defaults_review_v0_1.md)：30 项默认提案、各项理由、重大替代方案、行为预期及零开销边界。
- [编译器架构与自举路线](NEXT/self_hosting_compiler_design_v0_1.md)

后续编译器相关设计、源码和验证材料在本目录维护，Git 提交由父目录 ATRI 的统一仓库管理。

## 当前审查重点

已按用户授权拟定基础默认行为，新增方案仍待审查，用户可按 BD 编号指定修改。运行时零开销是明确方向，默认无隐式分配和强制语言运行时；原始指针检查与内置数组检查分别审查，默认零初始化等必要语义成本仍需保留。

已审查确认：条件与逻辑操作数必须为 bool；指针判断保持显式，不采用 `if (pointer)`、`!pointer` 等特殊真值转换。BD-02/09 的其他内容继续待审查。

已审查确认：BD-19 的语句式条件/循环、C 形状 for、明确的 break/continue 目标及首版无标签规则全部接受；所有控制体强制使用 `{}`，单语句和空体也不省略，换行和缩进不决定归属。

已审查确认：defer 使用 `defer { ... }`，在词法作用域退出时逆序执行，读取其实际执行时的变量当前值，不使用登记时快照；语言没有异常。返回值与清理的先后及 defer 的其余边界继续按 BD-20 审查。

已审查确认：结构体字段统一使用点号定位，指针必须先显式解引用，写作 `(*p).field`；不支持 `p.field` 或箭头成员访问，点号自身不承担自动解引用。

已审查确认：数组当前采用 C 兼容布局和有限静态越界检查，不生成运行时越界检查；同一结构体初始化器重复指定同一字段为编译错误，即使值相同亦不允许。

已审查确认：BD-16 的声明顺序、自然对齐、无聚合自动相等及首版布局边界接受；空结构体大小改为 0、对齐保持 1。零大小对象的地址、指针操作、分配与 FFI 配套继续审查。DOP 定位不自动增加专用 SoA/SIMD 语法或改变既有 C 布局。

1. 整数环绕、cast 截断与非法运算边界（BD-06/07/25）。
2. char 标量与只读指针接口（BD-04/05/13）；C 兼容字符指针已确认并实现，内置 string 整体暂缓。
3. 编译期常量的聚合/地址边界、目标只读接口和按值复制（BD-10/11/12）。
4. 原始指针的低层访问契约、数组完整类型/值语义、零大小对象配套、枚举有效零值及全局初始化限制（BD-14/15/16/17/24、ARRAY-01）；当前数组检查范围已确定，运行期检查后续探索。

主规范仍为规则维护入口，审查稿用于提出未确认行为和解释取舍；接受后再合入主规范。完整核心子集范围、AST 修改调度、完整 EBNF 和目标 ABI 实现继续按决策清单推进，不因基础提案而视为已经完成。

模块与全局的能力、静态初始化和路径限制见[实现子集](NEXT/parser_subset_v0_1.md#模块与全局的当前实现)。hello.ar 从 basic.ar 导入 add，并写入其共享全局 TC。尚未实现跨次编译缓存，也未测量相对 C 头文件的解析性能。

结构体示例见 [struct.ar](examples/struct.ar)，包括字段默认值、复制、按值传参与返回：

```cmd
build\atri-cc.exe -S -O1 -o build\struct.s examples\struct.ar
clang build\struct.s -o build\struct.exe
build\struct.exe
```

成功退出码为 0。当前结构体边界及复制成本见[实现子集](NEXT/parser_subset_v0_1.md#结构体的当前实现)。
