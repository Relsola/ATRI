# 测试写法

从 Compiler 目录运行：

```powershell
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -R optimization -V
```

CTest 会为读取用例文件的测试设置工作目录。原生测试需要 Windows x64、Clang 和链接工具。

## 先读这些文件

| 要看什么 | 入口 |
|---|---|
| 运行结果与副作用 | [control_flow_harness.c](control_flow_harness.c)、[pointer_harness.c](pointer_harness.c) |
| 优化是否发生 | [optimization_tests.c](optimization_tests.c) |
| 被编译的完整源码 | [cases/optimization](cases/optimization)、[cases/constants](cases/constants) |
| 编译错误 | [compiler_tests.c](compiler_tests.c)、[control_flow_tests.c](control_flow_tests.c) |
| 前端结构与错误位置 | [frontend_tests.c](frontend_tests.c) |
| 故意构造错误 IR | [verifier_tests.c](verifier_tests.c) |

## 结果测试

使用“期望值在前、实际表达式在后”的写法：

```c
EXPECT_EQ(120, atri_test_factorial(5));
EXPECT_EQ(8, atri_test_for_sum(5));
EXPECT_TRUE(atri_test_is_null(NULL));
EXPECT_PTR_EQ(values + 3, atri_test_add32(values, 3));
```

表达式只执行一次；需要验证写入时，把返回值与内存结果分开：

```c
EXPECT_EQ(42, atri_test_write(&x, 42));
EXPECT_EQ(42, x);
```

失败报告包含文件/行号、表达式、期望和实际值。代码见 [native_expect.h](native_expect.h)。

`atri_test_factorial` 来自源码中的 factorial。构建时 [generate_bindings.c](generate_bindings.c) 使用前端解析源码，根据函数签名自动生成 C 声明及到后端符号的映射；调整函数顺序不需要手改测试编号。生成文件位于 build/native-tests，只为普通定义生成测试映射，跳过 extern(C) 声明；实际外部调用直接使用 C 符号，规则见[实现子集](../NEXT/parser_subset_v0_1.md#c-字符串与外部调用)。

[native_tests.cmake](native_tests.cmake) 只列源码和期望返回值，统一在 -O0/-O1 下生成汇编、链接并执行；用于动态参数、指针和调用约定的函数库配有 C 驱动。递归环等故意不终止的用例仅做 IR 检查，不直接执行。

带第三个参数的 run_program 项还核对真实 stdout：输出重定向到文件，按字节读取，仅规范化 CRLF/LF，并要求成功退出且 stderr 为空。[c_strings.ar](cases/c_strings.ar) 验证 UTF-8 字节、空串、转义、内嵌零截断和字面量返回；这不等于验证当前终端代码页下的中文显示。[c_abi.ar](cases/c_abi.ar) 与 [c_abi_harness.c](c_abi_harness.c) 验证双向调用中的寄存器/栈参数、64 位值、字符指针/布尔返回和外部写入；使用的是普通定义的测试映射，不是正式导出语法。

原生测试同时核对误用 `-o output.exe` 的诊断：包括混合大小写扩展名，要求退出码 2、给出汇编/链接命令，并保持已有输出文件内容不变。

## 优化效果测试

源码放在独立文件，测试只描述变化和预期：

```c
Compilation test = test_compile_file("cases/optimization/inline_add.txt", TEST_O1);

EXPECT_CALL(test.before, "main", "add");
EXPECT_NO_CALLS(test.after, "main");
EXPECT_RETURN_I32(test.after, "main", 3);

test_destroy(&test);
```

before 是完成语义检查和强制常量求值、尚未可选优化的 IR；after 应用指定优化等级。两份 IR 独立保存。检查按函数名、操作种类和返回值关系进行，不固定指令数组下标或 SSA 编号。

EXPECT_RETURN_I32/I64/BOOL 要求函数存在返回，且每个保留的 return 都引用相应类型的指定常量；它不是“执行后返回这个值”的断言。实际执行由原生测试另外验证。

- 普通函数内联：优化前有调用，-O1 后无调用并返回常量；-O0 保留调用。
- := 常量调用：before 中已经没有运行期调用，-O0/-O1 都返回指定常量。
- 无用计算：相关操作消失。
- 可观察行为：指针写入和必要调用仍保留，并以原生执行验证写入结果。
- 后端目标：单独检查 main 的直接返回汇编及函数定义数量，不比较整个输出文件。main/inline_add 在 -O1 下只剩 main，-O0 保留 add；:= 的辅助函数在两种等级下均不输出。unreachable_functions.ar 验证不可达递归环及其未提供的 C 依赖消失，同时保留 main 的传递调用与可达递归；链接并执行应返回 7。没有 main 的库源码继续由 C 驱动验证全部可调用定义。

这些检查定义测试场景要达到的效果，不规定优化器必须采用哪种算法。失败报告给出用例、函数、阶段、期望/实际及该阶段 IR。

## 编译失败与内部测试

```c
EXPECT_PARSE_ERROR("fn f()->i32{if(true)return 1;}", "require braces");
EXPECT_COMPILE_ERROR("fn f()->i32{return true;}", "type mismatch");
EXPECT_OPT_ERROR("fn f()->i32{return 1/0;}", "by zero");
```

分别要求语法阶段拒绝、语义阶段拒绝、或允许进入优化阶段再拒绝；同时检查诊断关键词和源码范围。不要把任意失败都当成预期错误。

AST 结构检查保留在前端测试；跳转目标、支配关系、访存类型等错误 IR 的构造保留在 Verifier 专项测试。普通结果和优化用例不承担这些内部细节。

新增测试时，简单边界可以直接写短表达式；有循环、多个函数或副作用的用例优先放入 cases 中，使用正常缩进的完整源码。共享辅助代码在 [test_support.h](test_support.h) 和 [test_support.c](test_support.c)。

## 模块与共享全局回归

[cases/modules](cases/modules) 由 native-execution 在 -O0/-O1 下编译、链接并执行。module main 覆盖重复路径/别名、跨文件共享全局、值快照、bool/i64/指针全局、全局地址返回及静态常量调用；peer 中的同名私有函数检验定义模块名称解析。循环模块用例验证声明发现和运行期跨模块递归。诊断用例要求重名导入、私有访问、编译期常量写入、运行期全局依赖初始化和缺失文件均由编译器报错。hello.ar 预期输出为 `import "basic"` 并返回 0。这些是行为证据，不是解析性能测量。

## 结构体回归

[struct.ar](../examples/struct.ar) 和 [cases/structs](cases/structs) 覆盖声明默认值、指定/空/嵌套初始化、字段赋值和取地址、指针递归、值快照、参数/返回、名义类型及模块类型导出。C 驱动 [struct_harness.c](struct_harness.c) 通过生成绑定调用 1/2/3/4/8/16/24/32 字节结构体，并验证 C 到 ATRI 及 ATRI 到 C 的寄存器/栈实参、隐藏返回指针、多个实参副本独立性和自然布局。两种 ATRI 优化等级均编译、链接并执行。

失败用例验证重名字段/初始化项、未知字段、按值布局循环、不同名义类型、不写只读参数、不自动解引用、不返回局部字段地址、C 零大小聚合拒绝和非法声明默认值。Verifier 另构造越界字段引用、错误字段类型与错位布局。这里只证明已覆盖的语义和 ABI 行为，没有测量性能或证明复制最少。

[struct_globals.ar](../examples/struct_globals.ar) 验证普通结构体全局的默认/指定静态初始化和整体复制。[globals.ar](cases/structs/globals.ar) 与 [struct_globals_harness.c](struct_globals_harness.c) 在 -O0/-O1 下验证嵌套结构体、字段数组、编译期标量调用、符号/高位整数、null 指针、自然对齐、值快照及 C 指针写入；[global_modules.ar](cases/structs/global_modules.ar) 验证重复导入共享对象、导出类型及字段默认值在定义模块解析，[global_empty.ar](cases/structs/global_empty.ar) 验证零大小全局和无显式项的巨大零大小子树。

compiler 专项核对最终 IR 的目标初始字节及临时函数已删除，并拒绝聚合调用/复制初值、运行期依赖、非零静态指针和非法字段/数组项。Verifier 专项检查错误缓冲区长度、bool/指针表示和非法聚合常量标记。

## 数组与 #run 回归

[cases/arrays_run.ar](cases/arrays_run.ar) 在 -O0/-O1 下编译、链接、原生执行，覆盖局部长度常量/遮蔽、数组默认/部分/嵌套初始化、初始化求值顺序、复制、参数/返回、数组指针、嵌套索引与字段写入、零大小元素、u8 环绕/无符号除余和短路条件表达式。结构化例子与运行期调用副作用用于验证实际路径，不把 IR 常量断言当作机器执行结果。

[array_globals.ar](cases/array_globals.ar) 在两种优化等级下验证静态初始化、模块共享和多维索引，包含求值顺序、一次求值、子数组赋值及元素取地址。compiler 专项检查静态初始字节和每一维的诊断。

[run_constants.ar](cases/run_constants.ar) 在两种优化等级下均只生成 `main` 并直接返回 2。compiler 测试核对优化前 IR 的调用清除、编译期输入限制，以及条件未选分支的求值行为。arrays_run 原生用例覆盖初始化器内的 `#run`、对具有编译期初值的普通变量的写入和 `for` 初始化。

[array_harness.c](array_harness.c) 对 [array_abi.ar](cases/array_abi.ar) 做双向 C 调用，核对 u8 的寄存器/栈参数、返回与指针写入，以及内嵌数组的 4/24 字节结构体布局、参数和返回。测试绑定生成器按元素类型与维度生成 C 字段声明，不伪造 C 按值数组参数。

## 延迟清理回归

[defer_cleanup.ar](cases/defer_cleanup.ar) 在 -O0/-O1 下验证四种语法、条件和实参的清理时求值、逆序、名字遮蔽、嵌套 defer、标量及聚合返回值，以及 `continue`、`break` 与 `#through;` 的清理边界。control-flow 专项检查编译期调用内的清理和非法语法、条件类型、不可见名称及跨边界跳转的诊断。

compiler/front-end 测试覆盖索引静态越界、零长度访问、初始化过量/错误形式、只读参数、临时值写入、数组类型不匹配、运行期长度、递归布局及字节范围。Verifier 另构造错误索引类型/引用和循环数组描述，不能用 native 用例代替这些不变量检查。当前无动态越界检查、性能测量或复制最少证明。

## 编译器指令、枚举与 switch 回归

`#run` 解析回归接受直接函数调用并拒绝表达式形式。`switch` 回归继续验证普通 `case` 常量表达式和 `case #run f():`，以及求值后的重复标签诊断。

[多级指针原生用例](cases/multi_level_pointers.ar)在 -O0/-O1 下与 C 双向传参和返回，核对指针槽的别名改写、三级解引用、偏移与差值、指针转换、多级指针的全局和字段布局、opaque 指针槽与数组目标。Verifier 专项检查解引用层数不匹配的 IR。

frontend 专项核对 #run/#extern 的前缀、# 与名称间空白、未知/重复指令、旧语法拒绝，以及两种 #run 块形式的未实现诊断。compiler 专项验证顶层和函数内的独立调用、丢弃结果和 `void` 调用，以及独立调用仍报告编译期求值错误。原生回归使用 [cases/run_constants.ar](cases/run_constants.ar)，并检查只产生 `main`。代码块形式的示例见[语言规范](../NEXT/language_spec_v0_1.md#121-能力边界)。

[enum_switch.ar](../examples/enum_switch.ar) 与 [enum_switch_harness.c](enum_switch_harness.c) 对 [cases/enum_switch.ar](cases/enum_switch.ar) 在 -O0/-O1 下验证成员自动/显式值、各整数符号/高位表示、位组合及零值、直接计算/数组长度/索引、整数别名的 C 参数/返回/字段/可变参数提升和静态全局。分支验证一次求值、空标签组、默认不贯穿、显式 #through、嵌套词法块、独立 case 作用域、嵌套 switch 与循环的 break/continue；[enum_modules.ar](cases/enum_modules.ar) 验证直接及命名导入、三段限定成员名和共享存储。

compiler 专项核对编译期函数内 switch、case 常量函数/显式 #run、求值后重值诊断、成员名/值重定义、范围/位宽溢出、位枚举成员禁止零/复合位和运行期 case 输入拒绝。Verifier 专项破坏枚举元数据的表范围、底层类型、就绪状态及成员位模式。不包含 Payload、函数型成员初值、跳转表、完整 #run 块或分派性能验收。

## union 回归

[union.ar](../examples/union.ar) 验证整数/字节跨成员读取、成员写入重叠、数组元素、指针及值副本；[cases/unions/modules.ar](cases/unions/modules.ar) 验证 union 类型导出和重复导入共享全局。

[cases/unions/abi.ar](cases/unions/abi.ar) 与 [union_harness.c](union_harness.c) 在 -O0/-O1 下双向调用 C，覆盖 1/2/3/4/8/12/16 字节 union 的寄存器/栈参数和返回、隐藏返回参数平移、间接实参副本独立性、C 可变参数尾参、自然对齐、union 内嵌结构体/数组及结构体内嵌 union。C 驱动使用生成的真正 union 定义，并独立断言大小、对齐和成员零偏移；同时验证全局静态字节、合法指针成员复制、C 指针写入、opaque/递归指针及零存储与显式选择成员的默认值差异。

frontend/compiler 专项覆盖语法、名义类型、静态初值、单成员限制、未知/重复成员、按值布局循环、非法成员默认值、聚合常量/编译期执行拒绝；Verifier 构造错误 union 偏移/大小/对齐及非法默认值元数据，并确认重叠存储不被同时按 bool/指针验证，外围普通 bool 仍受检查。测试只读取合法表示，不将非法 bool/指针模式作为定义良好的行为；没有证明完整 C ABI、复制消除或性能最优。

## 整数转换与目标布局查询回归

[conversions.ar](../examples/conversions.ar) 和 [conversion_harness.c](conversion_harness.c) 在 -O0/-O1 下编译、链接并原生执行。驱动调用 [cases/conversions.ar](cases/conversions.ar)，核对负 i32 的符号扩展、u8 零扩展、低位截断、同宽符号解释、u64 最高位/最大值的加减乘负号、无符号排序与除余、嵌套转换及 C 寄存器/栈参数和返回。示例另核对零大小结构体/数组及目标自然对齐，不把零大小类型伪造为 C 测试绑定。

compiler/front-end 测试核对 `cast(Type, expression)` 的类型和 AST、静态常量/标量 #run 结果、无隐式转换、禁用 bool 整数/指针整数转换、字面量范围、旧语法拒绝及 sizeof/alignof 的完整对象类型要求。布局查询参与数组长度，按值循环和嵌套查询资源耗尽必须诊断；Verifier 另构造错误目标类型与自引用 cast。其余定宽整数和 *u64 已由后续基础类型回归覆盖；测试通过不表示已支持浮点转换、额外 cast 模式或完整 C ABI，也不代表性能测量。

## C 可变参数、void 与标准库文件回归

[variadic_harness.c](variadic_harness.c) 对 [cases/c_variadic.ar](cases/c_variadic.ar) 在 -O0/-O1 下做真实 C va_arg 验证，覆盖 u8/bool 默认提升、i32/i64/u64、数据指针、小/大结构体、寄存器与栈尾参、隐藏返回参数平移、void 调用/返回和实参副作用顺序。独立编译的 C 驱动使用 C23，以允许纯 ellipsis 声明。默认提升有单独 IR 指令，不将 bool 整数转换开放为普通语言行为；Verifier 构造未提升尾参、错误 promotion/void return 与非法 opaque 布局。

文件用例在本次 OUTPUT_DIR 下创建和删除测试文件：由 C 写入 257 字节（含 NUL、CR/LF 和高位字节），由 ATRI 使用 fopen/fread 分块读取并验证返回计数、EOF/ferror、缺失文件及 fclose，再由 ATRI fwrite 写回 4 字节交由 C 检查。数据文件不写入源码或仓库根。[read_file.ar](../examples/read_file.ar) 的 stdout 与读取的源码文本核对；[variadic.ar](../examples/variadic.ar) 的 printf stdout 为 `255 1 -7 18446744073709551615 ok` 加换行。

前端和编译器诊断覆盖 ellipsis 位置/定义端拒绝、固定前缀数量/类型、数组与零大小尾参、opaque 对象使用、void 对象/返回使用、*void 访问/算术及数据指针 cast 的有限 null/局部地址返回检查。原有尖括号/C 式转换和指针整数转换仍不接受，运行期 C 调用也不能在 #run 中执行。本轮不覆盖浮点尾参、va_list 定义端、printf 格式静态校验、宽字符路径或完整 C ABI。


## 基础类型与运算符回归

[basic_types.ar](cases/basic_types.ar) 与 [basic_types_harness.c](basic_types_harness.c) 在 -O0/-O1 下编译、链接并原生执行：C 动态实参覆盖八种整数的环绕加法、除余、按位操作/符号与逻辑移位、扩展/截断；窄指针写入后核对相邻元素未被覆盖，检查各步长的偏移/差值。另验证新类型的数组/字段/全局/默认值、取地址、全部复合赋值及其求值顺序、u8/bool 指针、混合字段布局、双向 C 固定签名/隐藏返回和 C varargs 默认提升。

字节字面量用例对比传给 C 的实际字节（固定两位十六进制、独立零字节转义、内嵌零、额外终止零），高位字节按 u8 解释为 255；另通过 i8 验证目标 C plain char 的有符号标量 ABI。字符减法、与数字运算和数组索引复用普通 u8；不再提供 char 语法。

frontend/compiler 覆盖进制与分隔符、范围/负最小值、同类型要求、非法移位、窄负数不能作为数组长度、短路未执行路径、只允许指针与 null 比较、u8 字符运算/直接索引、char 类型拒绝及八进制转义拒绝；Verifier 另检查窄常量高位及异型移位数量。用例区分 IR 常量断言与真实机器执行，未测性能或证明复制/访存已最少。
