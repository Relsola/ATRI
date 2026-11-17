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

## 数组与 run 回归

[cases/arrays_run.ar](cases/arrays_run.ar) 在 -O0/-O1 下编译、链接、原生执行，覆盖局部长度常量/遮蔽、数组默认/部分/嵌套初始化、初始化求值顺序、复制、参数/返回、数组指针、嵌套索引与字段写入、零大小元素、u8 环绕/无符号除余和短路条件表达式。结构化例子与运行期调用副作用用于验证实际路径，不把 IR 常量断言当作机器执行结果。

[run_constants.ar](../examples/run_constants.ar) 验证用户的 max 示例：两种优化等级下均只输出 main，直接返回 2。compiler 测试另核对优化前 IR 已无运行期调用，run 不得读取运行期变量/参数或执行 extern(C)，以及条件表达式未选分支不执行。run 声明支持 `=` 可变绑定和 `:=` 常量绑定，单独的 run 表达式支持整数/bool；arrays_run 原生用例覆盖顶层/局部可变结果写入和 for 初始化，frontend/compiler 检查完整初始化器范围、缺失初始化器及非法编译期输入。代码块、AST API 与聚合求值不在本轮验收内。

[array_harness.c](array_harness.c) 对 [array_abi.ar](cases/array_abi.ar) 做双向 C 调用，核对 u8 的寄存器/栈参数、返回与指针写入，以及内嵌数组的 4/24 字节结构体布局、参数和返回。测试绑定生成器按元素类型与维度生成 C 字段声明，不伪造 C 按值数组参数。

compiler/front-end 测试覆盖索引静态越界、零长度访问、初始化过量/错误形式、只读参数、临时值写入、数组类型不匹配、运行期长度、递归布局及字节范围。Verifier 另构造错误索引类型/引用和循环数组描述，不能用 native 用例代替这些不变量检查。当前无动态越界检查、性能测量或复制最少证明。

## 整数转换与目标布局查询回归

[conversions.ar](../examples/conversions.ar) 和 [conversion_harness.c](conversion_harness.c) 在 -O0/-O1 下编译、链接并原生执行。驱动调用 [cases/conversions.ar](cases/conversions.ar)，核对负 i32 的符号扩展、u8 零扩展、低位截断、同宽符号解释、u64 最高位/最大值的加减乘负号、无符号排序与除余、嵌套转换及 C 寄存器/栈参数和返回。示例另核对零大小结构体/数组及目标自然对齐，不把零大小类型伪造为 C 测试绑定。

compiler/front-end 测试核对 `cast(Type, expression)` 的类型和 AST、静态常量/标量 run 结果、无隐式转换、禁用 bool 整数/指针整数转换、字面量范围、旧语法拒绝及 sizeof/alignof 的完整对象类型要求。布局查询参与数组长度，按值循环和嵌套查询资源耗尽必须诊断；Verifier 另构造错误目标类型与自引用 cast。测试通过不表示已支持其余整数位宽、*u64、浮点转换、额外 cast 模式或完整 C ABI，也不代表性能测量。

## C 可变参数、void 与标准库文件回归

[variadic_harness.c](variadic_harness.c) 对 [cases/c_variadic.ar](cases/c_variadic.ar) 在 -O0/-O1 下做真实 C va_arg 验证，覆盖 u8/bool 默认提升、i32/i64/u64、数据指针、小/大结构体、寄存器与栈尾参、隐藏返回参数平移、void 调用/返回和实参副作用顺序。独立编译的 C 驱动使用 C23，以允许纯 ellipsis 声明。默认提升有单独 IR 指令，不将 bool 整数转换开放为普通语言行为；Verifier 构造未提升尾参、错误 promotion/void return 与非法 opaque 布局。

文件用例在本次 OUTPUT_DIR 下创建和删除测试文件：由 C 写入 257 字节（含 NUL、CR/LF 和高位字节），由 ATRI 使用 fopen/fread 分块读取并验证返回计数、EOF/ferror、缺失文件及 fclose，再由 ATRI fwrite 写回 4 字节交由 C 检查。数据文件不写入源码或仓库根。[read_file.ar](../examples/read_file.ar) 的 stdout 与读取的源码文本核对；[variadic.ar](../examples/variadic.ar) 的 printf stdout 为 `255 1 -7 18446744073709551615 ok` 加换行。

前端和编译器诊断覆盖 ellipsis 位置/定义端拒绝、固定前缀数量/类型、数组与零大小尾参、opaque 对象使用、void 对象/返回使用、*void 访问/算术及数据指针 cast 的有限 null/局部地址返回检查。原有尖括号/C 式转换和指针整数转换仍不接受，运行期 C 调用也不能在 run 中执行。本轮不覆盖浮点尾参、va_list 定义端、printf 格式静态校验、宽字符路径或完整 C ABI。
