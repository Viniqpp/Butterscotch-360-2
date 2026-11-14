#include <xtl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>

#include "utils.h"
#include "stb_ds.h"
#include "xdk_file_system.h"

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1) // Win32 value; not in the XDK headers
#endif

static char* xdkResolvePath(FileSystem* fs, const char* relativePath) {
    XdkFileSystem* xfs = (XdkFileSystem*)fs;
    if (!xfs || !relativePath) return NULL;

    // Reject parent-directory components so game paths cannot escape the
    // selected content root. Check both slash styles before normalizing.
    const char* component = relativePath;
    while (*component) {
        while (*component == '/' || *component == '\\') ++component;
        const char* start = component;
        while (*component && *component != '/' && *component != '\\') ++component;
        if (component - start == 2 && start[0] == '.' && start[1] == '.') return NULL;
    }

    // Only an actual drive-qualified path or a rooted path bypasses data.win's
    // directory. A colon in an ordinary filename is not an absolute-path marker.
    const char* colon = strchr(relativePath, ':');
    bool deviceQualified = colon && (size_t)(colon - relativePath) <= 8 &&
                           (colon[1] == '\\' || colon[1] == '/');
    bool absolute = deviceQualified || relativePath[0] == '\\' || relativePath[0] == '/';
    size_t baseLen = absolute ? 0 : strlen(xfs->basePath);
    size_t relLen = strlen(relativePath);
    if (relLen > (size_t)-1 - baseLen - 1) return NULL;
    char* result = (char*)malloc(baseLen + relLen + 1);
    if (!result) return NULL;

    if (baseLen) memcpy(result, xfs->basePath, baseLen);
    memcpy(result + baseLen, relativePath, relLen + 1);

    // Normalize the entire joined path, including the base path copied from
    // data.win. Mixed slash styles otherwise leak into XDK file operations.
    for (char* p = result; *p; ++p) {
        if (*p == '/') *p = '\\';
    }
    return result;
}

static bool xdkGetFileSize(HANDLE file, DWORD* outSize) {
    DWORD high = 0;
    SetLastError(NO_ERROR);
    DWORD low = GetFileSize(file, &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) return false;
    if (high != 0) return false; // The engine's binary API is int32-sized.
    *outSize = low;
    return true;
}

static bool xdkReadExact(HANDLE file, uint8_t* buffer, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD received = 0;
        if (!ReadFile(file, buffer + total, size - total, &received, NULL)) return false;
        if (received == 0) return false;
        total += received;
    }
    return true;
}

static bool xdkWriteExact(HANDLE file, const uint8_t* buffer, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(file, buffer + total, size - total, &written, NULL)) return false;
        if (written == 0) return false;
        total += written;
    }
    return true;
}

static bool xdkFileExists(FileSystem* fs, const char* relativePath) {
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;
    DWORD attrs = GetFileAttributesA(path);
    free(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static char* xdkReadFileText(FileSystem* fs, const char* relativePath) {
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return NULL;

    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return NULL;

    DWORD size = 0;
    if (!xdkGetFileSize(file, &size) || size > 0x7fffffffU) {
        CloseHandle(file);
        return NULL;
    }

    // Bound the text API before adding its NUL terminator on a 32-bit target.
    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        CloseHandle(file);
        return NULL;
    }

    bool ok = xdkReadExact(file, (uint8_t*)buffer, size);
    CloseHandle(file);
    if (!ok) {
        free(buffer);
        return NULL;
    }
    buffer[size] = '\0';
    return buffer;
}

static bool xdkWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    if (!contents) return false;
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return false;

    size_t length = strlen(contents);
    bool ok = length <= 0x7fffffffU &&
              xdkWriteExact(file, (const uint8_t*)contents, (DWORD)length);
    CloseHandle(file);
    return ok;
}

static bool xdkDeleteFile(FileSystem* fs, const char* relativePath) {
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;
    BOOL ok = DeleteFileA(path);
    free(path);
    return ok != FALSE;
}

static bool xdkReadFileBinary(FileSystem* fs, const char* relativePath,
                              uint8_t** outData, int32_t* outSize) {
    if (!outData || !outSize) return false;
    *outData = NULL;
    *outSize = 0;

    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD size = 0;
    if (!xdkGetFileSize(file, &size) || size > 0x7fffffffU) {
        CloseHandle(file);
        return false;
    }

    uint8_t* data = size ? (uint8_t*)malloc(size) : NULL;
    if (size && !data) {
        CloseHandle(file);
        return false;
    }

    bool ok = xdkReadExact(file, data, size);
    CloseHandle(file);
    if (!ok) {
        free(data);
        return false;
    }

    *outData = data;
    *outSize = (int32_t)size;
    return true;
}

static bool xdkWriteFileBinary(FileSystem* fs, const char* relativePath,
                               const uint8_t* data, int32_t size) {
    if (size < 0 || (size > 0 && !data)) return false;
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = xdkWriteExact(file, data, (DWORD)size);
    CloseHandle(file);
    return ok;
}

static bool xdkRenameFile(FileSystem* fs, const char* oldRelativePath,
                          const char* newRelativePath) {
    char* oldPath = xdkResolvePath(fs, oldRelativePath);
    char* newPath = xdkResolvePath(fs, newRelativePath);
    if (!oldPath || !newPath) {
        free(oldPath);
        free(newPath);
        return false;
    }
    BOOL ok = MoveFileA(oldPath, newPath);
    free(oldPath);
    free(newPath);
    return ok != FALSE;
}

static void* xdkBinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return NULL;

    DWORD access = GENERIC_READ;
    DWORD creation = OPEN_EXISTING;
    if (mode == GML_FILE_BIN_WRITE) {
        access = GENERIC_READ | GENERIC_WRITE;
        creation = CREATE_ALWAYS;
    } else if (mode == GML_FILE_BIN_READWRITE) {
        access = GENERIC_READ | GENERIC_WRITE;
        creation = OPEN_ALWAYS;
    } else if (mode != GML_FILE_BIN_READ) {
        free(path);
        return NULL;
    }

    HANDLE file = CreateFileA(path, access, FILE_SHARE_READ, NULL,
                              creation, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    return file == INVALID_HANDLE_VALUE ? NULL : (void*)file;
}

static void xdkBinaryClose(FileSystem* fs, void* handle) {
    (void)fs;
    if (handle) CloseHandle((HANDLE)handle);
}

static int32_t xdkBinaryRead(FileSystem* fs, void* handle, void* dst, int32_t count) {
    (void)fs;
    if (!handle || count < 0 || (count > 0 && !dst)) return -1;
    if (count == 0) return 0;
    DWORD received = 0;
    if (!ReadFile((HANDLE)handle, dst, (DWORD)count, &received, NULL)) return -1;
    return (int32_t)received;
}

static int32_t xdkBinaryWrite(FileSystem* fs, void* handle, const void* src, int32_t count) {
    (void)fs;
    if (!handle || (count > 0 && !src) || count < 0) return -1;
    if (count == 0) return 0;
    DWORD written = 0;
    if (!WriteFile((HANDLE)handle, src, (DWORD)count, &written, NULL)) return -1;
    return (int32_t)written;
}

static int32_t xdkBinaryTell(FileSystem* fs, void* handle) {
    (void)fs;
    if (!handle) return -1;
    LONG high = 0;
    SetLastError(NO_ERROR);
    DWORD low = SetFilePointer((HANDLE)handle, 0, &high, FILE_CURRENT);
    if ((low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) ||
        high != 0 || low > 0x7fffffffU) return -1;
    return (int32_t)low;
}

static bool xdkBinarySeek(FileSystem* fs, void* handle, int32_t position) {
    (void)fs;
    if (!handle || position < 0) return false;
    SetLastError(NO_ERROR);
    DWORD result = SetFilePointer((HANDLE)handle, (LONG)position, NULL, FILE_BEGIN);
    return !(result == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR);
}

static int32_t xdkBinarySize(FileSystem* fs, void* handle) {
    (void)fs;
    if (!handle) return -1;
    DWORD size = 0;
    if (!xdkGetFileSize((HANDLE)handle, &size) || size > 0x7fffffffU) return -1;
    return (int32_t)size;
}

static void xdkBinaryRewrite(FileSystem* fs, void* handle) {
    (void)fs;
    if (!handle) return;
    SetLastError(NO_ERROR);
    DWORD position = SetFilePointer((HANDLE)handle, 0, NULL, FILE_BEGIN);
    if (position == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        logWarn("%s", "BS: BinaryRewrite seek failed\n");
        return;
    }
    if (!SetEndOfFile((HANDLE)handle)) {
        logWarn("%s", "BS: BinaryRewrite truncate failed\n");
    }
}

static bool xdkDirectoryExists(FileSystem* fs, const char* relativePath) {
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;
    DWORD attrs = GetFileAttributesA(path);
    free(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool xdkCreateDirectory(FileSystem* fs, const char* relativePath) {
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;
    BOOL ok = CreateDirectoryA(path, NULL);
    free(path);
    return ok != FALSE;
}

static bool xdkDeleteDirectory(FileSystem* fs, const char* relativePath) {
    char* path = xdkResolvePath(fs, relativePath);
    if (!path) return false;
    BOOL ok = RemoveDirectoryA(path);
    free(path);
    return ok != FALSE;
}

static FileSystemDirEntry* xdkListDirectory(FileSystem* fs, const char* relativeDirPath) {
    char* directory = xdkResolvePath(fs, relativeDirPath);
    if (!directory) return NULL;

    size_t length = strlen(directory);
    char* pattern = (char*)malloc(length + 3);
    if (!pattern) {
        free(directory);
        return NULL;
    }
    memcpy(pattern, directory, length);
    if (length && directory[length - 1] != '\\') pattern[length++] = '\\';
    pattern[length++] = '*';
    pattern[length] = '\0';

    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    free(pattern);
    free(directory);
    if (search == INVALID_HANDLE_VALUE) return NULL;

    FileSystemDirEntry* entries = NULL;
    bool allocationFailed = false;
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0) continue;
        FileSystemDirEntry entry;
        size_t nameLen = strlen(found.cFileName);
        entry.name = (char*)malloc(nameLen + 1);
        if (!entry.name) {
            allocationFailed = true;
            break;
        }
        memcpy(entry.name, found.cFileName, nameLen + 1);
        entry.isDirectory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        arrput(entries, entry);
    } while (FindNextFileA(search, &found));

    DWORD enumerationError = GetLastError();
    FindClose(search);
    if (allocationFailed || enumerationError != ERROR_NO_MORE_FILES) {
        for (size_t i = 0; i < arrlen(entries); i++) free(entries[i].name);
        arrfree(entries);
        return NULL;
    }
    return entries;
}

static FileSystemVtable xdkFileSystemVtable;
static bool xdkFileSystemVtableInitialized = false;

XdkFileSystem* XdkFileSystem_create(const char* dataWinPath) {
    XdkFileSystem* xfs = (XdkFileSystem*)calloc(1, sizeof(XdkFileSystem));
    if (!xfs) return NULL;

    if (!xdkFileSystemVtableInitialized) {
        memset(&xdkFileSystemVtable, 0, sizeof(xdkFileSystemVtable));
        xdkFileSystemVtable.resolvePath = xdkResolvePath;
        xdkFileSystemVtable.fileExists = xdkFileExists;
        xdkFileSystemVtable.readFileText = xdkReadFileText;
        xdkFileSystemVtable.writeFileText = xdkWriteFileText;
        xdkFileSystemVtable.deleteFile = xdkDeleteFile;
        xdkFileSystemVtable.readFileBinary = xdkReadFileBinary;
        xdkFileSystemVtable.writeFileBinary = xdkWriteFileBinary;
        xdkFileSystemVtable.renameFile = xdkRenameFile;
        xdkFileSystemVtable.binaryOpen = xdkBinaryOpen;
        xdkFileSystemVtable.binaryClose = xdkBinaryClose;
        xdkFileSystemVtable.binaryRead = xdkBinaryRead;
        xdkFileSystemVtable.binaryWrite = xdkBinaryWrite;
        xdkFileSystemVtable.binaryTell = xdkBinaryTell;
        xdkFileSystemVtable.binarySeek = xdkBinarySeek;
        xdkFileSystemVtable.binarySize = xdkBinarySize;
        xdkFileSystemVtable.binaryRewrite = xdkBinaryRewrite;
        xdkFileSystemVtable.directoryExists = xdkDirectoryExists;
        xdkFileSystemVtable.createDirectory = xdkCreateDirectory;
        xdkFileSystemVtable.deleteDirectory = xdkDeleteDirectory;
        xdkFileSystemVtable.listDirectory = xdkListDirectory;
        xdkFileSystemVtableInitialized = true;
    }

    xfs->base.vtable = &xdkFileSystemVtable;

    const char* lastSlash = dataWinPath ? strrchr(dataWinPath, '\\') : NULL;
    if (!lastSlash && dataWinPath) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen >= sizeof(xfs->basePath)) {
            free(xfs);
            return NULL;
        }
        memcpy(xfs->basePath, dataWinPath, dirLen);
        xfs->basePath[dirLen] = '\0';
        for (char* p = xfs->basePath; *p; ++p) {
            if (*p == '/') *p = '\\';
        }
    } else {
        strcpy(xfs->basePath, "game:\\");
    }
    return xfs;
}
