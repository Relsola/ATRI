# 整数编译原型 v0.1

> 状态：已实现的 Stage 0 整数子集，2026-11-16。语言规则入口为[语言规范](language_spec_v0_1.md)，内部表示见 [IR 设计](ir_design_v0_1.md)。本文描述当前实现，不冻结整门语言。

## 输入与目标

当前示例 [main.txt](../examples/main.txt)：

```text
fn main() -> i32 {
    const a: i32 = 1;
    const b: i32 = 2;
    return add(a, b);
}

fn add(a: i32, b: i32) -> i32 {
    return a + b;
}
```

名称在前、类型在后，通过 `:` 分隔。函数返回类型仍放在 `->` 后。先收集全部函数签名，再处理函数体，允许调用后面定义的函数。局部绑定当前要求先声明再使用；参数不可重新赋值，采用 BD-12 候选，这些局部作用域细节仍待正式审查。

`const` 仍可用运行期表达式初始化。不可变绑定有助于传播值；当前 `-O1` 内联适合的小函数，然后折叠整数运算，示例 main 的执行指令最终为：

```asm
main:
    mov eax, 3
    ret
```

完整汇编还有节、符号和 Windows 栈展开描述等指令，以及单独的 add 函数定义；目前没有跨函数删除未引用定义的 Pass。这里的 3 为返回值/进程退出码，不向标准输出打印字符。

## 已实现的句法

```ebnf
program    = { function }, EOF ;
function   = "fn", identifier, "(", [parameters], ")", "->", identifier, block ;
parameters = parameter, { ",", parameter } ;
parameter  = identifier, ":", identifier ;
block      = "{", { statement }, "}" ;
statement  = "const", identifier, ":", identifier, "=", expression, ";"
           | "let", identifier, ":", identifier, ["=", expression], ";"
           | identifier, "=", expression, ";"
           | "return", expression, ";" ;
expression = sum ;
sum        = product, { ("+" | "-"), product } ;
product    = unary, { ("*" | "/" | "%"), unary } ;
unary      = ("+" | "-"), unary | primary ;
primary    = decimal-integer | identifier
           | identifier, "(", [arguments], ")"
           | "(", expression, ")" ;
arguments  = expression, { ",", expression } ;
decimal-integer = digit, { digit } ;
identifier = identifier-start, { identifier-start | digit } ;
identifier-start = "A" ... "Z" | "a" ... "z" | "_" ;
digit = "0" ... "9" ;
```

`...` 仅表示文法说明中的字符范围。乘除余数优先于加减，同级二元运算左结合；一元正负号更优先。实参当前按从左到右降低，采用 BD-09 候选。不接受尾逗号。

支持 ASCII 空白、LF/CRLF/CR 和开头可选 UTF-8 BOM；这些便利不自动冻结 BD-01 全部词法规则。全角 `；`、嵌入 NUL、不支持的字符均会诊断。暂不支持注释、其他进制/数值分隔符、浮点数、比较/逻辑/位运算、控制流、指针、聚合、导入、全局绑定、外部调用或 `run`。

## 整数与语义检查

- 可执行类型仅为 `i32`；解析器仍保留任意标识符形式的类型名，语义阶段拒绝未支持的类型，不偷偷按 i32 处理。
- 字面量按目标 i32 范围检查，支持 `-2147483648`；正数 `2147483648` 非法。
- 按用户授权暂用 BD-06 候选：加减乘和一元负号按 32 位环绕；除法向零截断，余数与被除数同号。
- 静态折叠遇到除零、INT_MIN 除以或取模于 -1 会诊断，包括内联后才成为常量的情况。当前还保守诊断函数体中可证明的常量非法运算；不生成运行期检查。动态非法运算暂沿用候选的未定义行为，不保证具体陷阱形式。
- 支持 let 默认零初始化、重新赋值；禁止 const/参数重新赋值。相同函数名、重复局部/参数名、未知名称、不可调用的局部值、实参数量不符、缺少返回值均报错。
- 递归/相互递归可以解析与检查，递归调用保留；本子集还没有条件控制，测试不执行无终止的递归。
- `return` 后的语句仍检查名称和类型，但不进入执行 IR；不可达常量运算不强制折叠。

这些是原型的具体边界，整数环绕等规则仍保留待审查状态；不据此冻结其他宽度、参数可变性、遮蔽或一般未定义行为范围。浮点运算本轮明确暂缓，长期类型目标仍保留。

## AST、IR 与后端

AST 用平坦数组和稳定索引记录函数、参数、语句、表达式和实参边；名称、类型名、字面量仍保留原始源码半开字节区间。源码由调用方持有，寿命必须长于 AST/IR；通过对应 destroy API 释放数组。

当前在检查名称和类型的遍历中直接产生 Typed IR，尚无独立 Typed AST 层。IR 只有一个 entry 基本块，不宣称已实现 CFG/phi/内存 IR 或任意 AST 修改接口。优化与机器后端读取 IR，不重新解释源码表达式。

Windows x64 后端生成供 Clang 集成汇编器使用的 Intel 语法 `.s`，实现本子集的寄存器/栈参数、调用空间、栈对齐和栈展开描述。非 main 函数暂命名为 `atri_fn_<函数索引>`，不是正式 FFI 命名标准。常量可直接装入寄存器，其他临时值暂存栈槽，尚无通用寄存器分配；不承诺全部代码已达到最终零开销目标。后端暂限栈帧小于 4096 字节、每次调用至多 256 个实参，超出时诊断。

语言没有异常；Windows 栈展开元数据服务于目标 ABI 和系统工具，不引入 throw/catch 或语言级清理展开。后端规则依据 [Microsoft x64 调用约定](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)及[函数序言/尾声](https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog?view=msvc-170)。

## 构建与使用

已在 Windows x86-64 使用 Clang 23.1.2、C23、Ninja 验证：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

.\build\atri-parse.exe .\examples\main.txt
.\build\atri-cc.exe --emit-ir-before-opt .\examples\main.txt
.\build\atri-cc.exe --emit-ir .\examples\main.txt
.\build\atri-cc.exe -S -o .\build\main.s .\examples\main.txt
clang .\build\main.s -o .\build\main.exe
.\build\main.exe
$LASTEXITCODE # 3
```

`atri-parse` 默认输出 AST；`atri-cc` 默认输出汇编，两者共用驱动，均支持上述输出选项。`-o` 可指定输出文件，`-` 从标准输入读源码；例子暂用 `.txt`，不冻结正式扩展名。

`-O1` 为默认，实施常量折叠、死指令消除和小函数内联；`-O0` 仍折叠局部常量及消除死指令，但保留调用，便于验证调用链。`--emit-ir-before-opt` 输出未经优化的检查结果。退出码 0 成功，1 编译错误，2 命令行/I/O 错误。

自动测试分四组：前端 AST/位置/错误与扩容；整数边界、作用域与类型、优化及 IR Verifier；CLI 示例；Windows 原生汇编/链接/执行。原生测试在 -O0/-O1 下都检查示例退出码 3，并用 C 调用动态整数函数验证负数除余、环绕、可变绑定、嵌套调用及六参数传递。需要本机 Clang 和 Windows SDK/链接工具，不自动下载安装。它们不等于语言全部语义用例已实现。
