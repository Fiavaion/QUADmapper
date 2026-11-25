#include "MacShell.hpp"

#include "Logger.hpp"

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace quadmapper {

void macOpen(const std::string& target, bool revealInFinder) {
    std::string program = "/usr/bin/open";
    std::string reveal = "-R";
    std::string separator = "--";  // a target that starts with '-' is not an option
    std::string argument = target;
    char* arguments[5] = {program.data(), nullptr, nullptr, nullptr, nullptr};
    int next = 1;
    if (revealInFinder) arguments[next++] = reveal.data();
    arguments[next++] = separator.data();
    arguments[next++] = argument.data();

    pid_t child = 0;
    const int error = posix_spawn(&child, program.c_str(), nullptr, nullptr, arguments, environ);
    if (error != 0) {
        Logger::warning("Could not run 'open' for {} (error {})", target, error);
        return;
    }
    int status = 0;
    waitpid(child, &status, 0);  // `open` returns at once; this only reaps it
}

}  // namespace quadmapper
