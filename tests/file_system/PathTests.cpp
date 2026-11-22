// The lexical path helpers (ADR-0035 rule 1): normalization, joining and the parts of a path.

#include "ith/file_system/Path.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>

namespace fs = ith::fs;

TEST_SUITE("file_system.path") {
    TEST_CASE("roots are recognized on every platform") {
        CHECK(fs::rootLength("") == 0);
        CHECK(fs::rootLength("a/b") == 0);
        CHECK(fs::rootLength("/") == 1);
        CHECK(fs::rootLength("/home/ann") == 1);
        CHECK(fs::rootLength("\\Windows") == 1);
        CHECK(fs::rootLength("C:/") == 3);
        CHECK(fs::rootLength("c:\\Games") == 3);
        CHECK(fs::rootLength("C:Games") == 0);
        CHECK(fs::rootLength("//server/share/a") == 14);
        CHECK(fs::rootLength("\\\\server\\share") == 14);
        CHECK(fs::rootLength("//server") == 8);
        CHECK(fs::isAbsolutePath("C:/Games"));
        CHECK(fs::isAbsolutePath("/usr"));
        CHECK_FALSE(fs::isAbsolutePath("Content/Ship.gltf"));
    }

    TEST_CASE("normalizePath turns separators, drops dots and resolves parents") {
        CHECK(fs::normalizePath("") == "");
        CHECK(fs::normalizePath(".") == ".");
        CHECK(fs::normalizePath("a/..") == ".");
        CHECK(fs::normalizePath("a\\b\\c") == "a/b/c");
        CHECK(fs::normalizePath("a//b///c/") == "a/b/c");
        CHECK(fs::normalizePath("./a/./b/.") == "a/b");
        CHECK(fs::normalizePath("a/b/../c") == "a/c");
        CHECK(fs::normalizePath("../a/../../b") == "../../b");
        CHECK(fs::normalizePath("/") == "/");
        CHECK(fs::normalizePath("/..") == "/");
        CHECK(fs::normalizePath("/a/../../b") == "/b");
        CHECK(fs::normalizePath("C:\\Game\\.\\Content\\..\\Config\\") == "C:/Game/Config");
        CHECK(fs::normalizePath("C:/..") == "C:/");
        CHECK(fs::normalizePath("C:") == "C:");
        CHECK(fs::normalizePath("\\\\server\\share\\a\\..\\..") == "//server/share");
        CHECK(fs::normalizePath("//server/share//a") == "//server/share/a");
        // Names are kept byte for byte: no case folding, UTF-8 passes through.
        CHECK(fs::normalizePath("Ship/\xc3\x89t\xc3\xa9.gltf") == "Ship/\xc3\x89t\xc3\xa9.gltf");
    }

    TEST_CASE("normalizing twice changes nothing") {
        for (const char* path : {"a/../b", "C:\\x\\..\\..", "//s/sh/../x", "../../a", "/a/./b/", "x"}) {
            CAPTURE(path);
            const std::string once = fs::normalizePath(path);
            CHECK(fs::normalizePath(once) == once);
        }
    }

    TEST_CASE("joinPath appends relative paths and takes absolute ones whole") {
        CHECK(fs::joinPath("/home/ann", "Saved/Editor") == "/home/ann/Saved/Editor");
        CHECK(fs::joinPath("C:\\Game\\", "Config/../Saved") == "C:/Game/Saved");
        CHECK(fs::joinPath("a", "/etc") == "/etc");
        CHECK(fs::joinPath("a", "D:/x") == "D:/x");
        CHECK(fs::joinPath("a/b", "") == "a/b");
        CHECK(fs::joinPath("", "a/b") == "a/b");
        CHECK(fs::joinPath("/", "a") == "/a");
        CHECK(fs::joinPath("//server/share", "a") == "//server/share/a");
    }

    TEST_CASE("parentPath, fileName and extension take a path apart") {
        CHECK(fs::parentPath("Content/Ships/Hull.gltf") == "Content/Ships");
        CHECK(fs::parentPath("Hull.gltf") == "");
        CHECK(fs::parentPath("/Hull.gltf") == "/");
        CHECK(fs::parentPath("C:/Hull.gltf") == "C:/");
        CHECK(fs::parentPath("C:/") == "C:/");
        CHECK(fs::parentPath("/") == "/");
        CHECK(fs::parentPath("//server/share/a") == "//server/share");
        CHECK(fs::parentPath("") == "");

        CHECK(fs::fileName("Content/Ships/Hull.gltf") == "Hull.gltf");
        CHECK(fs::fileName("Hull.gltf") == "Hull.gltf");
        CHECK(fs::fileName("/a") == "a");
        CHECK(fs::fileName("C:/a") == "a");
        CHECK(fs::fileName("C:/") == "");
        CHECK(fs::fileName("//server/share") == "");

        CHECK(fs::extension("Content/Hull.gltf") == ".gltf");
        CHECK(fs::extension("archive.tar.gz") == ".gz");
        CHECK(fs::extension(".gitignore") == "");
        CHECK(fs::extension("Makefile") == "");
        CHECK(fs::extension("a.b/Makefile") == "");
        CHECK(fs::extension("..") == "");
        CHECK(fs::extension("name.") == ".");
    }
}
