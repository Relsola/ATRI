# 基础编译原型 v0.1

> 状态：Stage 0 已实现子集，2026-11-16。规则入口为[语言规范](language_spec_v0_1.md)，内部契约见 [IR 设计](ir_design_v0_1.md)。本文记录可运行行为，不冻结整门语言。

## 声明与调用

函数使用 `fn name(parameters) -> Type`，参数和局部声明使用 `name: Type`。先收集全部函数签名，允许调用后定义的函数。普通变量采用 `name: Type [= expression];`，可变并默认零初始化；`name: Type := expression;` 必须在编译期求值成功。旧 let/const 声明及 `name: Type : expression` 不接受；`::` 保留给模块限定访问，类型推导简写待定。

局部声明顺序可见，块内可遮蔽外层名称，同层重复报错；函数最外层与参数共享名称作用域。参数绑定只读，指针参数可写其目标。作用域和参数规则采用当前 BD-12 候选，仍待完整审查。

[main.txt](../examples/main.txt) 在 -O1 下内联 add(1, 2) 并折叠为 3；[constants.txt](../examples/constants.txt) 用 `:=` 强制求值，-O0 下也直接生成 `mov eax, 3; ret`。这里的 3 是返回值/进程退出码，不向标准输出打印字符。未引用的 add 定义仍保留。

## 当前句法

```ebnf
program    = { function }, EOF ;
function   = "fn", identifier, "(", [parameters], ")", "->", type, block ;
parameters = parameter, { ",", parameter } ;
parameter  = identifier, ":", type ;
type       = { "*" }, identifier ;
block      = "{", { statement }, "}" ;
statement  = declaration, ";" | assignment, ";" | call, ";"
           | "return", expression, ";" | block
           | "if", "(", expression, ")", block, [ "else", (block | if-statement) ]
           | "while", "(", expression, ")", block
           | "do", block, "while", "(", expression, ")", ";"
           | "for", "(", [for-init], ";", [expression], ";", [for-step], ")", block
           | "loop", block | "break", ";" | "continue", ";" ;
if-statement = "if", "(", expression, ")", block, [ "else", (block | if-statement) ] ;
declaration = identifier, ":", type, (":=", expression | ["=", expression]) ;
assignment = expression, "=", expression ;
for-init   = declaration | assignment | call ;
for-step   = assignment | call ;
expression = logical-or ;
logical-or = logical-and, { "||", logical-and } ;
logical-and = equality, { "&&", equality } ;
equality   = relational, { ("==" | "!="), relational } ;
relational = sum, { ("<" | "<=" | ">" | ">="), sum } ;
sum        = product, { ("+" | "-"), product } ;
product    = unary, { ("*" | "/" | "%"), unary } ;
unary      = ("+" | "-" | "&" | "*" | "!"), unary | primary ;
primary    = decimal-integer | "true" | "false" | "null" | identifier | call
           | "(", expression, ")" ;
call       = identifier, "(", [arguments], ")" ;
arguments  = expression, { ",", expression } ;
decimal-integer = digit, { digit } ;
identifier = identifier-start, { identifier-start | digit } ;
identifier-start = "A" ... "Z" | "a" ... "z" | "_" ;
digit = "0" ... "9" ;
```

`...` 只表示文法中的范围；以上是实现文法，赋值目标等限制由语义检查。所有二元层级左结合。一元运算、乘除余、加减、关系比较、相等比较、&&、|| 的优先级依次降低；求值按源码从左到右，&&/|| 短路。这些表达式细节沿用 BD-09 候选，不因实现而冻结该条其他内容。普通表达式不能作为语句，仅函数调用可以丢弃结果。

支持 ASCII 空白、LF/CRLF/CR 和文件开头可选 UTF-8 BOM；暂不支持注释、其他进制/数值分隔符、浮点、位运算、聚合、模块、全局绑定、外部调用或 run。全角分号、嵌入 NUL、不支持的字符会诊断。不接受尾逗号、逗号表达式、复合赋值、++/--。

## 类型与运算

- 可执行值类型为 i32、i64、bool 和一层 *i32。不做整数跨宽度、整数/指针、整数/bool 或指针/bool 隐式转换，不自动解引用。当前仍无显式转换语法。
- 十进制整数根据上下文的 i32/i64 检查范围，支持各自最小负值。无上下文的纯整数表达式默认 i32，指针偏移字面量默认 i64。
- 暂用用户授权的 BD-06 候选：整数加减乘及一元负号按位宽环绕；除法向零截断，余数随被除数符号。编译期实际执行或直线代码折叠遇到除零、INT_MIN/-1 会报错；当前多块函数尚无完整常量传播，不保证静态诊断覆盖所有此类表达式。动态非法运算没有运行期检查，仍按候选未定义行为处理。
- 同宽整数支持 ==、!=、<、<=、>、>=；bool 支持 ==、!=；*i32 支持 ==、!=，可与 null 比较。关系运算结果为 bool，bool/指针不支持排序。比较链不会解释成数学上的区间判断，`a < b < c` 因 bool 与整数不匹配而报错。
- !、&&、|| 只接受 bool。bool 默认 false；原型暂采用 1 字节对象、合法表示 0/1，内部临时值和 x64 调用使用规范化的 0/1。布局仍属于 BD-02 待审查部分。
- 指针支持 null、默认空初始化、取地址、显式读写、复制、参数/返回、按元素加减与 i64 元素距离。详情及有限空值/逃逸诊断见[整数指针子集](pointer_subset_v0_1.md)。没有动态空值、边界、生命周期检查或隐式分配。

## 控制流与作用域

BD-19 已接受并实现。if/else、while、do...while、for、loop 均为语句；条件严格为 bool，所有体必须使用 {}，包括空体。else if 各分支同样必须有块。for 的三个头部项均可省略，缺省条件为 true；初始化至多一条声明、赋值或调用，迭代项至多一条赋值或调用。

break 退出最近循环；continue 对 while/do 转到条件、for 转到 step 再检查条件、loop 回到开头。循环外使用 break/continue 报错；不支持标签目标或循环值。

for 初始化建立覆盖条件、迭代项及体的作用域，循环结束后不可见；体有自己的内层作用域，体内声明不能被 step 或 do 的条件读取。循环体内普通变量每次执行声明时重新初始化。

返回分析使用 CFG 可达性，所有可能到达函数末尾的路径必须返回值；不终止循环允许没有 return。当前识别直接 bool 常量和省略 for 条件，不进行完整条件证明；例如条件为整数比较或复合逻辑且未显式声明为编译期常量时，可能仍需补充 return。不可达源码继续检查名称、类型及所有 := 义务，之后删除由直接常量分支证明不可达的运行期块。

[control_flow.txt](../examples/control_flow.txt) 展示循环、分支、continue/break 及编译期递归，-O0/-O1 原生退出码均为 3。defer 与 match 尚未实现；清理规则仍按各自条目推进，本轮不默认加入延迟清理。

## 编译期求值

:= 可引用字面量、已有编译期常量并调用函数；初始化直接读取普通变量或参数时拒绝，即使优化器知道其值。被调函数可使用已知参数、内部可变整数/bool 局部变量、分支、循环、递归及短路逻辑。

独立 IR 解释器按控制流执行，不执行未选分支或短路右侧；不会仅追踪最终返回表达式而跳过已执行到的调用。所有显式常量声明均必须求值，包括不可达源码中的声明。错误、循环依赖、超出资源预算必须报错，不回退运行期。单次编译预算暂为 100000 步、递归/依赖深度 256；这是实现限制。

常量解释器支持内部整数/bool 局部存储，仍拒绝原始指针值、取地址、指针算术和间接访存。此内部存储能力不是完整的编译期地址模型或 run/AST 修改系统。

## AST、IR 与后端

AST 使用平坦数组和稳定索引，块另有语句索引边；名称、类型名和字面量保留原始源码半开区间。源码寿命必须长于 AST/IR，通过 destroy API 释放数组。

名称/类型检查与 IR 降低同一遍进行；全部函数体检查后完成强制常量求值、验证及已知不可达块删除。IR 已有多基本块、跳转、条件分支、比较、局部对象与 load/store。控制流函数的普通可变局部使用存储衔接分支与回边；尚无 phi、mem2reg 或跨块内联/常量传播。直线代码仍保留原有传播/折叠/内联，避免跨分支复用错误的旧值。

Windows x64 后端输出 Clang 集成汇编器使用的 Intel 语法 .s，负责寄存器/栈参数、调用空间、栈对齐、展开元数据。非 main 符号暂名为 atri_fn_<索引>，不是最终 FFI 名称。语言没有异常；目标展开元数据服务于 ABI/系统工具，不引入语言异常机制。

非恒定临时值暂存栈槽，尚无寄存器分配；不承诺已达到最终零开销目标。当前限制：栈帧小于 4096 字节、每次调用至多 256 个实参、控制流至多 4096 块、表达式和语句嵌套至多 256。目标调用约定的依据见[Microsoft 文档](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)。

## 构建与验证

Windows x86-64，Clang 23.1.2、C23、Ninja：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
.\build\atri-parse.exe .\examples\control_flow.txt
.\build\atri-cc.exe --emit-ir-before-opt .\examples\control_flow.txt
.\build\atri-cc.exe -S -o .\build\flow.s .\examples\control_flow.txt
clang .\build\flow.s -o .\build\flow.exe
.\build\flow.exe
$LASTEXITCODE # 3
```

atri-parse 默认输出 AST，atri-cc 默认输出汇编；两者支持 --emit-ast、--emit-ir-before-opt、--emit-ir、-S、-o，文件名 - 读取标准输入。-O1 默认对直线函数实施折叠、DCE 和小函数内联；-O0 保留运行期调用但仍进行直线局部折叠/DCE。显式常量与优化等级无关。--emit-ir-before-opt 已经完成强制常量求值。退出码 0 成功、1 编译错误、2 命令行/I/O 错误。

CTest 五组：前端、原编译器回归、控制流语义/常量/Verifier、CLI 示例、Windows 原生执行。原生测试在 -O0/-O1 下汇编、链接并执行整数、常量、指针和控制流示例，另以 C 驱动验证数组指针遍历、嵌套循环、短路副作用、六个 bool 参数、i64 比较与局部存储、终止递归等。需要本机 Clang、Windows SDK/链接工具，不自动安装依赖。
