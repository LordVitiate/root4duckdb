#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define DUCKDB_EXTENSION_MAIN
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef ROOT4DUCKDB_CERN_LCG_VIEW
#error "ROOT4DUCKDB_CERN_LCG_VIEW must be defined for the CERN bootstrap"
#endif

extern "C" {
extern const unsigned char _binary_root4duckdb_payload_bin_start[];
extern const unsigned char _binary_root4duckdb_payload_bin_end[];
}

namespace {

constexpr const char *kLcgView = ROOT4DUCKDB_CERN_LCG_VIEW;
void *g_payload_handle = nullptr;

std::string CanonicalPath(const std::string &path) {
    char *resolved = realpath(path.c_str(), nullptr);
    if (!resolved) {
        throw std::runtime_error(
            "ROOT4DuckDB cannot resolve " + path + ": " + std::strerror(errno));
    }
    std::string result(resolved);
    std::free(resolved);
    return result;
}

std::string DirectoryName(const std::string &path) {
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        return std::string();
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string BaseName(const std::string &path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool StartsWith(const std::string &value, const char *prefix) {
    const std::string prefix_string(prefix);
    return value.compare(0, prefix_string.size(), prefix_string) == 0;
}

bool IsRootRuntimeLibrary(const std::string &path) {
    const auto name = BaseName(path);
    static const char *const prefixes[] = {
        "libCore.so",        "libRIO.so",          "libTree.so",
        "libHist.so",        "libCling.so",        "libThread.so",
        "libMathCore.so",    "libMatrix.so",       "libImt.so",
        "libNet.so",         "libMultiProc.so",    "libROOTVecOps.so",
        "libTreePlayer.so",  "libRint.so",         "libPhysics.so",
        "libROOTNTuple.so",  "libROOTNTupleUtil.so", "libROOTDataFrame.so",
        "libGpad.so",        "libGraf.so",         "libGraf3d.so",
        "libPostscript.so"
    };

    for (const auto *prefix : prefixes) {
        if (StartsWith(name, prefix)) {
            return true;
        }
    }
    return false;
}

struct RootRuntime {
    std::string prefix;
    std::string libdir;
};

RootRuntime ResolveCernRoot() {
    const auto core = CanonicalPath(std::string(kLcgView) + "/lib/libCore.so");
    RootRuntime runtime;
    runtime.libdir = DirectoryName(core);
    runtime.prefix = DirectoryName(runtime.libdir);
    if (runtime.libdir.empty() || runtime.prefix.empty()) {
        throw std::runtime_error(
            "ROOT4DuckDB resolved an invalid CERN ROOT location: " + core);
    }
    return runtime;
}

struct LoadedRootProbe {
    std::string expected_libdir;
    std::string foreign_library;
};

int InspectLoadedObjects(dl_phdr_info *info, std::size_t, void *opaque) {
    if (!info || !info->dlpi_name || !*info->dlpi_name) {
        return 0;
    }

    const std::string loaded_path(info->dlpi_name);
    if (!IsRootRuntimeLibrary(loaded_path)) {
        return 0;
    }

    auto &probe = *static_cast<LoadedRootProbe *>(opaque);
    try {
        const auto canonical = CanonicalPath(loaded_path);
        if (DirectoryName(canonical) == probe.expected_libdir) {
            return 0;
        }
        probe.foreign_library = canonical;
    } catch (const std::exception &) {
        probe.foreign_library = loaded_path;
    }
    return 1;
}

void RejectForeignRoot(const RootRuntime &runtime) {
    LoadedRootProbe probe {runtime.libdir, std::string()};
    dl_iterate_phdr(InspectLoadedObjects, &probe);
    if (!probe.foreign_library.empty()) {
        throw std::runtime_error(
            "ROOT4DuckDB refuses to mix ROOT runtimes; already loaded: " +
            probe.foreign_library + "; required ROOT directory: " +
            runtime.libdir);
    }
}

void *DlopenChecked(const std::string &path, int flags) {
    dlerror();
    void *handle = dlopen(path.c_str(), flags);
    if (!handle) {
        const char *error = dlerror();
        throw std::runtime_error(
            "ROOT4DuckDB cannot load " + path + ": " +
            (error ? std::string(error) : std::string("unknown dlopen error")));
    }
    return handle;
}

bool Readable(const std::string &path) {
    return access(path.c_str(), R_OK) == 0;
}

void LoadOptionalLcgGlobal(const char *name) {
    const std::vector<std::string> candidates = {
        std::string(kLcgView) + "/lib/" + name,
        std::string(kLcgView) + "/lib64/" + name
    };
    for (const auto &candidate : candidates) {
        if (Readable(candidate)) {
            (void)DlopenChecked(candidate, RTLD_NOW | RTLD_GLOBAL);
            return;
        }
    }
}

void LoadRootRuntime() {
    const auto runtime = ResolveCernRoot();
    RejectForeignRoot(runtime);

    if (setenv("ROOTSYS", runtime.prefix.c_str(), 1) != 0) {
        throw std::runtime_error(
            std::string("ROOT4DuckDB cannot set ROOTSYS: ") + std::strerror(errno));
    }

    // ROOT's own libraries use $ORIGIN to find one another.  TBB belongs to
    // the LCG view rather than the ROOT package, so make the matching TBB
    // globally visible before libThread is pulled in.
    LoadOptionalLcgGlobal("libtbb.so.12");

    (void)DlopenChecked(runtime.libdir + "/libCore.so", RTLD_NOW | RTLD_GLOBAL);
    (void)DlopenChecked(runtime.libdir + "/libRIO.so", RTLD_NOW | RTLD_GLOBAL);
    (void)DlopenChecked(runtime.libdir + "/libHist.so", RTLD_NOW | RTLD_GLOBAL);
    (void)DlopenChecked(runtime.libdir + "/libTree.so", RTLD_NOW | RTLD_GLOBAL);

    // Pin Cling to the same ROOT prefix. ROOT itself intentionally keeps
    // libCling local; preloading the exact pathname prevents a later lookup
    // from selecting another installation.
    const auto cling = runtime.libdir + "/libCling.so";
    if (Readable(cling)) {
        (void)DlopenChecked(cling, RTLD_LAZY | RTLD_LOCAL);
    }

    RejectForeignRoot(runtime);
}

int CreatePayloadMemfd() {
    const int fd = memfd_create("root4duckdb-payload", MFD_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("ROOT4DuckDB memfd_create failed: ") + std::strerror(errno));
    }
    return fd;
}

void WriteAll(int fd, const unsigned char *data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto written = write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::string("ROOT4DuckDB cannot write embedded payload: ") +
                std::strerror(errno));
        }
        if (written == 0) {
            throw std::runtime_error(
                "ROOT4DuckDB cannot write embedded payload: short write");
        }
        offset += static_cast<std::size_t>(written);
    }
}

void *LoadEmbeddedPayload() {
    if (g_payload_handle) {
        return g_payload_handle;
    }

    const auto *begin = _binary_root4duckdb_payload_bin_start;
    const auto *end = _binary_root4duckdb_payload_bin_end;
    const auto payload_size = static_cast<std::size_t>(end - begin);
    if (payload_size == 0) {
        throw std::runtime_error("ROOT4DuckDB embedded payload is empty");
    }

    const int fd = CreatePayloadMemfd();
    try {
        WriteAll(fd, begin, payload_size);
        const std::string path = "/proc/self/fd/" + std::to_string(fd);
        g_payload_handle = DlopenChecked(path, RTLD_NOW | RTLD_LOCAL);
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
    return g_payload_handle;
}

using PayloadInit = void (*)(duckdb::ExtensionLoader &);

void InitializePayload(duckdb::ExtensionLoader &loader) {
    void *handle = LoadEmbeddedPayload();

    dlerror();
    auto *symbol = dlsym(handle, "root_duckdb_cpp_init");
    const char *error = dlerror();
    if (error || !symbol) {
        throw std::runtime_error(
            "ROOT4DuckDB embedded payload does not export root_duckdb_cpp_init: " +
            (error ? std::string(error) : std::string("symbol not found")));
    }

    auto init = reinterpret_cast<PayloadInit>(symbol);
    init(loader);
}

} // namespace

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(root, loader) {
    LoadRootRuntime();
    InitializePayload(loader);
}

}
