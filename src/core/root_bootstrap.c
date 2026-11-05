#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "duckdb.h"
#include "duckdb_extension.h"
#undef duckdb_library_version

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <elf.h>
#include <sys/stat.h>
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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef ROOT4DUCKDB_BUILD_ROOT_VERSION
#define ROOT4DUCKDB_BUILD_ROOT_VERSION ""
#endif

#ifndef ROOT4DUCKDB_BUILD_DUCKDB_VERSION
#error "ROOT4DUCKDB_BUILD_DUCKDB_VERSION must be defined"
#endif

/*
 * Conda-forge's Linux sysroot may expose glibc 2.17 headers, which predate
 * memfd_create() and <linux/memfd.h>. Use its stable Linux syscall ABI rather
 * than requiring a newer libc declaration or symbol.
 */
#ifndef SYS_memfd_create
#if defined(__x86_64__) && defined(__LP64__)
#define SYS_memfd_create 319
#elif defined(__aarch64__)
#define SYS_memfd_create 279
#else
#error "ROOT4DuckDB bootstrap requires a Linux memfd_create syscall number for this architecture"
#endif
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
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

static void load_notice(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    (void)fprintf(stderr, "[ROOT4DUCKDB][LOAD] ");
    (void)vfprintf(stderr, fmt, args);
    (void)fprintf(stderr, "\n");
    (void)fflush(stderr);
    va_end(args);
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

static int elf_range(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

static int read_elf_soname(const char* path, char* soname, size_t capacity, char* error, size_t error_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        set_message(error, error_size, "cannot open ROOT Core ELF %s: %s", path, strerror(errno));
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr) || (uintmax_t)st.st_size > SIZE_MAX)
    {
        set_message(error, error_size, "cannot inspect ROOT Core ELF %s", path);
        close(fd);
        return 0;
    }

    const size_t size = (size_t)st.st_size;
    const unsigned char* data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED)
    {
        set_message(error, error_size, "cannot map ROOT Core ELF %s: %s", path, strerror(errno));
        return 0;
    }

    int ok = 0;
    soname[0] = '\0';
    const Elf64_Ehdr* header = (const Elf64_Ehdr*)data;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 || header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_type != ET_DYN || header->e_phentsize != sizeof(Elf64_Phdr) ||
        !header->e_phnum ||
        !elf_range(size, header->e_phoff, (uint64_t)header->e_phnum * sizeof(Elf64_Phdr)))
        goto done;

    const Elf64_Phdr* segments = (const Elf64_Phdr*)(data + header->e_phoff);
    const Elf64_Dyn* dynamic = NULL;
    size_t count = 0;
    for (size_t i = 0; i < header->e_phnum; ++i)
    {
        if (segments[i].p_type != PT_DYNAMIC)
            continue;
        if (!elf_range(size, segments[i].p_offset, segments[i].p_filesz) ||
            segments[i].p_filesz % sizeof(Elf64_Dyn) != 0)
            goto done;
        dynamic = (const Elf64_Dyn*)(data + segments[i].p_offset);
        count = (size_t)(segments[i].p_filesz / sizeof(Elf64_Dyn));
        break;
    }
    if (!dynamic)
        goto done;

    uint64_t string_address = 0, string_size = 0, name_offset = 0;
    int has_name = 0;
    for (size_t i = 0; i < count && dynamic[i].d_tag != DT_NULL; ++i)
    {
        if (dynamic[i].d_tag == DT_STRTAB)
            string_address = dynamic[i].d_un.d_ptr;
        else if (dynamic[i].d_tag == DT_STRSZ)
            string_size = dynamic[i].d_un.d_val;
        else if (dynamic[i].d_tag == DT_SONAME)
        {
            name_offset = dynamic[i].d_un.d_val;
            has_name = 1;
        }
    }

    if (!has_name)
    {
        ok = 1;
        goto done;
    }
    if (!string_address || !string_size || name_offset >= string_size)
        goto done;

    for (size_t i = 0; i < header->e_phnum; ++i)
    {
        const Elf64_Phdr* segment = &segments[i];
        if (segment->p_type != PT_LOAD || string_address < segment->p_vaddr)
            continue;
        uint64_t displacement = string_address - segment->p_vaddr;
        if (displacement > segment->p_filesz || string_size > segment->p_filesz - displacement)
            continue;
        uint64_t offset = segment->p_offset + displacement;
        if (!elf_range(size, offset, string_size))
            continue;
        const char* name = (const char*)(data + offset + name_offset);
        const char* terminator = memchr(name, '\0', (size_t)(string_size - name_offset));
        if (!terminator || terminator == name || memchr(name, '/', (size_t)(terminator - name)) ||
            (size_t)(terminator - name) >= capacity)
            goto done;
        memcpy(soname, name, (size_t)(terminator - name));
        soname[terminator - name] = '\0';
        ok = 1;
        break;
    }

done:
    (void)munmap((void*)data, size);
    if (!ok)
        set_message(error, error_size, "invalid or unreadable ROOT Core ELF SONAME: %s", path);
    return ok;
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
    load_notice("ROOT: discovering loaded libraries and installed runtime");
    bootstrap_trace("before discover_runtime");

    if (!discover_runtime(&runtime, error, error_size))
    {
        return 0;
    }

    bootstrap_trace("after discover_runtime");
    load_notice("ROOT: version=%s, prefix=%s", runtime.version, runtime.prefix);
    load_notice("ROOT: libraries=%s, headers=%s", runtime.libdir, runtime.incdir);
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

        int length = snprintf(path, sizeof(path), "%s/%s", runtime.libdir, libraries[i].name);
        if (length < 0 || length >= (int)sizeof(path))
        {
            set_message(error, error_size, "ROOT library path is too long");
            return 0;
        }

        if (i == 0)
        {
            char soname[PATH_MAX];
            if (!read_elf_soname(path, soname, sizeof(soname), error, error_size))
                return 0;
            if (soname[0])
            {
                length = snprintf(path, sizeof(path), "%s/%s", runtime.libdir, soname);
                if (length < 0 || length >= (int)sizeof(path))
                {
                    set_message(error, error_size, "ROOT Core SONAME path is too long");
                    return 0;
                }
                char resolved[PATH_MAX], resolved_dir[PATH_MAX];
                if (!canonical_path(path, resolved, sizeof(resolved), error, error_size) ||
                    !directory_name(resolved, resolved_dir, sizeof(resolved_dir)) || strcmp(resolved_dir, runtime.libdir) != 0)
                {
                    set_message(error, error_size, "ROOT Core SONAME escapes selected installation: %s", soname);
                    return 0;
                }
            }
            load_notice("ROOT Core: %s (ELF SONAME: %s)", path, soname[0] ? soname : "none");
        }

        load_notice("ROOT library: loading %s", path);
        char stage[128];
        (void)snprintf(stage, sizeof(stage), "before dlopen %s", base_name(path));
        bootstrap_trace(stage);
        if (!dlopen_checked(path, libraries[i].flags, error, error_size))
        {
            return 0;
        }
        (void)snprintf(stage, sizeof(stage), "after dlopen %s", base_name(path));
        bootstrap_trace(stage);
    }

    if (!verify_loaded_root(&runtime, error, error_size))
        return 0;
    load_notice("ROOT: %zu libraries loaded", sizeof(libraries) / sizeof(libraries[0]));
    return 1;
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

    fd = (int)syscall(SYS_memfd_create, "root4duckdb-payload", (unsigned int)MFD_CLOEXEC);

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

    load_notice("C++ payload: loading embedded module");
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
    load_notice("C++ payload: module loaded, entry point resolved");

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
    load_notice("START: initializing extension");
    bootstrap_trace("root_init_c_api entered");

    const duckdb_ext_api_v1* api = access->get_api ?
        (const duckdb_ext_api_v1*)access->get_api(info, DUCKDB_EXTENSION_API_VERSION_STRING) : NULL;
    const char* host_version = api && api->duckdb_library_version ? api->duckdb_library_version() : NULL;
    load_notice("DuckDB: host=%s, required=%s", host_version ? host_version : "unknown",
                ROOT4DUCKDB_BUILD_DUCKDB_VERSION);
    if (!host_version || strcmp(host_version, ROOT4DUCKDB_BUILD_DUCKDB_VERSION) != 0)
    {
        set_message(error, sizeof(error), "ROOT4DuckDB C++ payload requires DuckDB %s; host is %s",
                    ROOT4DUCKDB_BUILD_DUCKDB_VERSION, host_version ? host_version : "unavailable");
        load_notice("FAILED: %s", error);
        access->set_error(info, error);
        return false;
    }

    if (!ensure_runtime(error, sizeof(error)))
    {
        load_notice("FAILED: %s", error);
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

    load_notice("Functions: registering ROOT4DuckDB interface");
    bootstrap_trace("before payload init");
    result = g_payload_init(info, access);
    bootstrap_trace("after payload init");
    load_notice("COMPLETE: %s", result ? "extension ready" : "registration failed");

    (void)pthread_mutex_unlock(&g_registration_mutex);

    return result;
}
