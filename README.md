# ATRI

自研编程语言与自举编译器项目。

当前处于语言规范与编译器架构设计阶段，首个实现目标为 Windows x86-64，计划使用 C23 编写 Stage 0，并逐步完成自举。

## 设计文档

- [语言规范 v0.1](Compiler/NEXT/language_spec_v0_1.md)：词法、语法、类型、内存、模块、编译期执行与互操作的工作草案。
- [自举编译器架构与实施路线](Compiler/NEXT/self_hosting_compiler_design_v0_1.md)：项目边界、前后端架构、自举里程碑及验收要求。

语言规范区分已确定规则、建议方案与待定事项；当前尚未冻结完整的核心可自举子集，仓库暂未包含编译器实现。

## 当前模块方案

```text
import "math";
import "../common/utils" as tools;

math::add(1, 2);
tools::some_function();
```

导入路径相对于导入方源文件所在目录解析；`as` 提供可选别名，`::` 限定模块名称，`export` 用于公开声明。完整规则和待定边界以语言规范为准。
