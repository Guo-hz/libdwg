cmake_minimum_required(VERSION 3.10)

# ============================================================
# 强制 64 位平台（Visual Studio 生成器）
# 对于 VS 2017+，CMake 默认生成 Win32（32位），这里强制改为 x64
# ============================================================
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    if(NOT CMAKE_GENERATOR_PLATFORM OR CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "Generator platform" FORCE)
        message(STATUS "CMAKE_GENERATOR_PLATFORM forced to x64. Please re-run cmake configure if this is the first time.")
    endif()
endif()

project(libdwg2text C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ============================================================
# 1. 源文件列表
# ============================================================
set(LIB_SOURCES
    src/bits.c
    src/codepages.c
    src/common.c
    src/logging.c
    src/classes.c
    src/objects.c
    src/decode.c
    src/decode2.c
    src/decode_r11.c
    src/decode_r2007.c
    src/dwg.c
    src/hash.c
    src/dwg_api.c
    src/dynapi.c
    src/dxfclasses.c
    src/free.c
    src/geom.c
    src/out_dxf.c
    src/out_json.c
    src/out_geojson.c
    src/out_dxfb.c
    src/print.c
    src/encode.c
    src/encode2.c
    src/in_dxf.c
    src/reedsolomon.c
    src/geojson_api.c
    src/dwg_text_api.c
)

# ============================================================
# 2. 所有平台统一：自动检测头文件和函数
# ============================================================
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckTypeSize)
include(TestBigEndian)

# --- 头文件检测 ---
check_include_file("getopt.h"       HAVE_GETOPT_H)
check_include_file("unistd.h"       HAVE_UNISTD_H)
check_include_file("direct.h"       HAVE_DIRECT_H)
check_include_file("sys/stat.h"     HAVE_SYS_STAT_H)
check_include_file("sys/types.h"    HAVE_SYS_TYPES_H)
check_include_file("iconv.h"        HAVE_ICONV_H)
check_include_file("string.h"       HAVE_STRING_H)
check_include_file("strings.h"      HAVE_STRINGS_H)
check_include_file("stdlib.h"       HAVE_STDLIB_H)
check_include_file("stdio.h"        HAVE_STDIO_H)
check_include_file("math.h"         HAVE_MATH_H)
check_include_file("float.h"        HAVE_FLOAT_H)
check_include_file("ctype.h"        HAVE_CTYPE_H)
check_include_file("wchar.h"        HAVE_WCHAR_H)
check_include_file("wctype.h"       HAVE_WCTYPE_H)
check_include_file("stddef.h"       HAVE_STDDEF_H)
check_include_file("stdint.h"       HAVE_STDINT_H)
check_include_file("inttypes.h"     HAVE_INTTYPES_H)
check_include_file("limits.h"       HAVE_LIMITS_H)
check_include_file("malloc.h"       HAVE_MALLOC_H)
check_include_file("memory.h"       HAVE_MEMORY_H)
check_include_file("dirent.h"       HAVE_DIRENT_H)
check_include_file("endian.h"       HAVE_ENDIAN_H)
check_include_file("sys/endian.h"   HAVE_SYS_ENDIAN_H)
check_include_file("byteorder.h"    HAVE_BYTEORDER_H)
check_include_file("sys/byteorder.h" HAVE_SYS_BYTEORDER_H)
check_include_file("byteswap.h"     HAVE_BYTESWAP_H)
check_include_file("winsock2.h"     HAVE_WINSOCK2_H)
check_include_file("machine/endian.h" HAVE_MACHINE_ENDIAN_H)
check_include_file("sys/param.h"    HAVE_SYS_PARAM_H)
check_include_file("sys/time.h"     HAVE_SYS_TIME_H)
check_include_file("alloca.h"       HAVE_ALLOCA_H)
check_include_file("libgen.h"       HAVE_LIBGEN_H)
check_include_file("pcre2.h"        HAVE_PCRE2_H)
check_include_file("valgrind/valgrind.h" HAVE_VALGRIND_VALGRIND_H)

# --- 函数检测 ---
check_symbol_exists(floor        "math.h"     HAVE_FLOOR)
check_symbol_exists(sqrt         "math.h"     HAVE_SQRT)
check_symbol_exists(gettimeofday "sys/time.h" HAVE_GETTIMEOFDAY)
check_symbol_exists(memchr       "string.h"   HAVE_MEMCHR)
check_symbol_exists(memmem       "string.h"   HAVE_MEMMEM)
check_symbol_exists(memmove      "string.h"   HAVE_MEMMOVE)
check_symbol_exists(realloc      "stdlib.h"   HAVE_REALLOC)
check_symbol_exists(scandir      "dirent.h"   HAVE_SCANDIR)
check_symbol_exists(setenv       "stdlib.h"   HAVE_SETENV)
check_symbol_exists(strcasecmp   "strings.h"  HAVE_STRCASECMP)
check_symbol_exists(strcasestr   "string.h"   HAVE_STRCASESTR)
check_symbol_exists(strchr       "string.h"   HAVE_STRCHR)
check_symbol_exists(strnlen      "string.h"   HAVE_STRNLEN)
check_symbol_exists(strrchr      "string.h"   HAVE_STRRCHR)
check_symbol_exists(strtol       "stdlib.h"   HAVE_STRTOL)
check_symbol_exists(strtoul      "stdlib.h"   HAVE_STRTOUL)
check_symbol_exists(strtoull     "stdlib.h"   HAVE_STRTOULL)
check_symbol_exists(wcscmp       "wchar.h"    HAVE_WCSCMP)
check_symbol_exists(wcscpy       "wchar.h"    HAVE_WCSCPY)
check_symbol_exists(wcslen       "wchar.h"    HAVE_WCSLEN)
check_symbol_exists(wcsnlen      "wchar.h"    HAVE_WCSNLEN)
check_symbol_exists(basename     "libgen.h"   HAVE_BASENAME)

# --- 大小端检测 ---
test_big_endian(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
    set(WORDS_BIGENDIAN 1)
endif()

# --- 类型大小检测 ---
check_type_size(size_t  SIZE_T)
check_type_size(wchar_t WCHAR_T)
if(NOT SIZE_T)
    set(SIZE_T 8)
endif()
if(NOT WCHAR_T)
    set(WCHAR_T 4)
endif()

# --- 数学库检测（Linux/Unix 需要 -lm，Windows 内置） ---
if(NOT WIN32)
    find_library(MATH_LIB m)
    if(MATH_LIB)
        set(HAVE_LIBM 1)
    endif()
else()
    set(HAVE_LIBM 1)
endif()

# --- GCC/Clang 特有属性检测 ---
include(CheckCCompilerFlag)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(HAVE_ATTRIBUTE_VISIBILITY_DEFAULT 1)
    set(HAVE_FUNC_ATTRIBUTE_ALIGNED 1)
    set(HAVE_FUNC_ATTRIBUTE_FORMAT 1)
    set(HAVE_FUNC_ATTRIBUTE_MALLOC 1)
    set(HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL 1)
endif()

# --- PCRE2 检测 ---
check_symbol_exists(pcre2_compile_16 "pcre2.h" HAVE_PCRE2_16)

# --- 版本信息（供源代码中 PACKAGE_VERSION 等宏使用） ---
set(PACKAGE_VERSION "0.13")
set(LIBREDWG_SO_VERSION "0:14:0")

# --- lstat symlink 行为检测（仅 POSIX 平台） ---
if(UNIX)
    set(LSTAT_FOLLOWS_SLASHED_SYMLINK 1)
endif()

# ============================================================
# 3. 从模板统一生成 config.h（所有平台）
# ============================================================
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cmakeconfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.h
)

# ============================================================
# 4. 头文件搜索路径
#    注意：build 目录优先级高于 src/，确保使用自动生成的 config.h
# ============================================================
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/test
)

# ============================================================
# 5. 构建目标
# ============================================================

# --- 静态库 ---
add_library(dwgcore STATIC ${LIB_SOURCES})
target_include_directories(dwgcore PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

# MSVC 特有：禁用不安全函数警告
if(MSVC)
    target_compile_definitions(dwgcore PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# --- 主程序 ---
if(HAVE_GETOPT_H)
    add_executable(dwg2text test/dwgread.c)
else()
    add_executable(dwg2text test/dwgread.c test/getopt.c)
endif()

target_link_libraries(dwg2text dwgcore)

# MSVC: dwg2text 也需要禁用不安全函数警告
if(MSVC)
    target_compile_definitions(dwg2text PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# --- 平台特定链接库 ---
if(WIN32)
    target_link_libraries(dwgcore PUBLIC ws2_32)
endif()

if(MATH_LIB)
    target_link_libraries(dwgcore PUBLIC ${MATH_LIB})
endif()

# --- 可选 iconv ---
find_package(Iconv)
if(Iconv_FOUND)
    target_include_directories(dwgcore PUBLIC ${Iconv_INCLUDE_DIRS})
    target_link_libraries(dwgcore PUBLIC ${Iconv_LIBRARIES})
endif()

# --- 编译定义（必须在 target 创建之后） ---
if(HAVE_GETOPT_H)
    target_compile_definitions(dwg2text PRIVATE HAVE_GETOPT_H)
endif()

if(UNIX)
    target_compile_definitions(dwgcore PUBLIC _POSIX_C_SOURCE=200809L)
endif()
