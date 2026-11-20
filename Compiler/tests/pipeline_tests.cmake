file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(driver "${COMPILER}")
set(plugin "${SOURCE_DIR}/examples/meta/operators.ar")
set(example "${SOURCE_DIR}/examples/meta/pipeline.ar")

function(check_command expected)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${OUTPUT_DIR}" RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
    if(NOT "${status}" STREQUAL "${expected}")
        message(FATAL_ERROR "Expected ${expected}, got ${status}: ${ARGN}\n${stdout}\n${stderr}")
    endif()
    set(out "${stdout}" PARENT_SCOPE)
    set(err "${stderr}" PARENT_SCOPE)
endfunction()

function(reject text pattern)
    file(WRITE "${OUTPUT_DIR}/invalid.ar" "${text}")
    check_command(1 "${driver}" ${ARGN} -S invalid.ar)
    if(NOT err MATCHES "${pattern}")
        message(FATAL_ERROR "Missing diagnostic ${pattern}\n${err}")
    endif()
    set(err "${err}" PARENT_SCOPE)
endfunction()

foreach(level O0 O1 O2)
    check_command(0 "${driver}" --meta "${plugin}" -${level} --exe -o "pipeline-${level}.exe" "${example}")
    check_command(11 "${OUTPUT_DIR}/pipeline-${level}.exe")
    file(READ "${OUTPUT_DIR}/pipeline-${level}.exe" binary HEX)
    string(HEX "GENERATOR_ONLY_SENTINEL" sentinel)
    if(binary MATCHES "${sentinel}")
        message(FATAL_ERROR "Generator-only string reached ${level} executable")
    endif()
    check_command(0 "${driver}" -${level} --meta "${plugin}" -S "${example}")
    if(out MATCHES "atri_fn_[0-9]+" OR NOT out MATCHES "main:")
        message(FATAL_ERROR "Unexpected runtime functions in ${level} assembly\n${out}")
    endif()
endforeach()

get_filename_component(toolchain "${CLANG}" DIRECTORY)
find_program(readobj NAMES llvm-readobj HINTS "${toolchain}")
find_program(objdump NAMES llvm-objdump HINTS "${toolchain}")
if(readobj AND objdump)
    foreach(level O0 O1 O2)
        check_command(0 "${driver}" -${level} --meta "${plugin}" -c -o "pipeline-${level}.obj" "${example}")
        check_command(0 "${objdump}" -d "pipeline-${level}.obj")
        if(NOT out MATCHES "<main>:" OR out MATCHES "<atri_fn_[0-9]+>:")
            message(FATAL_ERROR "Unexpected functions in disassembled COFF\n${out}")
        endif()
        check_command(0 "${readobj}" --symbols "pipeline-${level}.obj")
        if(NOT out MATCHES "Name: main" OR out MATCHES "Name: atri_fn_|Name: (emit|triple)")
            message(FATAL_ERROR "Unexpected COFF symbols\n${out}")
        endif()
    endforeach()
endif()

file(WRITE "${OUTPUT_DIR}/input.bin" "Z")
file(WRITE "${OUTPUT_DIR}/trace.bin" "")
check_command(0 "${driver}" --exe -o io.exe "${SOURCE_DIR}/tests/cases/meta/build_io.ar")
check_command(8 "${OUTPUT_DIR}/io.exe")
file(READ "${OUTPUT_DIR}/trace.bin" trace)
if(NOT trace STREQUAL "ABB")
    message(FATAL_ERROR "Expected one lexical evaluation and two generator calls, saw '${trace}'")
endif()
check_command(0 "${driver}" -S "${SOURCE_DIR}/tests/cases/meta/build_io.ar")
if(out MATCHES "fgetc|fputc|fopen|fclose|atri_fn_[0-9]+")
    message(FATAL_ERROR "Generator-only functions or imports reached assembly\n${out}")
endif()

if(readobj)
    foreach(level O0 O1 O2)
        check_command(0 "${driver}" -${level} --exe -o "io-${level}.exe" "${SOURCE_DIR}/tests/cases/meta/build_io.ar")
        check_command(0 "${readobj}" --coff-imports "io-${level}.exe")
        if(out MATCHES "Symbol: (fgetc|fputc|fopen|fclose)( |\\()")
            message(FATAL_ERROR "Build-only stdio imports in ${level} executable\n${out}")
        endif()
    endforeach()
endif()

file(WRITE "${OUTPUT_DIR}/gen.ar" [=[export fn emit(s:*u8)->*u8{return s;}]=])
file(WRITE "${OUTPUT_DIR}/nested.ar" [=[
gen :: import "gen";
fn main()->i32 { return generated() + #insert #run gen::emit("#insert #run gen::emit(\"2\")"); }
#insert #run gen::emit("fn genera")ted()->i32 { return 2 * #insert #run gen::emit("1 + 2;") }
]=])
check_command(0 "${driver}" --exe -o nested.exe nested.ar)
check_command(6 "${OUTPUT_DIR}/nested.exe")

file(WRITE "${OUTPUT_DIR}/string.ar" [=[
fn message()->*u8{return "A\0B";}
@unregistered fn main()->i32 {p:*u8=#run message();return cast(i32,*(p+2));}
]=])
check_command(0 "${driver}" --exe -o string.exe string.ar)
check_command(66 "${OUTPUT_DIR}/string.exe")

file(WRITE "${OUTPUT_DIR}/identity.ar" "fn identity(x:i32)->i32{return x+0;}")
check_command(0 "${driver}" --emit-ir-before-opt identity.ar)
if(NOT out MATCHES "add i32")
    message(FATAL_ERROR "Missing bytecode add before O1\n${out}")
endif()
check_command(0 "${driver}" -O1 --emit-ir identity.ar)
if(out MATCHES "add i32" OR NOT out MATCHES "define i32 @atri_fn_0")
    message(FATAL_ERROR "O1 failed to simplify LLVM integer identity\n${out}")
endif()

reject("fn main()->i32{return #insert #run later();}fn later()->*u8{return \"1\";}" "already defined")
reject("fn emit(x:i32)->*u8{return \"1\";}fn main()->i32{return #insert #run emit(1+1);}" "after generator arguments")
reject("n:i32:=1;fn emit()->*u8{if(n==1){return \"1\";}return \"2\";}#insert #run emit()" "program globals are unavailable")
reject("fn emit()->i32{return 1;}#insert #run emit()" "must return \\*u8")
reject("fn emit()->*u8{return \"return 1 + ;\";}fn main()->i32 {#insert #run emit()}" "<generated by emit>:1:12")
if(NOT err MATCHES "invalid.ar:1:[0-9]+: note: expanded or rewritten here")
    message(FATAL_ERROR "Missing expansion origin\n${err}")
endif()
reject("fn empty()->*u8{return null;} fn main()->i32{return cast(i32,*(#run empty()));}" "statically known null")
reject("fn main()->i32{return foo(true);}fn foo(x:i32)->i32{return x;}" "expected i32, got bool")
if(NOT err MATCHES "parameter declared here")
    message(FATAL_ERROR "Missing parameter location\n${err}")
endif()
file(WRITE "${OUTPUT_DIR}/broken.ar" [=[export fn fail()->*u8{return null;}]=])
reject("m :: import \"broken\"; #insert #run m::fail()" "broken.ar:1:.*error:")
if(NOT err MATCHES "invalid.ar:1:.*note: generator needed")
    message(FATAL_ERROR "Missing cross-module insertion location\n${err}")
endif()
reject("fn main()->i32{return 1;}" "duplicate annotation registration" --meta "${plugin}" --meta "${plugin}")

check_command(0 "${CLANG}" -shared "${SOURCE_DIR}/tests/meta_listener.c" "-I${SOURCE_DIR}/src" -o first.dll)
file(WRITE "${OUTPUT_DIR}/repair.ar" "fn invalid_edit()->i32{return 0;}fn entry()->i32{return invalid_edit();}")
check_command(0 "${driver}" --meta "${OUTPUT_DIR}/first.dll" --meta "${SOURCE_DIR}/tests/cases/meta/repair.ar"
    --emit-ir-before-opt repair.ar)
if(NOT out MATCHES "func @late" OR NOT out MATCHES "call i32 @late")
    message(FATAL_ERROR "Lost injected declaration or its reference\n${out}")
endif()

file(WRITE "${OUTPUT_DIR}/helper.ar" "export fn text(id:u64)->*u8{return #keyword(id);}")
file(WRITE "${OUTPUT_DIR}/import_listener.ar" [=[
h :: import "helper";
fn atri_on_decl_v1(id:u64,k:u32,m:*u8,n:*u8,s:*u8,r:**u8,i:**u8)->i32{
    if(h::text(id)==null){return 1;}return 0;
}
]=])
check_command(0 "${driver}" --meta "${OUTPUT_DIR}/import_listener.ar" -S identity.ar)
reject("fn reject_here()->i32{return 0;} $$$" "callback reported failure" --meta "${OUTPUT_DIR}/first.dll")

file(WRITE "${OUTPUT_DIR}/deferred.ar" [=[
#extern(C) fn puts(s:*u8)->i32;
fn mark()->i32{puts("once");return 1;}
fn use(b:bool)->i32 {defer {#run mark();} if(b){return 1;} return 0;}
fn main()->i32{return use(false);}
]=])
check_command(0 "${driver}" --exe -o defer.exe deferred.ar)
string(STRIP "${out}" calls)
if(NOT calls STREQUAL "once")
    message(FATAL_ERROR "Unexpected compile-time I/O when lowering defer exits: ${calls}")
endif()
check_command(0 "${OUTPUT_DIR}/defer.exe")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "Compile-time effects reached runtime")
endif()

file(WRITE "${OUTPUT_DIR}/fragment.txt" "fn from_file()->i32{return 31;}")
file(WRITE "${OUTPUT_DIR}/read.ar" [=[
struct FILE;
#extern(C) fn fopen(path:*u8,mode:*u8)->*FILE;
#extern(C) fn fread(p:*void,size:u64,count:u64,f:*FILE)->u64;
#extern(C) fn fclose(f:*FILE)->i32;
#extern(C) fn malloc(size:u64)->*void;
fn read_source(path:*u8)->*u8 {
    f:*FILE=fopen(path,"rb");
    if(f==null){return null;}
    bytes:*u8=cast(*u8,malloc(257));
    n:u64=fread(cast(*void,bytes),1,256,f);
    fclose(f);
    *(bytes+cast(i64,n))=0;
    return bytes;
}
fn main()->i32{return from_file();}
#insert #run read_source("fragment.txt")
]=])
check_command(0 "${driver}" --exe -o read.exe read.ar)
check_command(31 "${OUTPUT_DIR}/read.exe")

get_filename_component(driver_dir "${driver}" DIRECTORY)
check_command(0 "${CLANG}" -shared "${SOURCE_DIR}/tests/meta_syntax_listener.c" "-I${SOURCE_DIR}/src"
    "${driver_dir}/atri-cc.lib" -o syntax.dll)
set(syntax "${OUTPUT_DIR}/syntax.dll")
file(WRITE "${OUTPUT_DIR}/ops.ar" [=[
@left fn a()->i32{return 9 <> 3 <> 1;}
@right fn b()->i32{return 9 <> 3 <> 1;}
@deref fn deref(p:*i32)->i32{return &p;}
fn main()->i32{n:i32=21;return deref(&n)+a()+b();}
]=])
check_command(0 "${driver}" --meta "${syntax}" --exe -o ops.exe ops.ar)
check_command(33 "${OUTPUT_DIR}/ops.exe")
reject("@left @right fn f()->i32{return 0;}" "conflicting operator definitions" --meta "${syntax}")
reject("@left @skip Bits :: enum { One = 1 <> 1 }" "custom operator must be rewritten" --meta "${syntax}")
reject("fn emit()->*u8{return \"fn one()->void{} fn two()->void{}\";} fn many()->void{}"
    "replacement must contain exactly one declaration" --meta "${syntax}")

file(WRITE "${OUTPUT_DIR}/edge.ar" [=[
fn text(s:*u8)->*u8{return s;}
Flag #insert #run text(":: enum { A = 7 }")
struct Holder { value:i32; } #insert #run text(";")
fn main()->i32 {return Flag::A /#insert #run text("* ignored */") + 2;}
]=])
check_command(0 "${driver}" --exe -o edge.exe edge.ar)
check_command(9 "${OUTPUT_DIR}/edge.exe")

reject("fn again()->*u8{return \"#insert #run again()\";} #insert #run again()" "source expansion exceeded")
if(NOT err MATCHES "skipped [0-9]+ intermediate source expansions")
    message(FATAL_ERROR "Missing bounded recursive expansion trace")
endif()
