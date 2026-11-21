#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <string>
#include <unistd.h>

int main(int, char** argv) {
    const char* engine = "engine-base";
    if (__builtin_cpu_supports("x86-64-v4")) {
        engine = "engine-avx512";
        if (__builtin_cpu_supports("avx512vbmi2"))
            engine = __builtin_cpu_is("amd") ? "engine-zen4" : "engine-vbmi2";
    } else if (__builtin_cpu_supports("x86-64-v3")) {
        engine = "engine-avx2";
    }
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path));
    if (n <= 0 || size_t(n) == sizeof(path)) {
        std::fprintf(stderr, "unable to locate shopping-sessions executable\n");
        return 1;
    }
    std::string program(path, n);
    program.resize(program.rfind('/') + 1);
    program += engine;
    execv(program.c_str(), argv);
    std::fprintf(stderr, "unable to run %s: %s\n", engine, std::strerror(errno));
    return 1;
}
