# Compiler

Compiler 是 ATRI 大型个人项目的基础子项目，也是当前建设的第一步，目标是实现一门以 DOP（面向数据编程）为核心取向、偏底层的强类型语言及可持续自举的原生编译器。

首个目标平台为 Windows x86-64，Stage 0 使用 C23 实现。从源码解析、类型检查、编译期求值和 IR 验证到 x64 汇编输出，当前支持八种定宽整数、bool、多级数据指针、整数枚举、固定数组、结构体和无标签 union。已支持相对 `.ar` 导入、共享全局存储、聚合静态初始化、循环、`switch`、`defer` 和 `#run` 编译期函数调用。现支持编译期块、声明监听器、注解中缀运算符及原位源码展开。浮点运算仍待实现。

原生 `string` 以字节计长，`std::string` 提供比较、查找、切片、拼接和显式 C 字符串转换。`std::list` 提供 `append(&items, x)`，分配使用函数隐式接收的 `Context`，默认复用 C 运行库。`#extern(C)` 支持已有类型的固定和可变参数调用、`puts` / `printf` 及文件读写。[字符串用例](tests/cases/native_string.ar) · [自定义分配器用例](tests/cases/context_list.ar) · [C 互操作](NEXT/parser_subset_v0_1.md#c-字符串与外部调用)

## 运行整数编译原型

在 Windows x64 安装 CMake、Ninja、Clang 和可用的 Windows SDK 链接环境，然后在 `Compiler/` 运行以下命令。`--exe` 会调用外部工具完成汇编和链接。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
.\build\atri-cc.exe --exe -o build/main.exe examples/main.ar
.\build\main.exe
$LASTEXITCODE # 3
```

`--emit-ast` 查看展开和改写后的语法树，`--emit-ir-before-opt` 查看编译期求值后的寄存器字节码，`-S` 输出汇编，`-c` 输出 COFF。默认 `-O0` 把字节码直接翻译为 x64 汇编。`-O1` 在 LLVM IR 上运行 ATRI 自研优化，依赖 Clang 同目录的 `LLVM-C.dll`。`-O2` 使用 LLVM 标准优化管线。[按模块阅读编译器](docs/README.md)

`--meta` 可重复指定，插件按参数顺序接收声明。下面的示例用 `@math` 插件改写运算符，执行编译期循环，并展开生成的源码。[示例源码、执行顺序与链路图](docs/metaprograms.md)

```powershell
.\build\atri-cc.exe --meta examples/meta/operators.ar --exe -o build/pipeline.exe examples/meta/pipeline.ar
.\build\pipeline.exe
$LASTEXITCODE # 11
```

普通变量写作 `a: i32 = 1;`，省略初始化器时默认零初始化；编译期常量写作 `n: i32 := add(1, 2);`，无法在编译期求值时必须报错。[常量示例](examples/constants.ar)在 -O0 下也直接返回 3，不依赖内联。全角分号 `；` 会报错。完整范围、已接受的整数运算规则和当前后端限制见[整数编译原型](NEXT/parser_subset_v0_1.md)。

打印示例导入 `basic` 模块并调用 C 函数 `puts`。[hello.ar](examples/hello.ar)

```powershell
.\build\atri-cc.exe --exe -o build/hello.exe examples/hello.ar
.\build\hello.exe
# import "basic"
```

`-o` 指定输出文件名，输出格式由所选模式决定。外部函数可用 `#extern(C)` 声明。[C 互操作规则](NEXT/language_spec_v0_1.md#13-c-互操作与执行环境)

数组写作 `data: [4]u8 = {1, 2};`，可声明在局部或模块顶层。省略的元素按类型默认初始化，多维数组支持 `mat[i, j]`。`a: i32 := #run max(1, 2);` 在 -O0/-O1 下均生成常量 2。`#run f();` 也可独立执行。详见[可执行回归用例](tests/cases/run_constants.ar)和[实现子集](NEXT/parser_subset_v0_1.md#固定数组的当前实现)。

整数转换使用 `byte: u8 = cast(u8, rgba);`，显式缩窄保留低位，扩宽按源类型符号/零扩展。`sizeof(T)`、`alignof(T)` 返回 u64 编译期值，也可用于数组长度。例子见 [conversions.ar](examples/conversions.ar)，准确能力与限制见[整数转换与布局查询](NEXT/parser_subset_v0_1.md#整数转换与布局查询)。

文件读取示例使用 [c_stdio.ar](examples/c_stdio.ar) 显式绑定 C 标准库：[read_file.ar](examples/read_file.ar) 分块读取并打印 examples/main.ar；[variadic.ar](examples/variadic.ar) 演示 `printf(format: *u8, ...)`。从 Compiler 目录执行：

```powershell
.\build\atri-cc.exe -S -o .\build\read_file.s .\examples\read_file.ar
clang .\build\read_file.s -o .\build\read_file.exe
.\build\read_file.exe
```

可变参数目前覆盖已支持整数、数据指针和 C 兼容普通结构体，i8/u8/i16/u16/bool 尾参提升为 i32；浮点、va_list/va_arg 定义端及完整 C ABI 仍待实现。FILE 只作 opaque 指针使用，CRT 的分配、缓冲和 I/O 有实际成本，文件错误及关闭由调用方处理。准确边界见[实现子集](NEXT/parser_subset_v0_1.md#c-可变参数opaque-file-与文件读取)。

`--emit-ir-before-opt` 输出完成编译期求值后的 IR。`a: i32 = #run max(1, 2);` 的初值在编译期求得，`a` 仍可在运行期赋值。使用 `:=` 可建立编译期常量。求值记录及生成代码视图仍待设计，见[编译期观察与调试](NEXT/language_spec_v0_1.md#125-观察与诊断)。

基础整数支持十进制、二进制、十六进制、数字分隔符、位运算、移位和复合赋值。单引号字节字面量为 `u8`，可直接参与算术或索引。双引号在 `string` 上下文生成按长度读取的字节视图，在 `*u8` 上下文生成带终止零的 C 字面量。`\xHH` 固定两位，浮点运算仍待实现。[基础类型示例](examples/basic_types.ar)

无标签 union 示例见 [union.ar](examples/union.ar)，使用 `union Word { bits: u32; bytes: [4]u8; }` 和 `{.bits = 0x12345678}`。成员按目标 C 自然布局共享存储，无隐含标签；支持成员读写、整体复制、按值调用和静态全局。默认/空初始化清零整个 union，成员声明默认值、匿名成员、位域和聚合编译期值暂未支持，准确边界见[实现子集](NEXT/parser_subset_v0_1.md#无标签-union-的当前实现)。

枚举示例见 [enum_switch.ar](examples/enum_switch.ar)：`Color :: enum { Red, Green, Count, }` 默认使用 i32，成员是普通整数，`1 + Color::Red`、数组长度和索引可直接使用；`Flags :: enum(bit8)` 自动生成 1、2、4，变量/计算结果仍允许 0。switch 选择值只求值一次，case 常量在编译期求得，非空分支默认结束，`#through;` 显式进入下一分支；case 不强制花括号。

`#run f(args)` 强制在编译期执行一次直接调用。`#run { ... }` 执行无结果块，`#run -> Type { ... }` 通过 `return` 交回值。`#insert #run emit(...)` 将生成的文本原位拼回源码并重新解析。[编译期规则](NEXT/language_spec_v0_1.md#12-编译期执行与-ast-元编程)

## 文档

- [Stage 0 编译器实现导读：模块、编译链路与关键算法](docs/README.md)
- [声明事件、编译期 VM 与 O0/O1/O2 原生输出](docs/metaprograms.md)
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

`defer` 支持块和直接调用，例如 `defer (ready) cleanup();`。每个作用域退出时逆序清理，轮到该条清理时计算条件和实参。外层 `return` 先求值再清理。详见[语言规范第 8.2 节](NEXT/language_spec_v0_1.md#82-defer)。

已审查确认：结构体字段统一使用点号定位，指针必须先显式解引用，写作 `(*p).field`；不支持 `p.field` 或箭头成员访问，点号自身不承担自动解引用。

已审查确认：数组当前采用 C 兼容布局和有限静态越界检查，不生成运行时越界检查；同一结构体初始化器重复指定同一字段为编译错误，即使值相同亦不允许。

已审查确认：BD-16 的声明顺序、自然对齐、无聚合自动相等及首版布局边界接受；空结构体大小改为 0、对齐保持 1。零大小对象的地址、指针操作、分配与 FFI 配套继续审查。DOP 定位不自动增加专用 SoA/SIMD 语法或改变既有 C 布局。

1. 整数环绕、cast 截断与非法运算边界（BD-06/07/25）。
2. 字节字符、原生 `string` 与 C 字符串均已实现。目标只读指针继续审查（BD-04/05/13）。
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

成功退出码为 0。结构体普通全局支持默认/指定静态初始化，包含嵌套结构体和字段数组，示例见 [struct_globals.ar](examples/struct_globals.ar)。标量叶子在编译期求值，直接生成静态数据；指针初值限 null，聚合常量和聚合调用/复制初值暂未支持。当前结构体边界及复制成本见[实现子集](NEXT/parser_subset_v0_1.md#结构体的当前实现)。
