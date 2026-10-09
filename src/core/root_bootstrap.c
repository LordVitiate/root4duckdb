#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "duckdb.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef ROOT4DUCKDB_BUILD_ROOT_VERSION
#define ROOT4DUCKDB_BUILD_ROOT_VERSION ""
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ROOT4DUCKDB_EXPORT __attribute__((visibility("default")))
#else
#define ROOT4DUCKDB_EXPORT
#endif

extern char** environ;

extern const unsigned char _binary_root4duckdb_payload_bin_start[];
extern const unsigned char _binary_root4duckdb_payload_bin_end[];

typedef bool (*root4duckdb_payload_init_fn)(duckdb_extension_info, struct duckdb_extension_access*);

struct root_runtime
{
    char prefix[PATH_MAX];
    char libdir[PATH_MAX];
    char incdir[PATH_MAX];
    char version[128];
};

struct loaded_root_probe
{
    char libdir[PATH_MAX];
    char conflict[PATH_MAX];
};

static pthread_mutex_t g_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_registration_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_runtime_state = 0;
static char g_runtime_error[8192];

static void* g_payload_handle = NULL;
static root4duckdb_payload_init_fn g_payload_init = NULL;

static void bootstrap_trace(const char* stage)
{
    const char* debug = getenv("ROOT4DUCKDB_DEBUG");
    if (!debug || !*debug || strcmp(debug, "0") == 0)
    {
        return;
    }
    (void)fprintf(stderr, "[ROOT4DUCKDB][BOOTSTRAP] %s\n", stage);
    (void)fflush(stderr);
}

static void set_message(char* dst, size_t dst_size, const char* fmt, ...)
{
    va_list args;

    if (!dst || dst_size == 0)
    {
        return;
    }

    va_start(args, fmt);
    (void)vsnprintf(dst, dst_size, fmt, args);
    va_end(args);

    dst[dst_size - 1] = '\0';
}

static const char* base_name(const char* path)
{
    const char* slash;

    if (!path)
    {
        return "";
    }

    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int starts_with(const char* value, const char* prefix)
{
    size_t prefix_len;

    if (!value || !prefix)
    {
        return 0;
    }

    prefix_len = strlen(prefix);
    return strncmp(value, prefix, prefix_len) == 0;
}

static int is_root_runtime_library(const char* path)
{
    static const char* const prefixes[] = {"libCore.so",
                                           "libRIO.so",
                                           "libTree.so",
                                           "libHist.so",
                                           "libCling.so",
                                           "libThread.so",
                                           "libMathCore.so",
                                           "libMatrix.so",
                                           "libImt.so",
                                           "libNet.so",
                                           "libMultiProc.so",
                                           "libROOTVecOps.so",
                                           "libTreePlayer.so",
                                           "libRint.so",
                                           "libPhysics.so",
                                           "libROOTNTuple.so",
                                           "libROOTNTupleUtil.so",
                                           "libROOTDataFrame.so",
                                           "libGpad.so",
                                           "libGraf.so",
                                           "libGraf3d.so",
                                           "libPostscript.so"};

    size_t i;
    const char* name = base_name(path);

    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
    {
        if (starts_with(name, prefixes[i]))
        {
            return 1;
        }
    }

    return 0;
}

static int canonical_path(const char* path, char* out, size_t out_size, char* error, size_t error_size)
{

    char resolved[PATH_MAX];

    if (!path || !*path)
    {
        set_message(error, error_size, "empty path");
        return 0;
    }

    if (!realpath(path, resolved))
    {
        set_message(error, error_size, "cannot resolve %s: %s", path, strerror(errno));
        return 0;
    }

    const size_t resolved_length = strlen(resolved);
    if (!out || resolved_length >= out_size)
    {
        set_message(error, error_size, "resolved path is too long");
        return 0;
    }

    memcpy(out, resolved, resolved_length + 1);
    return 1;
}

static int directory_name(const char* path, char* out, size_t out_size)
{

    const char* slash;
    size_t length;

    if (!path || !*path || !out)
    {
        return 0;
    }

    slash = strrchr(path, '/');
    if (!slash)
    {
        return 0;
    }

    if (slash == path)
    {
        if (out_size < 2)
        {
            return 0;
        }
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    length = (size_t)(slash - path);
    if (length + 1 > out_size)
    {
        return 0;
    }

    memcpy(out, path, length);
    out[length] = '\0';
    return 1;
}

static void trim_in_place(char* value)
{
    char* start;
    char* end;

    if (!value)
    {
        return;
    }

    start = value;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
    {
        ++start;
    }

    if (start != value)
    {
        memmove(value, start, strlen(start) + 1);
    }

    end = value + strlen(value);
    while (end > value)
    {
        const char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            break;
        }
        --end;
    }

    *end = '\0';
}

static int loaded_root_callback(struct dl_phdr_info* info, size_t size, void* opaque)
{

    struct loaded_root_probe* probe = (struct loaded_root_probe*)opaque;

    char canonical[PATH_MAX];
    char libdir[PATH_MAX];
    char error[512];

    (void)size;

    if (!info || !info->dlpi_name || !*info->dlpi_name || !is_root_runtime_library(info->dlpi_name))
    {
        return 0;
    }

    if (!canonical_path(info->dlpi_name, canonical, sizeof(canonical), error, sizeof(error)))
    {
        set_message(probe->conflict, sizeof(probe->conflict), "%s", info->dlpi_name);
        return 1;
    }

    if (!directory_name(canonical, libdir, sizeof(libdir)))
    {
        set_message(probe->conflict, sizeof(probe->conflict), "%s", canonical);
        return 1;
    }

    if (!probe->libdir[0])
    {
        set_message(probe->libdir, sizeof(probe->libdir), "%s", libdir);
        return 0;
    }

    if (strcmp(probe->libdir, libdir) != 0)
    {
        set_message(probe->conflict, sizeof(probe->conflict), "%s", canonical);
        return 1;
    }

    return 0;
}

static void inspect_loaded_root(struct loaded_root_probe* probe)
{
    memset(probe, 0, sizeof(*probe));
    (void)dl_iterate_phdr(loaded_root_callback, probe);
}

static int run_tool(const char* program, const char* argument, char* output, size_t output_size, char* error,
                    size_t error_size)
{

    int pipe_fd[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    int spawn_status;
    int child_status = 0;
    size_t used = 0;

    char* argv[] = {(char*)program, (char*)argument, NULL};

    if (pipe(pipe_fd) != 0)
    {
        set_message(error, error_size, "pipe failed: %s", strerror(errno));
        return 0;
    }

    spawn_status = posix_spawn_file_actions_init(&actions);
    if (spawn_status != 0)
    {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        set_message(error, error_size, "posix_spawn setup failed: %s", strerror(spawn_status));
        return 0;
    }

    if (posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(&actions, pipe_fd[0]) != 0 ||
        posix_spawn_file_actions_addclose(&actions, pipe_fd[1]) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0)
    {

        posix_spawn_file_actions_destroy(&actions);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        set_message(error, error_size, "could not configure root-config process");
        return 0;
    }

    spawn_status = posix_spawnp(&pid, program, &actions, NULL, argv, environ);

    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fd[1]);

    if (spawn_status != 0)
    {
        close(pipe_fd[0]);
        set_message(error, error_size, "cannot execute %s: %s", program, strerror(spawn_status));
        return 0;
    }

    while (used + 1 < output_size)
    {
        ssize_t count = read(pipe_fd[0], output + used, output_size - used - 1);

        if (count > 0)
        {
            used += (size_t)count;
            continue;
        }

        if (count == 0)
        {
            break;
        }

        if (errno == EINTR)
        {
            continue;
        }

        set_message(error, error_size, "cannot read %s output: %s", program, strerror(errno));

        close(pipe_fd[0]);

        while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR)
        {
        }

        return 0;
    }

    close(pipe_fd[0]);
    output[used] = '\0';

    while (waitpid(pid, &child_status, 0) < 0)
    {
        if (errno != EINTR)
        {
            set_message(error, error_size, "waitpid failed: %s", strerror(errno));
            return 0;
        }
    }

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
    {
        set_message(error, error_size, "%s %s failed", program, argument);
        return 0;
    }

    trim_in_place(output);

    if (!*output)
    {
        set_message(error, error_size, "%s %s returned an empty result", program, argument);
        return 0;
    }

    return 1;
}

static int runtime_from_root_config(const char* root_config, struct root_runtime* runtime, char* error,
                                    size_t error_size)
{

    char raw_prefix[PATH_MAX];
    char raw_libdir[PATH_MAX];
    char raw_incdir[PATH_MAX];

    if (!run_tool(root_config, "--prefix", raw_prefix, sizeof(raw_prefix), error, error_size))
    {
        return 0;
    }

    if (!run_tool(root_config, "--libdir", raw_libdir, sizeof(raw_libdir), error, error_size))
    {
        return 0;
    }

    if (!run_tool(root_config, "--incdir", raw_incdir, sizeof(raw_incdir), error, error_size))
    {
        return 0;
    }

    if (!run_tool(root_config, "--version", runtime->version, sizeof(runtime->version), error, error_size))
    {
        return 0;
    }

    if (!canonical_path(raw_prefix, runtime->prefix, sizeof(runtime->prefix), error, error_size))
    {
        return 0;
    }

    if (!canonical_path(raw_libdir, runtime->libdir, sizeof(runtime->libdir), error, error_size))
    {
        return 0;
    }

    if (!canonical_path(raw_incdir, runtime->incdir, sizeof(runtime->incdir), error, error_size))
    {
        return 0;
    }

    return 1;
}

static int runtime_from_prefix(const char* prefix, struct root_runtime* runtime, char* error, size_t error_size)
{

    char canonical_prefix[PATH_MAX];
    char root_config[PATH_MAX];

    if (!canonical_path(prefix, canonical_prefix, sizeof(canonical_prefix), error, error_size))
    {
        return 0;
    }

    if (snprintf(root_config, sizeof(root_config), "%s/bin/root-config", canonical_prefix) >= (int)sizeof(root_config))
    {
        set_message(error, error_size, "ROOT prefix is too long");
        return 0;
    }

    if (access(root_config, X_OK) != 0)
    {
        set_message(error, error_size, "no executable root-config below discovered ROOT prefix");
        return 0;
    }

    return runtime_from_root_config(root_config, runtime, error, error_size);
}

static int validate_runtime(const struct root_runtime* runtime, char* error, size_t error_size)
{

    static const char* const required[] = {"libCore.so", "libRIO.so", "libHist.so", "libTree.so",
                                           "libCling.so", "libPhysics.so"};

    size_t i;
    char root_header[PATH_MAX];

    if (snprintf(root_header, sizeof(root_header), "%s/Rtypes.h", runtime->incdir) >= (int)sizeof(root_header))
    {
        set_message(error, error_size, "ROOT include directory is too long");
        return 0;
    }

    if (access(root_header, R_OK) != 0)
    {
        set_message(error, error_size, "incomplete ROOT installation: cannot read %s", root_header);
        return 0;
    }

    if (!ROOT4DUCKDB_BUILD_ROOT_VERSION[0])
    {
        set_message(error, error_size, "ROOT4DuckDB build ROOT version is unavailable");
        return 0;
    }

    if (strcmp(runtime->version, ROOT4DUCKDB_BUILD_ROOT_VERSION) != 0)
    {
        set_message(error, error_size, "ROOT version mismatch: built with %s, discovered %s",
                    ROOT4DUCKDB_BUILD_ROOT_VERSION, runtime->version);
        return 0;
    }

    for (i = 0; i < sizeof(required) / sizeof(required[0]); ++i)
    {
        char path[PATH_MAX];
        char canonical[PATH_MAX];
        char canonical_dir[PATH_MAX];

        if (snprintf(path, sizeof(path), "%s/%s", runtime->libdir, required[i]) >= (int)sizeof(path))
        {
            set_message(error, error_size, "ROOT library path is too long");
            return 0;
        }

        if (access(path, R_OK) != 0)
        {
            set_message(error, error_size, "incomplete ROOT runtime: %s is unavailable", required[i]);
            return 0;
        }

        if (!canonical_path(path, canonical, sizeof(canonical), error, error_size))
        {
            return 0;
        }

        if (!directory_name(canonical, canonical_dir, sizeof(canonical_dir)))
        {
            set_message(error, error_size, "cannot determine ROOT library directory");
            return 0;
        }

        if (strcmp(canonical_dir, runtime->libdir) != 0)
        {
            set_message(error, error_size, "ROOT runtime crosses installation boundaries");
            return 0;
        }
    }

    return 1;
}

static int discover_runtime(struct root_runtime* runtime, char* error, size_t error_size)
{

    struct loaded_root_probe loaded;
    const char* explicit_config;
    const char* rootsys;

    inspect_loaded_root(&loaded);

    if (loaded.conflict[0])
    {
        set_message(error, error_size, "process already mixes ROOT installations: %s", loaded.conflict);
        return 0;
    }

    if (loaded.libdir[0])
    {
        char prefix[PATH_MAX];

        if (!directory_name(loaded.libdir, prefix, sizeof(prefix)))
        {
            set_message(error, error_size, "cannot derive prefix from already-loaded ROOT");
            return 0;
        }

        if (!runtime_from_prefix(prefix, runtime, error, error_size))
        {
            return 0;
        }

        if (strcmp(runtime->libdir, loaded.libdir) != 0)
        {
            set_message(error, error_size, "already-loaded ROOT disagrees with its root-config");
            return 0;
        }

        return validate_runtime(runtime, error, error_size);
    }

    explicit_config = getenv("ROOT4DUCKDB_ROOT_CONFIG");

    if (explicit_config && *explicit_config)
    {
        if (!runtime_from_root_config(explicit_config, runtime, error, error_size))
        {
            return 0;
        }

        return validate_runtime(runtime, error, error_size);
    }

    rootsys = getenv("ROOTSYS");

    if (rootsys && *rootsys)
    {
        if (runtime_from_prefix(rootsys, runtime, error, error_size) && validate_runtime(runtime, error, error_size))
        {
            return 1;
        }
    }

    if (runtime_from_root_config("root-config", runtime, error, error_size) &&
        validate_runtime(runtime, error, error_size))
    {
        return 1;
    }

    set_message(error, error_size,
                "ROOT4DuckDB could not discover a compatible ROOT runtime. "
                "No ROOT installation path is compiled into the extension. "
                "A compatible root-config must be discoverable through the "
                "current process environment.");

    return 0;
}

static int verify_loaded_root(const struct root_runtime* runtime, char* error, size_t error_size)
{

    struct loaded_root_probe loaded;

    inspect_loaded_root(&loaded);

    if (loaded.conflict[0])
    {
        set_message(error, error_size, "ROOT4DuckDB refuses to mix ROOT runtimes: %s", loaded.conflict);
        return 0;
    }

    if (loaded.libdir[0] && strcmp(loaded.libdir, runtime->libdir) != 0)
    {
        set_message(error, error_size, "ROOT4DuckDB refuses to mix ROOT runtimes");
        return 0;
    }

    return 1;
}

static void* dlopen_checked(const char* path, int flags, char* error, size_t error_size)
{

    void* handle;
    const char* dl_error;

    dlerror();
    handle = dlopen(path, flags);

    if (handle)
    {
        return handle;
    }

    dl_error = dlerror();

    set_message(error, error_size, "cannot load %s: %s", path, dl_error ? dl_error : "unknown dlopen error");

    return NULL;
}

/* Prefer the selected ROOT paths, preserving caller entries.
 * Called before libCore under our runtime mutex; other environment writers need external coordination. */
static int prepend_root_search_path(const char* variable, const char* directory, char* error, size_t error_size)
{

    const char* existing;
    size_t directory_size;
    size_t existing_size;
    size_t total_size;
    char* combined;
    int saved_errno;

    if (!variable || !*variable || !directory || !*directory)
    {
        set_message(error, error_size, "invalid ROOT search path");
        return 0;
    }

    directory_size = strlen(directory);
    existing = getenv(variable);

    if (!existing || !*existing)
    {
        if (setenv(variable, directory, 1) == 0)
        {
            return 1;
        }
        set_message(error, error_size, "cannot set %s: %s", variable, strerror(errno));
        return 0;
    }

    if (strncmp(existing, directory, directory_size) == 0 &&
        (existing[directory_size] == ':' || existing[directory_size] == '\0'))
    {
        return 1;
    }

    existing_size = strlen(existing);
    if (existing_size > SIZE_MAX - 2U || directory_size > SIZE_MAX - existing_size - 2U)
    {
        set_message(error, error_size, "%s search path is too long", variable);
        return 0;
    }

    total_size = directory_size + 1U + existing_size + 1U;
    combined = malloc(total_size);
    if (!combined)
    {
        set_message(error, error_size, "out of memory preparing %s", variable);
        return 0;
    }

    memcpy(combined, directory, directory_size);
    combined[directory_size] = ':';
    memcpy(combined + directory_size + 1U, existing, existing_size + 1U);

    if (setenv(variable, combined, 1) != 0)
    {
        saved_errno = errno;
        free(combined);
        set_message(error, error_size, "cannot set %s: %s", variable, strerror(saved_errno));
        return 0;
    }

    free(combined);
    return 1;
}

static int load_root_runtime(char* error, size_t error_size)
{

    struct root_runtime runtime;

    static const struct
    {
        const char* name;
        int flags;
    } libraries[] = {{"libCore.so", RTLD_NOW | RTLD_GLOBAL},
                     {"libRIO.so", RTLD_NOW | RTLD_GLOBAL},
                     {"libHist.so", RTLD_NOW | RTLD_GLOBAL},
                     {"libTree.so", RTLD_NOW | RTLD_GLOBAL},
                     {"libPhysics.so", RTLD_NOW | RTLD_GLOBAL},
                     {"libCling.so", RTLD_LAZY | RTLD_LOCAL}};

    size_t i;

    memset(&runtime, 0, sizeof(runtime));
    bootstrap_trace("before discover_runtime");

    if (!discover_runtime(&runtime, error, error_size))
    {
        return 0;
    }

    bootstrap_trace("after discover_runtime");
    if (!verify_loaded_root(&runtime, error, error_size))
    {
        return 0;
    }

    if (!prepend_root_search_path("ROOT_LIBRARY_PATH", runtime.libdir, error, error_size) ||
        !prepend_root_search_path("ROOT_INCLUDE_PATH", runtime.incdir, error, error_size))
    {
        return 0;
    }

    if (setenv("ROOTSYS", runtime.prefix, 1) != 0)
    {
        set_message(error, error_size, "cannot set ROOTSYS: %s", strerror(errno));
        return 0;
    }

    for (i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i)
    {
        char path[PATH_MAX];

        if (snprintf(path, sizeof(path), "%s/%s", runtime.libdir, libraries[i].name) >= (int)sizeof(path))
        {
            set_message(error, error_size, "ROOT library path is too long");
            return 0;
        }

        char stage[128];
        (void)snprintf(stage, sizeof(stage), "before dlopen %s", libraries[i].name);
        bootstrap_trace(stage);
        if (!dlopen_checked(path, libraries[i].flags, error, error_size))
        {
            return 0;
        }
        (void)snprintf(stage, sizeof(stage), "after dlopen %s", libraries[i].name);
        bootstrap_trace(stage);
    }

    return verify_loaded_root(&runtime, error, error_size);
}

static int write_all(int fd, const unsigned char* data, size_t size, char* error, size_t error_size)
{

    size_t offset = 0;

    while (offset < size)
    {
        ssize_t written = write(fd, data + offset, size - offset);

        if (written > 0)
        {
            offset += (size_t)written;
            continue;
        }

        if (written < 0 && errno == EINTR)
        {
            continue;
        }

        set_message(error, error_size, "cannot write embedded payload: %s",
                    written < 0 ? strerror(errno) : "short write");
        return 0;
    }

    return 1;
}

static int load_embedded_payload(char* error, size_t error_size)
{

    const unsigned char* begin = _binary_root4duckdb_payload_bin_start;
    const unsigned char* end = _binary_root4duckdb_payload_bin_end;

    size_t payload_size;
    int fd;
    char fd_path[128];
    void* symbol;

    if (end <= begin)
    {
        set_message(error, error_size, "embedded payload is empty");
        return 0;
    }

    payload_size = (size_t)(end - begin);

    fd = memfd_create("root4duckdb-payload", MFD_CLOEXEC);

    if (fd < 0)
    {
        set_message(error, error_size, "memfd_create failed: %s", strerror(errno));
        return 0;
    }

    if (!write_all(fd, begin, payload_size, error, error_size))
    {
        close(fd);
        return 0;
    }

    if (snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd) >= (int)sizeof(fd_path))
    {
        close(fd);
        set_message(error, error_size, "payload file descriptor path is too long");
        return 0;
    }

    bootstrap_trace("before dlopen embedded payload");
    g_payload_handle = dlopen_checked(fd_path, RTLD_NOW | RTLD_LOCAL, error, error_size);
    bootstrap_trace("after dlopen embedded payload");

    close(fd);

    if (!g_payload_handle)
    {
        return 0;
    }

    dlerror();

    symbol = dlsym(g_payload_handle, "root4duckdb_payload_init_c");

    {
        const char* dl_error = dlerror();

        if (dl_error || !symbol)
        {
            set_message(error, error_size,
                        "embedded payload does not export "
                        "root4duckdb_payload_init_c: %s",
                        dl_error ? dl_error : "symbol not found");
            return 0;
        }
    }

    /*
     * POSIX specifies dlsym() for function symbols. memcpy avoids an
     * ISO-C object-pointer/function-pointer conversion diagnostic.
     */
    memcpy(&g_payload_init, &symbol, sizeof(g_payload_init));
    bootstrap_trace("embedded payload entry resolved");

    return 1;
}

static int initialize_runtime(char* error, size_t error_size)
{

    bootstrap_trace("before ROOT runtime load");
    if (!load_root_runtime(error, error_size))
    {
        return 0;
    }
    bootstrap_trace("after ROOT runtime load");

    if (!load_embedded_payload(error, error_size))
    {
        return 0;
    }
    bootstrap_trace("after embedded payload load");

    return 1;
}

static int ensure_runtime(char* error, size_t error_size)
{

    int lock_status;

    lock_status = pthread_mutex_lock(&g_runtime_mutex);
    if (lock_status != 0)
    {
        set_message(error, error_size, "runtime mutex lock failed: %s", strerror(lock_status));
        return 0;
    }

    if (g_runtime_state == 0)
    {
        g_runtime_error[0] = '\0';

        if (initialize_runtime(g_runtime_error, sizeof(g_runtime_error)))
        {
            g_runtime_state = 1;
        }
        else
        {
            g_runtime_state = -1;
        }
    }

    if (g_runtime_state < 0)
    {
        set_message(error, error_size, "%s",
                    g_runtime_error[0] ? g_runtime_error : "ROOT4DuckDB runtime initialization failed");

        (void)pthread_mutex_unlock(&g_runtime_mutex);
        return 0;
    }

    (void)pthread_mutex_unlock(&g_runtime_mutex);
    return 1;
}

ROOT4DUCKDB_EXPORT
bool root_init_c_api(duckdb_extension_info info, struct duckdb_extension_access* access)
{

    char error[8192];
    int lock_status;
    bool result;

    if (!info || !access || !access->set_error || !access->get_database)
    {
        return false;
    }

    error[0] = '\0';
    bootstrap_trace("root_init_c_api entered");

    if (!ensure_runtime(error, sizeof(error)))
    {
        access->set_error(info, error[0] ? error : "ROOT4DuckDB runtime initialization failed");
        return false;
    }

    bootstrap_trace("runtime ready");
    if (!g_payload_init)
    {
        access->set_error(info, "ROOT4DuckDB payload initialization function is unavailable");
        return false;
    }

    lock_status = pthread_mutex_lock(&g_registration_mutex);

    if (lock_status != 0)
    {
        set_message(error, sizeof(error), "registration mutex lock failed: %s", strerror(lock_status));
        access->set_error(info, error);
        return false;
    }

    bootstrap_trace("before payload init");
    result = g_payload_init(info, access);
    bootstrap_trace("after payload init");

    (void)pthread_mutex_unlock(&g_registration_mutex);

    return result;
}
