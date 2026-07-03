#include <errno.h>
#include <stdio.h>

#include <string>
#include <stdexcept>

#include "catch/catch.hpp"
#include "utils/file.h"

#if defined(OS_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace eular;

namespace {

std::string make_temp_file_path()
{
#if defined(OS_WINDOWS)
    char tempPath[MAX_PATH] = {0};
    char tempFile[MAX_PATH] = {0};
    const DWORD pathLen = GetTempPathA(MAX_PATH, tempPath);
    if (pathLen == 0 || pathLen >= MAX_PATH) {
        return "utils_test_file.tmp";
    }
    if (GetTempFileNameA(tempPath, "ut", 0, tempFile) == 0) {
        return std::string(tempPath) + "utils_test_file.tmp";
    }
    DeleteFileA(tempFile);
    return tempFile;
#else
    char tmpl[] = "/tmp/utils_file_test_XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd != -1) {
        close(fd);
        unlink(tmpl);
    }
    return tmpl;
#endif
}

struct ScopedFilePath {
    std::string path;
    ~ScopedFilePath() {
        if (!path.empty()) {
            remove(path.c_str());
        }
    }
};

} // namespace

TEST_CASE("file_info_reports_existence_and_metadata", "[file]") {
    ScopedFilePath file{make_temp_file_path()};
    const std::string content = "hello\nworld\n";

    {
        FileOp writer(file.path, FileOp::Create | FileOp::WriteOnly | FileOp::Truncate);
        REQUIRE(writer.isOpened());
        REQUIRE(writer.write(content) == static_cast<int64_t>(content.size()));
        REQUIRE(writer.flush());
    }

    REQUIRE(FileInfo::FileExist(file.path.c_str()));
    CHECK(FileInfo::GetFileSize(file.path.c_str()) == static_cast<int64_t>(content.size()));

    FileInfo info(file.path.c_str());
    CHECK(info.getFileSize() == static_cast<int64_t>(content.size()));
    CHECK(info.getModifyTime() > 0);
}

TEST_CASE("file_op_reads_writes_and_tracks_position", "[file]") {
    ScopedFilePath file{make_temp_file_path()};
    const std::string content = "hello\nworld\n";

    {
        FileOp writer;
        REQUIRE(writer.open(file.path, FileOp::Create | FileOp::WriteOnly | FileOp::Truncate));
        CHECK(writer.fileName() == file.path);
        REQUIRE(writer.write(content) == static_cast<int64_t>(content.size()));
    }

    FileOp reader(file.path, FileOp::ReadOnly);
    REQUIRE(reader.isOpened());
    CHECK(reader.fileSize() == static_cast<int64_t>(content.size()));

    char buf[6] = {0};
    REQUIRE(reader.read(buf, 5) == 5);
    CHECK(std::string(buf, 5) == "hello");
    CHECK(reader.pos() == 5);

    REQUIRE(reader.seek(0, SEEK_SET));
    CHECK(reader.readLine() == "hello");
    CHECK(!reader.eof());
    CHECK(reader.readLine() == "world");
    CHECK(reader.eof());
}

TEST_CASE("file_op_reports_errors_for_invalid_usage", "[file]") {
    char ch = 0;
    FileOp unopened;
    CHECK(unopened.read(&ch, 1) == -EBADF);
    CHECK(unopened.read(nullptr, 1) == 0);
    CHECK(!unopened.open("", FileOp::ReadOnly));

    ScopedFilePath file{make_temp_file_path()};
    FileOp invalid(file.path);
    CHECK_THROWS_AS(invalid.open(FileOp::ReadOnly | FileOp::WriteOnly), std::logic_error);
}