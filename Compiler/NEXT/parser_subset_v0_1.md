# 基础编译原型 v0.1

> 状态：Stage 0 已实现子集，2026-11-17。规则入口为[语言规范](language_spec_v0_1.md)，内部契约见 [IR 设计](ir_design_v0_1.md)。本文记录可运行行为，不冻结整门语言。

## 声明与调用

函数使用 `fn name(parameters) -> Type`，参数和局部声明使用 `name: Type`。先收集全部函数签名，允许调用后定义的函数。普通变量采用 `name: Type [= expression];`，可变并默认初始化（结构体字段默认值规则见下文）；`name: Type := expression;` 必须在编译期求值成功。旧 let/const 声明及 `name: Type : expression` 不接受；类型推导简写待定。`::` 的模块绑定与限定访问设计见[语言规范第 11 节](language_spec_v0_1.md#11-模块与名称空间)，当前最小实现范围见下节。

局部声明顺序可见，块内可遮蔽外层名称，同层重复报错；函数最外层与参数共享名称作用域。参数绑定只读，指针参数可写其目标。作用域和参数规则采用当前 BD-12 候选，仍待完整审查。

[main.ar](../examples/main.ar) 在 -O1 下内联 add(1, 2) 并折叠为 3；[constants.ar](../examples/constants.ar) 用 `:=` 强制求值，-O0 下也直接生成 `mov eax, 3; ret`。这里的 3 是返回值/进程退出码，不向标准输出打印字符。汇编只保留 main 及优化后从 main 可达的函数，已无运行期引用的 add 不再输出；-O0 下 main.ar 的 add 调用和定义仍保留。

## 模块与全局的当前实现

CLI 支持 `import "path";` 直接引入 export 名称和 `name :: import "path";` 命名模块绑定；模块中的函数、结构体类型和已支持全局默认私有，export 使它们可被导入。路径相对导入方文件解析，省略 `.ar` 扩展名，使用 `/`、`.`、`..`；不支持绝对导入、路径转义、包搜索或再导出。Windows 按文件最终路径识别符号链接/目录链接，再去重加载；当前文件接口使用 Windows 窄字符 API，非当前系统编码可表示的路径尚未验证。导入依赖深度上限 256。

一次编译中源模块只加载/解析一次。加载器先扫描依赖，再在保留文件边界的源码存储中解析 AST；各模块保持独立作用域，不是 include 文本拼贴。公开名称表建立后排序并二分查找；模块名称查找不生成运行时代码。没有跨次构建的接口缓存或增量失效，冷启动仍读取和检查所加载模块的完整源码，不据此宣称比 C 头文件更快。

顶层允许 `export name: Type [= expression];`、私有普通变量及 `name: Type := expression;`。当前普通全局类型为八种定宽整数、bool、已有一层数据指针、结构体及 union；整数/bool 初始化器必须在编译期求值，可引用编译期常量、调用可解释执行的函数。指针静态初始化只支持 null 或省略初始化器；运行期可赋其他合法指针值。支持这些全局取地址、通过返回的指针读写；聚合静态初始化见后文对应小节。顶层数组全局、非零指针静态重定位初始化及指针/聚合编译期常量仍未支持。

导入访问指向原全局对象；普通赋值复制值。导出不加入锁、原子指令或运行期名称查找。所有模块先收集声明后检查函数体，因此跨模块递归可用；编译期依赖环及非法访问仍报错。CLI 错误按原模块路径、行列显示。当前一次生成包含各模块所需代码和全局数据的 `.s`，外部 Clang 完成汇编/链接；尚无独立模块接口/目标文件输出。

## 结构体的当前实现

支持 `struct Name { field: Type [= expression]; ... }`，定义后可有分号；类型按声明身份区分，不采用结构相等。先收集类型再计算布局，允许后定义类型及指针递归，拒绝按值布局循环。字段按声明顺序自然对齐，支持八种定宽整数、u8/bool、已有指针、固定数组、内嵌结构体及 `*T`。大小必须小于 4096 字节，字段最多 4096 个，按值嵌套深度最多 256；实际函数栈帧仍须小于 4096 字节。

变量沿用 `value: Type`。支持 `{}`、`{.field = expression}`、嵌套指定初始化及初始化项尾逗号；重复字段和未知字段均报错。显式项按源码顺序求值；省略字段使用声明默认值，其余字段按类型默认初始化，内嵌结构体递归应用其默认值。当前声明默认值仅支持可在编译期求值的标量表达式，指针默认值限 null；所有声明默认值均检查，不能用显式初始化覆盖来隐藏非法默认表达式。

结构体赋值、实参和返回均为值副本；指针字段只复制指针值。参数保持只读，需要修改时复制到局部变量；通过指针访问的原对象仍可写。`obj.field` 和 `(*p).field` 可读写，禁止 `p.field` 自动解引用；支持可写 i32/结构体字段取地址。临时结构体返回值的字段可读、不可赋值。尚无自动结构体相等比较。

支持 `export struct`、直接及命名导入类型、参数和返回中的 `module::Type`。C 按值 ABI 已通过原生交叉调用验证：1/2/4/8 字节结构体作为同宽整数传递/返回，其余正大小结构体参数使用调用方独立的 16 字节对齐副本；返回使用隐藏的首个缓冲区参数并在 RAX 返回该地址。后续参数位置随隐藏参数平移。每个间接实参都有独立副本，不把同一只读值重复传参时的缓冲区共享给 C。依据见 [Microsoft x64 调用约定](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)。

空结构体大小 0、对齐 1；当前内部调用占一个实参位置，参数/结果不复制内容，返回寄存器置零。这个内部约定尚非稳定 C 接口，extern(C) 按值拒绝空结构体及含零大小成员的结构体；零大小目标指针算术报错。

普通结构体全局支持 `state: State;`、`state: State = {};` 和指定字段初始化，省略字段沿用上述默认规则；允许嵌套结构体及结构体内的固定数组。每个显式标量叶子独立强制编译期求值，允许纯标量函数调用，拒绝运行期全局依赖和外部 C 调用；指针叶子仅接受 null。初始化器在全局定义模块解析，字段声明默认值仍在字段定义模块解析。编译器直接生成对齐的静态数据，没有运行期初始化函数；运行期字段读写、取地址、整体赋值/传参/返回继续复用已有语义。示例见 [struct_globals.ar](../examples/struct_globals.ar)。

静态聚合部分仅接受花括号，不支持从其他聚合对象复制初值或由返回聚合的函数生成初值，也不支持 :=/#run 聚合结果、聚合字段默认表达式或非零地址重定位。物化静态对象全次编译限 100000 次节点访问、深度 256；没有显式子项的零大小子树直接跳过。每个零大小全局在后端保留一个静态字节作为地址锚点，sizeof 仍为 0；这不定义未来零大小对象的完整地址规则。

本轮未实现 packed/自定义对齐或完整生命周期证明。含结构体/字段位置操作的函数暂不套用原单块优化与内联；当前会保留额外的栈快照和复制，没有隐式堆分配，但不能将语义正确或 ABI 通过当成复制已消除或性能已达标。局部/调用示例为 [struct.ar](../examples/struct.ar)，验证入口见测试说明。

## 无标签 union 的当前实现

支持 `union Name { member: Type; ... }`、可选定义尾分号、`export union` 及 `union Name;` opaque 指针目标。union 与 struct 共用名义类型表和名称空间；不因成员相同而互换类型。布局、零初始化、指定单个成员及跨成员读取的规则统一见[语言规范第 9.2 节](language_spec_v0_1.md#92-无标签联合体)。成员类型、布局大小/深度及函数栈帧沿用结构体的当前上限，按值循环仍拒绝，指针递归可用。

已接通局部及普通全局、嵌套结构体/union/数组、字段读写/取地址、显式指针解引用、整体赋值、按值参数/返回、sizeof/alignof 和指针偏移/差值。全局花括号初值可指定嵌套聚合，标量叶子强制编译期求值；静态指针仍限 null，不能借 union 的另一个整数成员伪造合法可解引用地址。`u: U;`/`u: U = {};` 清零整块存储；`u: U = {.entry = {}};` 则应用所选 entry 类型自己的初始化规则。

复用现有 Windows x64 C 聚合 ABI，原生交叉调用覆盖 1/2/3/4/8/12/16 字节 union、含 union 的结构体、栈参数、隐藏返回缓冲区、独立实参副本、指针写入和 C 可变参数尾参。整块 union 可以包含不同时对所有成员有效的位模式；只在实际读取某成员时要求该类型的合法表示，不跟踪有效成员。没有隐含标签/动态检查，但默认零化、快照和整体复制仍有成本，含 union 的函数继续保守跳过原单块聚合优化。

尚不支持匿名 union/成员、位域、成员声明默认值、空 union 定义、packed/自定义对齐、:=/#run 聚合结果、聚合调用/复制静态初值、非零指针重定位或顶层数组全局。语言已有的零大小成员仍可用于内部布局，C 按值拒绝零大小对象及含零大小成员的聚合；opaque 声明尚不能后续补全。示例见 [union.ar](../examples/union.ar)，验证入口见[测试说明](../tests/README.md#union-回归)。

## 固定数组的当前实现

支持 `data: [N]u8 = {1, 2};`、空列表、尾逗号、多维数组及结构体字段数组。类型由元素类型和长度共同确定，重复出现的同一数组类型共享描述；数组不自动退化为指针。整体赋值、参数和返回保持值副本，指针元素只复制地址。参数绑定仍只读，借助 `*[N]T` 显式修改原数组时使用 `(*p)[i]`；元素取地址仍受已有指针目标类型限制。

当前 N 支持十/二/十六进制字面量、可见整数 `:=` 常量、整数显式 cast、sizeof/alignof 及整数一元/算术/位运算/移位，使用对应整数位宽；无具体类型的长度表达式按 i64 定型。局部/for 头部常量遵循词法作用域和声明顺序，运行期变量及参数不能作为长度。布局求值暂不支持函数调用或条件表达式，即使 `#run` 能对这些表达式求值；这是当前布局阶段限制。长度须在 0..4096，数组总大小须小于 4096 字节；长度求值最多 100000 步、依赖深度 256，实际函数仍受总栈帧限制。

元素按初始化列表的源码顺序求值；不足部分按元素类型默认初始化，结构体元素递归使用字段默认值，超过长度报错。`a[i]`、嵌套索引和索引后的字段均可读写；临时数组只读，参数数组元素不能直接修改。索引当前接受八种定宽整数（单引号字节字面量可直接作为索引），支持常量/有限纯整数传播的静态越界诊断，两种优化等级下均不生成动态边界检查。零长度数组不能索引；正长度零大小元素数组可按合法逻辑索引访问，但零大小目标指针算术仍拒绝。

已接通 u8 值、`*u8`、字节读写/取地址、同宽整数算术、比较、常量求值与 Windows x64 固定签名调用。u8 算术采用已接受 BD-06 的模 256 环绕，除余与排序按无符号值处理；u8 与 i32/i64 没有隐式转换。其他整数位宽及指针现已补齐，见下节。

尚不支持数组全局、`:=`/`#run` 聚合结果、聚合字段默认表达式、`.len`/`.data` 或动态数组。extern(C) 直接按值数组参数/返回拒绝，含正长度 C 兼容数组的普通结构体沿用已有 C 布局和聚合 ABI；不把 C 数组参数声明当作语言内按值数组。数组初始化、复制和索引临时地址仍占栈空间，有额外快照/复制，未证明复制最少或性能达标。语法之外的这些细节属于本轮原型边界，相关长期提案仍按 BD-12/15 审查。

## 枚举与 switch 的当前实现

支持顶层 `[export] Name :: enum [(Type)] { Member [= constant], ... }`，默认 i32；bit8/16/32/64 使用对应无符号整数。自动成员编号、单一位与零声明限制、整数别名/运算/索引语义统一见[语言规范第 9.3 节](language_spec_v0_1.md#93-普通枚举)。枚举类型名在签名、字段、数组和指针中直接解析成底层整数，成员降为相同类型的 IR CONST；不同枚举不引入独立运行期值类型。支持直接/命名导入及 `module::Enum::Member`。

成员初值当前采用布局整数常量求值路径：支持字面量、已存在的编译期整数常量、整数运算/转换、sizeof/alignof 和枚举成员引用。同一枚举可引用此前成员，其他枚举可前向引用；循环/未完成成员引用报错。暂不支持在成员初值中调用函数或完整 #run 代码块；`#run` 包裹不会绕过该布局阶段限制。单枚举最多 4096 成员，常量表达式沿用 100000 步/256 深度限制。命名/值重复检查当前为有界线性扫描，未做接口缓存或性能测量。

switch、case/default、空标签组、显式 `#through;`、case 词法作用域和 break/continue 目标规则见[语言规范第 8.3 节](language_spec_v0_1.md#83-switch)。case 借用完整标量常量解释器，允许可解释执行的函数调用和显式 #run，求值后检查重复值；选择值只计算一次。实现使用比较链和现有多块 CFG，最多 1024 case，函数仍受 4096 块/实际栈帧上限限制；不生成运行期 case 求值、成员标签或穷尽检查，尚无跳转表。

可运行示例见 [enum_switch.ar](../examples/enum_switch.ar)。[enum.ar](../examples/enum.ar) 保留为原始语法草稿，其中的 `switch(E1)` 和 `expr` 用于讨论语法。Payload 枚举、局部枚举定义及无命名空间的枚举成员访问尚未实现。

## 当前句法

```ebnf
program    = { function | import | global | aggregate | enum | run-statement, ";" }, EOF ;
enum       = [ "export" ], identifier, "::", "enum", ["(", identifier, ")"],
             "{", enum-item, {",", enum-item}, [","], "}", [";"] ;
enum-item  = identifier, ["=", expression] ;
aggregate  = [ "export" ], ("struct" | "union"), identifier, (";" | "{", { field }, "}", [ ";" ]) ;
field      = identifier, ":", type, [ "=", expression ], ";" ;
import     = [ identifier, "::" ], "import", string-literal, ";" ;
global     = [ "export" ], declaration, ";" ;
function   = [ "export" ], ("fn", signature, block
           | "#extern", "(", "C", ")", "fn", signature, ";") ;
signature  = identifier, "(", [parameters, [ ",", "..." ] | "..."], ")", "->", type ;
parameters = parameter, { ",", parameter } ;
parameter  = identifier, ":", type ;
type       = "*", type | "[", expression, "]", type | identifier, [ "::", identifier ] ;
block      = "{", { statement }, "}" ;
statement  = declaration, ";" | run-statement, ";" | assignment, ";" | call, ";"
           | "return", [expression], ";" | block
           | "if", "(", expression, ")", block, [ "else", (block | if-statement) ]
           | "while", "(", expression, ")", block
           | "do", block, "while", "(", expression, ")", ";"
           | "for", "(", [for-init], ";", [expression], ";", [for-step], ")", block
           | "loop", block | "break", ";" | "continue", ";" | switch ;
switch     = "switch", "(", expression, ")", "{", {case}, "}" ;
case       = ("case", expression | "default"), ":", {statement}, ["#through", ";"] ;
if-statement = "if", "(", expression, ")", block, [ "else", (block | if-statement) ] ;
run-statement = "#run", unary ;
declaration = identifier, ":", type, (":=", expression | ["=", expression]) ;
assignment = expression, ("=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="), expression ;
for-init   = declaration | assignment | call | run-statement ;
for-step   = assignment | call | run-statement ;
expression = logical-or, [ "?", expression, ":", expression ] ;
logical-or = logical-and, { "||", logical-and } ;
logical-and = bit-or, { "&&", bit-or } ;
bit-or     = bit-xor, { "|", bit-xor } ;
bit-xor    = bit-and, { "^", bit-and } ;
bit-and    = equality, { "&", equality } ;
equality   = relational, { ("==" | "!="), relational } ;
relational = shift, { ("<" | "<=" | ">" | ">="), shift } ;
shift      = sum, { ("<<" | ">>"), sum } ;
sum        = product, { ("+" | "-"), product } ;
product    = unary, { ("*" | "/" | "%"), unary } ;
unary      = ("+" | "-" | "&" | "*" | "!" | "~" | "#run"), unary | postfix ;
postfix    = primary, { ".", identifier | "[", expression, "]" } ;
initializer = "{", [ initializer-item, { ",", initializer-item }, [ "," ] ], "}" ;
initializer-item = expression | ".", identifier, "=", expression ;
conversion = "cast", "(", type, ",", expression, ")" ;
layout-query = ("sizeof" | "alignof"), "(", type, ")" ;
primary    = conversion | layout-query | integer-literal | character-literal | string-literal | "true" | "false" | "null" | name | call | initializer
           | "(", expression, ")" ;
name       = identifier, [ "::", identifier, ["::", identifier] ] ;
call       = name, "(", [arguments], ")" ;
arguments  = expression, { ",", expression } ;
integer-literal = decimal-integer | hex-integer | binary-integer ;
decimal-integer = digit, { ["_"], digit } ;
hex-integer = ("0x" | "0X"), hex-digit, { ["_"], hex-digit } ;
binary-integer = ("0b" | "0B"), binary-digit, { ["_"], binary-digit } ;
hex-digit = digit | "A" ... "F" | "a" ... "f" ;
binary-digit = "0" | "1" ;
identifier = identifier-start, { identifier-start | digit } ;
identifier-start = "A" ... "Z" | "a" ... "z" | "_" ;
digit = "0" ... "9" ;
```

`...` 只表示文法中的范围。赋值目标、已定型整数间的精确类型等由语义检查。优先级、结合性、短路和求值顺序按[主规范第 7.1 节](language_spec_v0_1.md#71-求值与副作用)。赋值和复合赋值是语句。普通函数调用和独立 `#run` 可以丢弃结果。`#run` 前缀绑定一个一元操作数。

支持 ASCII 空白、LF/CRLF/CR、开头可选 UTF-8 BOM 和 // 行注释。暂不支持块注释、浮点、整数类型后缀、八进制整数、++/--、逗号表达式；聚合初始化之外不接受尾逗号。union 定义必须非空且成员声明不能有默认值，初始化最多指定一个成员。全角分号、源码 NUL、不支持的字符会诊断。

## 类型与运算

- 八种定宽整数 i8/i16/i32/i64/u8/u16/u32/u64 完整覆盖局部/标量全局、字段/数组、参数/返回、:=、标量 #run、cast、布局查询与一层数据指针。自然布局按目标 1/2/4/8 字节计算。
- bool 布局为 1 字节、合法表示 0/1；只支持布尔逻辑与相等比较，不提供 bool/整数或 bool/指针转换。
- 不提供内置 char 或旧语法别名。单引号字节字面量固定为 u8，支持普通整数运算、比较、常量/#run、指针读写和直接索引；i8 需要显式整数转换。双引号字面量固定为 *u8，正文为 UTF-8 字节并额外附加终止零；\xHH 恰好两位，拒绝八进制整数/转义，\0 不吸收后续数字。
- 整数字面量和运算按已接受的[第 5.4/7.2 节](language_spec_v0_1.md#72-数值运算与比较)，包含无符号负号环绕、位运算、移位及复合赋值。无上下文纯整数默认 i32，指针偏移字面量默认 i64。
- 实际执行的编译期非法除余/移位报错；可证明的非法移位数量在 -O0/-O1 均诊断。多块函数没有完整常量传播，不承诺发现所有非法除余。动态不增加检查；合法整数环绕不能使非法指针操作合法。
- 复合赋值先固定左侧位置并读取旧值，再求右侧，仅计算一次左侧；覆盖名称、显式解引用、字段、数组元素和合法的指针 +=/-=。
- 所有支持的数据指针都可复制、传参/返回、与同类型指针或 null 作 ==/!=；只有指针能与 null 比较。取地址/显式目标读写覆盖全部整数、bool、结构体/数组；*void/opaque 目标不能直接访问。无多层指针或默认动态检查，见[指针子集](pointer_subset_v0_1.md)。

## 整数转换与布局查询

`cast(Type, expression)` 支持八种定宽整数的两两显式转换；缩窄保留低位，扩宽按源类型符号/零扩展，同宽保留位模式并按目标类型解释。cast 操作数使用普通表达式定型规则，无具体类型的纯数字表达式仍默认 i32；需要大整数时先声明 i64/u64 源值。目标类型不反向限制源值的范围，例如 `cast(u8, 300)` 得到 44。没有隐式整数转换，也不开放 bool 整数、指针整数、浮点或聚合转换；数据指针的显式 cast 已随 C 互操作补齐；不提供旧尖括号/C 式转换兼容或额外模式参数。

u64 覆盖 0..18446744073709551615，支持默认/显式初始化、算术、比较、常量求值、模块全局、结构体字段/数组元素、参数/返回和固定签名 C 调用。算术按已接受 BD-06 模 2^64 环绕，除余与排序按无符号值处理；已接通 *u64 和其余定宽整数；未加入平台相关整数或浮点。指针偏移仍要求 i32/i64，u64 大小需要显式转换后使用，不能将地址溢出视为普通整数环绕。

`sizeof(T)` 与 `alignof(T)` 查询完整对象类型，结果是 u64 编译期常量，使用 Windows x86-64 的目标布局；不读取运行期表达式，不生成查询函数调用。支持当前标量、指针、固定数组、结构体和零大小布局；只计算指针自身布局时无需完成目标对象布局。void/未知类型/按值布局循环拒绝，尚无表达式形式。两者可参与数组长度，嵌套查询与长度求值共享 100000 步/256 深度预算，循环依赖必须诊断。

常量解释器、折叠器和后端遵循同一转换结果。整数 cast 降为必要的截断/扩展或位模式传递，无隐式保值检查；u64 无符号运算也不添加默认溢出检查。原型仍可能保留值栈槽和 mov，不把功能正确等同于机器指令最少。例子见 [conversions.ar](../examples/conversions.ar)。

## C 字符串与外部调用

[hello.ar](../examples/hello.ar) 声明 `#extern(C) fn puts(text: *u8) -> i32;`，导入 basic.ar，写入共享全局 TC，再调用导出的 add；结果为 6 时向标准输出打印 `import "basic"` 并返回 0。属性内部允许空白与换行；当前只支持 extern(C)，声明必须以分号结束，不接受函数体、重复声明、未知属性或其他调用约定。

字符串字面量的类型为 *u8，降低为 IR 自有字节数据和符号引用，最终放入 .rdata；返回字面量地址不会悬空，也不会产生运行期堆分配。编码、转义、零结尾、内嵌零及禁止写入契约统一见[语言规范第 5.5 节](language_spec_v0_1.md#55-字符与字符串)。单引号字节字面量、u8 算术/索引及指针读写已实现；Unicode 解码和内置 string 继续暂缓。

外部调用使用实际 C 名称，八种整数/bool/已有数据指针的完整固定签名沿用 Windows x64 C 调用约定，覆盖寄存器与栈参数及返回。C bool 返回只取 AL，避免高位未定义值进入 bool 运算。外部代码可以修改合法的指针目标，调用不可因结果未使用而删除；不假定其纯净或可在编译期运行。实际求值到 extern(C) 调用时必须报错，不能让编译器执行目标 C 库函数。

当前没有浮点、可变参数定义端、其余聚合按值、回调、函数指针、动态库加载、头文件解析或正式导出；声明必须由使用者与实际 C 原型核对，链接由外部工具完成。字面量不提供目标只读限定类型，调用可写 C 接口时必须自行提供可写存储。普通函数仍用后端临时符号，不能据此承诺稳定二进制导出。完整稳定 ABI 方向见[语言规范第 13 节](language_spec_v0_1.md#13-c-互操作与执行环境)。

每个字面量占用解码后长度加 1 字节的静态数据，当前不去重或删除未引用数据。puts 的终止零扫描、CRT 缓冲与实际 I/O 有成本，不能把无隐式语言堆分配描述成输出无成本。

## C 可变参数、opaque FILE 与文件读取

支持 extern(C) 声明末尾 `...`，包含 C23 纯 ellipsis 声明。固定部分精确检查，实际参数至少覆盖固定部分，尾部 i8/u8/i16/u16/bool 必须提升为 i32，其余已支持整数/数据指针和 C 兼容正大小普通结构体沿用相应 ABI。尾部数组、void/零大小对象等拒绝，裸 null 需要显式指针类型；参数按源码顺序求值一次。当前不支持浮点或在 ATRI 定义/读取可变参数，不验证 printf 格式字符串，也不生成 varargs 容器。

新增 void 返回与 *void。void 函数可正常落到末尾或裸 `return;`，有值函数仍必须返回值；void 调用不能参与赋值、参数或其他值计算。Hosted main 代码生成要求 i32 返回。*void 是薄数据指针，与已有数据指针可显式 cast，不允许指针与整数/bool 互转。*void 和 opaque 目标不能读写解引用或算术；转换不改变对象权限、对齐、大小或寿命。直接可证明的 null 与局部地址返回诊断会穿过数据指针 cast，但不保证发现所有逃逸。

`struct FILE;` 是 opaque 名义声明，只能通过指针使用，不能伪造零大小 FILE 对象；当前不支持同名后续补全。`struct Empty {}` 仍是完整零大小类型。[c_stdio.ar](../examples/c_stdio.ar) 给出精确类型的 C 标准库声明，[read_file.ar](../examples/read_file.ar) 从 Compiler 工作目录读取 examples/main.ar，并通过 printf 按块输出文本；[variadic.ar](../examples/variadic.ar) 展示 u8/bool 提升及 64 位/指针尾参。

标准库读写是运行期调用，#run/常量求值仍不能执行外部 C 代码。fread 不保证零结尾，调用方必须按实际返回数量访问数据；文本和二进制模式、路径编码、清理和失败处理规则见语言规范第 13.4 节。测试在 -O0/-O1 下验证 257 字节二进制循环读取、EOF/缺失文件、写回字节及实际 printf stdout；不据此承诺 Unicode 路径或完整 C ABI。

## 控制流与作用域

BD-19 已接受并实现。if/else、while、do...while、for、loop 均为语句；条件严格为 bool，所有体必须使用 {}，包括空体。else if 各分支同样必须有块。for 的三个头部项均可省略，缺省条件为 true；初始化至多一条声明、赋值或调用，迭代项至多一条赋值或调用。

break 退出最近循环；continue 对 while/do 转到条件、for 转到 step 再检查条件、loop 回到开头。循环外使用 break/continue 报错；不支持标签目标或循环值。

for 初始化建立覆盖条件、迭代项及体的作用域，循环结束后不可见；体有自己的内层作用域，体内声明不能被 step 或 do 的条件读取。循环体内普通变量每次执行声明时重新初始化。

返回分析使用 CFG 可达性，所有可能到达函数末尾的路径必须返回值；不终止循环允许没有 return。当前识别直接 bool 常量和省略 for 条件，不进行完整条件证明；例如条件为整数比较或复合逻辑且未显式声明为编译期常量时，可能仍需补充 return。不可达源码继续检查名称、类型及所有 := 义务，之后删除由直接常量分支证明不可达的运行期块。

[control_flow.ar](../examples/control_flow.ar) 展示循环、分支、continue/break 及编译期递归，-O0/-O1 原生退出码均为 3。defer 尚未实现；switch 见上文，不采用 match；清理规则仍按各自条目推进，本轮不默认加入延迟清理。

## 编译期求值

顶层和函数内支持 `#run f(args);`，可丢弃整数/bool 结果或执行 `void` 调用。标量 `#run expression` 可用于普通变量和 `:=` 常量的初始化器、返回和实参，关闭优化仍必须求值成功。`a: i32 = #run f();` 只在编译期取得初值，`a` 的赋值和作用域遵循普通变量规则。旧的 `#run` 声明前缀已移除。可执行示例见 [run_constants.ar](../tests/cases/run_constants.ar) 与 [arrays_run.ar](../tests/cases/arrays_run.ar)。

按[规范第 12.1 节](language_spec_v0_1.md#121-能力边界)，`#run { ... }` 是无结果块，`#run -> Type { ... }` 是以显式 `return` 产生值的块表达式。两类代码块当前均报尚未实现。聚合结果、编译期指针/数组内存和 AST API 也仍待实现。

:= 可引用字面量、已有编译期常量并调用函数；初始化直接读取普通变量或参数时拒绝，即使优化器知道其值。cast 与 sizeof/alignof 同样可用于常量与 #run。被调函数可使用已知参数、内部可变整数/bool 局部变量、分支、循环、递归及短路逻辑。

独立 IR 解释器按控制流执行，不执行未选分支或短路右侧；不会仅追踪最终返回表达式而跳过已执行到的调用。所有显式常量声明均必须求值，包括不可达源码中的声明。错误、循环依赖、超出资源预算必须报错，不回退运行期。单次编译预算暂为 100000 步、递归/依赖深度 256；这是实现限制。

常量解释器支持内部整数/bool 局部存储，仍拒绝原始指针值、取地址、指针算术、间接访存及实际执行的外部调用。此内部存储能力不是完整的编译期地址模型或 #run 代码块/AST 修改系统。

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
