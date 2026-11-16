# 基础编译原型 v0.1

> 状态：Stage 0 已实现子集，2026-11-17。规则入口为[语言规范](language_spec_v0_1.md)，内部契约见 [IR 设计](ir_design_v0_1.md)。本文记录可运行行为，不冻结整门语言。

## 声明与调用

函数使用 `fn name(parameters) -> Type`，参数和局部声明使用 `name: Type`。先收集全部函数签名，允许调用后定义的函数。普通变量采用 `name: Type [= expression];`，可变并默认初始化（结构体字段默认值规则见下文）；`name: Type := expression;` 必须在编译期求值成功。旧 let/const 声明及 `name: Type : expression` 不接受；类型推导简写待定。`::` 的模块绑定与限定访问设计见[语言规范第 11 节](language_spec_v0_1.md#11-模块与名称空间)，当前最小实现范围见下节。

局部声明顺序可见，块内可遮蔽外层名称，同层重复报错；函数最外层与参数共享名称作用域。参数绑定只读，指针参数可写其目标。作用域和参数规则采用当前 BD-12 候选，仍待完整审查。

[main.ar](../examples/main.ar) 在 -O1 下内联 add(1, 2) 并折叠为 3；[constants.ar](../examples/constants.ar) 用 `:=` 强制求值，-O0 下也直接生成 `mov eax, 3; ret`。这里的 3 是返回值/进程退出码，不向标准输出打印字符。汇编只保留 main 及优化后从 main 可达的函数，已无运行期引用的 add 不再输出；-O0 下 main.ar 的 add 调用和定义仍保留。

## 模块与全局的当前实现

CLI 支持 `import "path";` 直接引入 export 名称和 `name :: import "path";` 命名模块绑定；模块中的函数、结构体类型和标量全局默认私有，export 使它们可被导入。路径相对导入方文件解析，省略 `.ar` 扩展名，使用 `/`、`.`、`..`；不支持绝对导入、路径转义、包搜索或再导出。Windows 按文件最终路径识别符号链接/目录链接，再去重加载；当前文件接口使用 Windows 窄字符 API，非当前系统编码可表示的路径尚未验证。导入依赖深度上限 256。

一次编译中源模块只加载/解析一次。加载器先扫描依赖，再在保留文件边界的源码存储中解析 AST；各模块保持独立作用域，不是 include 文本拼贴。公开名称表建立后排序并二分查找；模块名称查找不生成运行时代码。没有跨次构建的接口缓存或增量失效，冷启动仍读取和检查所加载模块的完整源码，不据此宣称比 C 头文件更快。

顶层允许 `export name: Type [= expression];`、私有普通变量及 `name: Type := expression;`。当前全局类型为 i32/i64/bool/*i32/*char 及结构体指针；整数/bool 初始化器必须在编译期求值，可引用编译期常量、调用可解释执行的函数。指针静态初始化只支持 null 或省略初始化器；运行期可赋其他合法指针值。支持全局 i32 取地址、通过返回的指针读写；不支持聚合或非零指针静态重定位初始化。全局编译期常量仍不支持指针常量。

导入访问指向原全局对象；普通赋值复制值。导出不加入锁、原子指令或运行期名称查找。所有模块先收集声明后检查函数体，因此跨模块递归可用；编译期依赖环及非法访问仍报错。CLI 错误按原模块路径、行列显示。当前一次生成包含各模块所需代码和全局数据的 `.s`，外部 Clang 完成汇编/链接；尚无独立模块接口/目标文件输出。

## 结构体的当前实现

支持 `struct Name { field: Type [= expression]; ... }`，定义后可有分号；类型按声明身份区分，不采用结构相等。先收集类型再计算布局，允许后定义类型及指针递归，拒绝按值布局循环。字段按声明顺序自然对齐，支持 i32/i64/bool、已有指针、内嵌结构体及 `*T`。大小必须小于 4096 字节，字段最多 4096 个，按值嵌套深度最多 256；实际函数栈帧仍须小于 4096 字节。

变量沿用 `value: Type`。支持 `{}`、`{.field = expression}`、嵌套指定初始化及初始化项尾逗号；重复字段和未知字段均报错。显式项按源码顺序求值；省略字段使用声明默认值，其余字段按类型默认初始化，内嵌结构体递归应用其默认值。当前声明默认值仅支持可在编译期求值的标量表达式，指针默认值限 null；所有声明默认值均检查，不能用显式初始化覆盖来隐藏非法默认表达式。

结构体赋值、实参和返回均为值副本；指针字段只复制指针值。参数保持只读，需要修改时复制到局部变量；通过指针访问的原对象仍可写。`obj.field` 和 `(*p).field` 可读写，禁止 `p.field` 自动解引用；支持可写 i32/结构体字段取地址。临时结构体返回值的字段可读、不可赋值。尚无自动结构体相等比较。

支持 `export struct`、直接及命名导入类型、参数和返回中的 `module::Type`。C 按值 ABI 已通过原生交叉调用验证：1/2/4/8 字节结构体作为同宽整数传递/返回，其余正大小结构体参数使用调用方独立的 16 字节对齐副本；返回使用隐藏的首个缓冲区参数并在 RAX 返回该地址。后续参数位置随隐藏参数平移。每个间接实参都有独立副本，不把同一只读值重复传参时的缓冲区共享给 C。依据见 [Microsoft x64 调用约定](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)。

空结构体大小 0、对齐 1；当前内部调用占一个实参位置，参数/结果不复制内容，返回寄存器置零。这个内部约定尚非稳定 C 接口，extern(C) 按值拒绝空结构体及含零大小成员的结构体；零大小目标指针算术报错。

本轮未实现结构体全局、:= 结构体常量、聚合字段默认表达式、数组/union/enum、packed/自定义对齐或完整生命周期证明。含结构体/字段位置操作的函数暂不套用原单块优化与内联；当前会保留额外的栈快照和复制，没有隐式堆分配，但不能将语义正确或 ABI 通过当成复制已消除或性能已达标。示例为 [struct.ar](../examples/struct.ar)，验证入口见测试说明。

## 当前句法

```ebnf
program    = { function | import | global | struct }, EOF ;
struct     = [ "export" ], "struct", identifier, "{", { field }, "}", [ ";" ] ;
field      = identifier, ":", type, [ "=", expression ], ";" ;
import     = [ identifier, "::" ], "import", string-literal, ";" ;
global     = [ "export" ], identifier, ":", type, [ ("=" | ":="), expression ], ";" ;
function   = [ "export" ], "fn", signature, block
           | "[[", "extern", "(", "C", ")", "]]", "fn", signature, ";" ;
signature  = identifier, "(", [parameters], ")", "->", type ;
parameters = parameter, { ",", parameter } ;
parameter  = identifier, ":", type ;
type       = { "*" }, identifier, [ "::", identifier ] ;
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
unary      = ("+" | "-" | "&" | "*" | "!"), unary | postfix ;
postfix    = primary, { ".", identifier } ;
initializer = "{", [ ".", identifier, "=", expression, { ",", ".", identifier, "=", expression }, [ "," ] ], "}" ;
primary    = decimal-integer | string-literal | "true" | "false" | "null" | name | call | initializer
           | "(", expression, ")" ;
name       = identifier, [ "::", identifier ] ;
call       = name, "(", [arguments], ")" ;
arguments  = expression, { ",", expression } ;
decimal-integer = digit, { digit } ;
identifier = identifier-start, { identifier-start | digit } ;
identifier-start = "A" ... "Z" | "a" ... "z" | "_" ;
digit = "0" ... "9" ;
```

`...` 只表示文法中的范围；以上是实现文法，赋值目标等限制由语义检查。所有二元层级左结合。一元运算、乘除余、加减、关系比较、相等比较、&&、|| 的优先级依次降低；求值按源码从左到右，&&/|| 短路。这些表达式细节沿用 BD-09 候选，不因实现而冻结该条其他内容。普通表达式不能作为语句，仅函数调用可以丢弃结果。

支持 ASCII 空白、LF/CRLF/CR 和文件开头可选 UTF-8 BOM；支持 // 行注释；暂不支持块注释、其他进制/数值分隔符、浮点、位运算、数组/union/enum 或 run。全角分号、源码嵌入 NUL、不支持的字符会诊断。指定结构体初始化之外不接受尾逗号；不接受逗号表达式、复合赋值、++/--。

## 类型与运算

- 可执行值类型为 i32、i64、bool、名义结构体以及一层 *i32、*char、*T。不做整数跨宽度、整数/指针、整数/bool、指针/bool 或不同目标指针的隐式转换，不自动解引用。当前仍无显式转换语法。
- 十进制整数根据上下文的 i32/i64 检查范围，支持各自最小负值。无上下文的纯整数表达式默认 i32，指针偏移字面量默认 i64。
- 暂用用户授权的 BD-06 候选：整数加减乘及一元负号按位宽环绕；除法向零截断，余数随被除数符号。编译期实际执行或直线代码折叠遇到除零、INT_MIN/-1 会报错；当前多块函数尚无完整常量传播，不保证静态诊断覆盖所有此类表达式。动态非法运算没有运行期检查，仍按候选未定义行为处理。
- 同宽整数支持 ==、!=、<、<=、>、>=；bool 支持 ==、!=；*i32/*char 支持同类型的 ==、!=，可与 null 比较。关系运算结果为 bool，bool/指针不支持排序。比较链不会解释成数学上的区间判断，`a < b < c` 因 bool 与整数不匹配而报错。
- !、&&、|| 只接受 bool。bool 默认 false；原型暂采用 1 字节对象、合法表示 0/1，内部临时值和 x64 调用使用规范化的 0/1。布局仍属于 BD-02 待审查部分。
- 这些指针支持 null、默认空初始化、复制、参数/返回；正大小目标支持按元素加减与 i64 元素距离，取地址与显式目标读写支持 i32/结构体。详情及有限空值/逃逸诊断见[指针子集](pointer_subset_v0_1.md)。没有动态空值、边界、生命周期检查或隐式分配。

## C 字符串与外部调用

[hello.ar](../examples/hello.ar) 声明 `[[extern(C)]] fn puts(text: *char) -> i32;`，导入 basic.ar，写入共享全局 TC，再调用导出的 add；结果为 6 时向标准输出打印 `import "basic"` 并返回 0。属性内部允许空白与换行；当前只支持 extern(C)，声明必须以分号结束，不接受函数体、重复声明、未知属性或其他调用约定。

字符串字面量的类型为 *char，降低为 IR 自有字节数据和符号引用，最终放入 .rdata；返回字面量地址不会悬空，也不会产生运行期堆分配。编码、转义、零结尾、内嵌零及禁止写入契约统一见[语言规范第 5.5 节](language_spec_v0_1.md#55-字符与字符串)。暂不实现 char 标量、字符字面量/解引用读写、Unicode 解码或内置 string。

外部调用使用实际 C 名称，i32/i64/bool/*i32/*char 的完整固定签名沿用 Windows x64 C 调用约定，覆盖寄存器与栈参数及返回。C bool 返回只取 AL，避免高位未定义值进入 bool 运算。外部代码可以修改合法的指针目标，调用不可因结果未使用而删除；不假定其纯净或可在编译期运行。实际求值到 extern(C) 调用时必须报错，不能让编译器执行目标 C 库函数。

当前没有 void 返回、char 标量、浮点、可变参数、其余聚合按值、回调、函数指针、动态库加载、头文件解析或正式导出；声明必须由使用者与实际 C 原型核对，链接由外部工具完成。字面量不提供目标只读限定类型，调用可写 C 接口时必须自行提供可写存储。普通函数仍用后端临时符号，不能据此承诺稳定二进制导出。完整稳定 ABI 方向见[语言规范第 13 节](language_spec_v0_1.md#13-c-互操作与执行环境)。

每个字面量占用解码后长度加 1 字节的静态数据，当前不去重或删除未引用数据。puts 的终止零扫描、CRT 缓冲与实际 I/O 有成本，不能把无隐式语言堆分配描述成输出无成本。

## 控制流与作用域

BD-19 已接受并实现。if/else、while、do...while、for、loop 均为语句；条件严格为 bool，所有体必须使用 {}，包括空体。else if 各分支同样必须有块。for 的三个头部项均可省略，缺省条件为 true；初始化至多一条声明、赋值或调用，迭代项至多一条赋值或调用。

break 退出最近循环；continue 对 while/do 转到条件、for 转到 step 再检查条件、loop 回到开头。循环外使用 break/continue 报错；不支持标签目标或循环值。

for 初始化建立覆盖条件、迭代项及体的作用域，循环结束后不可见；体有自己的内层作用域，体内声明不能被 step 或 do 的条件读取。循环体内普通变量每次执行声明时重新初始化。

返回分析使用 CFG 可达性，所有可能到达函数末尾的路径必须返回值；不终止循环允许没有 return。当前识别直接 bool 常量和省略 for 条件，不进行完整条件证明；例如条件为整数比较或复合逻辑且未显式声明为编译期常量时，可能仍需补充 return。不可达源码继续检查名称、类型及所有 := 义务，之后删除由直接常量分支证明不可达的运行期块。

[control_flow.ar](../examples/control_flow.ar) 展示循环、分支、continue/break 及编译期递归，-O0/-O1 原生退出码均为 3。defer 与 match 尚未实现；清理规则仍按各自条目推进，本轮不默认加入延迟清理。

## 编译期求值

:= 可引用字面量、已有编译期常量并调用函数；初始化直接读取普通变量或参数时拒绝，即使优化器知道其值。被调函数可使用已知参数、内部可变整数/bool 局部变量、分支、循环、递归及短路逻辑。

独立 IR 解释器按控制流执行，不执行未选分支或短路右侧；不会仅追踪最终返回表达式而跳过已执行到的调用。所有显式常量声明均必须求值，包括不可达源码中的声明。错误、循环依赖、超出资源预算必须报错，不回退运行期。单次编译预算暂为 100000 步、递归/依赖深度 256；这是实现限制。

常量解释器支持内部整数/bool 局部存储，仍拒绝原始指针值、取地址、指针算术、间接访存及实际执行的外部调用。此内部存储能力不是完整的编译期地址模型或 run/AST 修改系统。

## AST、IR 与后端

AST 使用平坦数组和稳定索引，块另有语句索引边；名称、类型名和字面量保留原始源码半开区间。源码寿命必须长于 AST/IR，通过 destroy API 释放数组。

名称/类型检查与 IR 降低同一遍进行；全部函数体检查后完成强制常量求值、验证及已知不可达块删除。IR 已有多基本块、跳转、条件分支、比较、局部对象与 load/store。控制流函数的普通可变局部使用存储衔接分支与回边；尚无 phi、mem2reg 或跨块内联/常量传播。直线代码仍保留原有传播/折叠/内联，避免跨分支复用错误的旧值。

Windows x64 后端输出 Clang 集成汇编器使用的 Intel 语法 .s，负责寄存器/栈参数、调用空间、栈对齐、展开元数据。extern(C) 声明保留源码 C 名称，不输出函数体；普通非 main 定义暂名为 atri_fn_<索引>，不是最终 FFI 名称。外部名称与实际生成的内部符号冲突时诊断，不输出残缺汇编。语言没有异常；目标展开元数据服务于 ABI/系统工具，不引入语言异常机制。

源码有普通 main 定义时，-O0/-O1 均按优化后剩余调用，从 main 遍历并筛选函数及外部声明；不可达的递归环也不输出。没有 main 定义时保留全部函数，供当前 C 测试驱动等外部调用方链接。此规则是当前原型的输出约定，尚未实现正式 export 或函数地址/回调；未来加入这些能力时需扩展保留根。语义检查及 IR 验证仍覆盖所有函数，--emit-ir 仍保留完整函数集合，筛选不改变函数索引。

非恒定临时值暂存栈槽，尚无寄存器分配；不承诺已达到最终零开销目标。当前限制：栈帧小于 4096 字节、每次调用至多 256 个实参、控制流至多 4096 块、表达式和语句嵌套至多 256。目标调用约定的依据见[Microsoft 文档](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)。

## 构建与验证

Windows x86-64，Clang 23.1.2、C23、Ninja：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
.\build\atri-parse.exe .\examples\control_flow.ar
.\build\atri-cc.exe --emit-ir-before-opt .\examples\control_flow.ar
.\build\atri-cc.exe -S -o .\build\flow.s .\examples\control_flow.ar
clang .\build\flow.s -o .\build\flow.exe
.\build\flow.exe
$LASTEXITCODE # 3
```

atri-parse 默认输出 AST，atri-cc 默认输出汇编；两者支持 --emit-ast、--emit-ir-before-opt、--emit-ir、-S、-o，文件名 - 读取标准输入。-O1 默认对直线函数实施折叠、DCE 和小函数内联；-O0 保留运行期调用但仍进行直线局部折叠/DCE。显式常量与优化等级无关。--emit-ir-before-opt 已经完成强制常量求值。退出码 0 成功、1 编译错误、2 命令行/I/O 错误。

所有输出模式均产生文本，`-o` 不根据扩展名切换成二进制或自动链接；`.exe` 输出名（不区分大小写）在打开输出文件前拒绝，返回 2 并提示先生成 .s 再调用 Clang，不覆盖已有文件。真实可执行文件由 `clang source.s -o program.exe` 生成。

CTest 七组：前端、编译器回归、控制流语义/常量、优化效果、IR Verifier、CLI 示例、Windows 原生执行。优化按函数名和明确预期检查；原生测试自动生成具名 C 调用映射，写法见[测试说明](../tests/README.md)。原生测试在 -O0/-O1 下汇编、链接并执行整数、常量、指针、控制流和 puts 示例，核对真实 stdout、UTF-8 字节、内嵌零截断、返回静态字面量及外部调用；C 驱动覆盖 64 位/指针的寄存器与栈参数、C bool/指针返回、外部写入及既有控制流行为。需要本机 Clang、Windows SDK/链接工具，不自动安装依赖。
