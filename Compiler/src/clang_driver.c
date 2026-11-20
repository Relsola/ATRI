#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "clang_driver.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
static HANDLE inherited(HANDLE handle) {
    HANDLE result = NULL;
    DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &result, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return result;
}
/* CRT 参数引用规则：双引号前的反斜杠翻倍，末尾反斜杠也不能吃掉结束引号。 */
static char *command_line(const char *const *argv) {
    size_t size = 1;
    for (size_t i = 0; argv[i]; ++i) {
        size_t length = strlen(argv[i]);
        if (size > SIZE_MAX-4 || length > (SIZE_MAX-size-4)/2) { return NULL; }
        size += 2*length+4;
    }
    char *line = malloc(size);
    if (!line) { return NULL; }
    char *out = line;
    for (size_t i = 0; argv[i]; ++i) {
        if (i) { *out++ = ' '; }
        *out++ = '"';
        const char *p = argv[i];
        while (*p) {
            size_t slashes = 0;
            while (*p == '\\') { ++slashes; ++p; }
            if (!*p || *p == '"') { slashes *= 2; }
            memset(out, '\\', slashes);
            out += slashes;
            if (!*p) { break; }
            if (*p == '"') { *out++ = '\\'; }
            *out++ = *p++;
        }
        *out++ = '"';
    }
    *out = 0;
    return line;
}
bool atri_run_clang(const char *const *args, FILE *input, FILE *output) {
    char *line = command_line(args);
    if (!line) { return false; }
    STARTUPINFOA startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES};
    startup.hStdInput = inherited((HANDLE)_get_osfhandle(_fileno(input)));
    startup.hStdOutput = inherited((HANDLE)_get_osfhandle(_fileno(output)));
    startup.hStdError = inherited(GetStdHandle(STD_ERROR_HANDLE));
    PROCESS_INFORMATION process;
    bool ok = startup.hStdInput && startup.hStdOutput && startup.hStdError &&
        CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
    if (!ok) { fprintf(stderr, "cannot start clang (Windows error %lu)\n", GetLastError()); }
    free(line);
    if (startup.hStdInput) { CloseHandle(startup.hStdInput); }
    if (startup.hStdOutput) { CloseHandle(startup.hStdOutput); }
    if (startup.hStdError) { CloseHandle(startup.hStdError); }
    if (!ok) { return false; }
    CloseHandle(process.hThread);
    DWORD status = 1;
    ok = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hProcess);
    if (!ok || status != 0) { fprintf(stderr, "clang failed (status %lu)\n", status); }
    return ok && status == 0;
}
static char *temporary_output(const char *path) {
    char *dir = _fullpath(NULL, path, 0);
    if (!dir) { return NULL; }
    char *slash = strrchr(dir, '\\');
    if (!slash) { free(dir); return NULL; }
    slash[1] = 0;
    char temp[MAX_PATH];
    bool ok = GetTempFileNameA(dir, "atr", 0, temp) != 0;
    free(dir);
    if (!ok) { return NULL; }
    char *copy = _strdup(temp);
    if (!copy) { DeleteFileA(temp); }
    return copy;
}
static bool publish(const char *temp, const char *path) {
    return MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}
#else
#include <sys/wait.h>
#include <unistd.h>
bool atri_run_clang(const char *const *args, FILE *input, FILE *output) {
    pid_t child = fork();
    if (child < 0) { return false; }
    if (!child) {
        if (dup2(fileno(input), STDIN_FILENO) < 0 || dup2(fileno(output), STDOUT_FILENO) < 0) { _exit(126); }
        execvp(args[0], (char *const *)args);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) { if (errno != EINTR) { return false; } }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static char *temporary_output(const char *path) {
    size_t size = strlen(path) + sizeof(".XXXXXX");
    char *temp = malloc(size);
    if (!temp) { return NULL; }
    snprintf(temp, size, "%s.XXXXXX", path);
    int fd = mkstemp(temp);
    if (fd < 0) { free(temp); return NULL; }
    close(fd);
    return temp;
}
static bool publish(const char *temp, const char *path) { return rename(temp, path) == 0; }
#endif

static bool prepare(FILE *input, AtriSource source, const AtriIRProgram *ir, unsigned level, AtriDiagnostic *d) {
    if (!input) { return false; }
    bool ok;
    if (level == 1) {
        FILE *raw = tmpfile();
        ok = raw && atri_emit_llvm(raw, source, ir, d) && !fflush(raw) && !fseek(raw, 0, SEEK_SET) &&
             atri_llvm_o1(raw, input, d);
        if (raw) { fclose(raw); }
    } else {
        ok = level == 2 ? atri_emit_llvm(input, source, ir, d) : atri_emit_x64(input, source, ir, d);
    }
    return ok && !fflush(input) && !fseek(input, 0, SEEK_SET);
}
bool atri_llvm_optimize(FILE *out, AtriSource source, const AtriIRProgram *ir, unsigned level, bool llvm_text, AtriDiagnostic *d) {
    FILE *input = tmpfile(), *output = tmpfile();
    const char *args[] = {"clang", "--target=x86_64-pc-windows-msvc", "-Wno-override-module", "-x", "ir", level == 2 ? "-O2" : "-O0",
                          "-S", llvm_text ? "-emit-llvm" : "-masm=intel", "-o", "-", "-", NULL};
    bool ok = output && prepare(input, source, ir, level, d) && atri_run_clang(args, input, output) && !fseek(output, 0, SEEK_SET);
    if (ok) {
        char bytes[8192];
        size_t n;
        while ((n = fread(bytes, 1, sizeof(bytes), output)) != 0) {
            if (fwrite(bytes, 1, n, out) != n) { ok = false; break; }
        }
        if (ferror(output)) { ok = false; }
    }
    if (input) { fclose(input); }
    if (output) { fclose(output); }
    if (!ok && !d->message) { *d = (AtriDiagnostic){.span = {0}, .message = "cannot produce LLVM output (clang required)"}; }
    return ok;
}
bool atri_native_file(const char *path, AtriSource source, const AtriIRProgram *ir, unsigned level, bool link, AtriDiagnostic *d) {
    FILE *input = tmpfile();
    char *temp = temporary_output(path);
    const char *args[] = {"clang", "--target=x86_64-pc-windows-msvc", "-Wno-override-module",
        "-x", level ? "ir" : "assembler", "-o", temp, "-", NULL, NULL, NULL};
    size_t next = 8;
    if (level) { args[next++] = level == 2 ? "-O2" : "-O0"; }
    if (!link) { args[next] = "-c"; }
    bool ok = temp && prepare(input, source, ir, level, d) && atri_run_clang(args, input, stderr) && publish(temp, path);
    if (input) { fclose(input); }
    if (temp) { if (!ok) { remove(temp); } free(temp); }
    if (!ok && !d->message) { *d = (AtriDiagnostic){.span = {0}, .message = "cannot create native output (check clang, target libraries and output path)"}; }
    return ok;
}
