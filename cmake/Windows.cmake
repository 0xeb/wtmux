set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TMUX_WIN32_CONPTY "auto" CACHE STRING "ConPTY provider: inbox, bundled or auto")
set_property(CACHE TMUX_WIN32_CONPTY PROPERTY STRINGS inbox bundled auto)
set(TMUX_WIN32_CONPTY_VERSION "1.25.260930003" CACHE STRING "Pinned Microsoft.Windows.Console.ConPTY version")
set(TMUX_WIN32_CONPTY_SHA256 "02B07B349AF66D801159BDF9E440D4A1CE78BB951F37FC8609731665AFDAE7EE"
    CACHE STRING "SHA-256 of the pinned Microsoft.Windows.Console.ConPTY package")
set(TMUX_WIN32_CONPTY_PACKAGE "${CMAKE_BINARY_DIR}/_deps/conpty-${TMUX_WIN32_CONPTY_VERSION}"
    CACHE PATH "Extracted Microsoft.Windows.Console.ConPTY package; fetched here when missing")
option(TMUX_WIN32_CONPTY_FETCH "Download the pinned ConPTY package when it is missing" ON)
set(TMUX_WIN32_CONPTY_SOURCE "" CACHE STRING
    "NuGet v3 service index for the ConPTY package; empty uses the enabled https sources in NuGet.Config, else nuget.org")
if(CMAKE_C_COMPILER_ARCHITECTURE_ID MATCHES "ARM64|arm64")
    set(TMUX_WIN32_ARCH arm64)
elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID MATCHES "X86|x86|i386" OR CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(TMUX_WIN32_ARCH x86)
else()
    set(TMUX_WIN32_ARCH x64)
endif()
set(TMUX_WIN32_CONPTY_DLL "${TMUX_WIN32_CONPTY_PACKAGE}/runtimes/win-${TMUX_WIN32_ARCH}/native/conpty.dll")
set(TMUX_WIN32_OPENCONSOLE "${TMUX_WIN32_CONPTY_PACKAGE}/build/native/runtimes/${TMUX_WIN32_ARCH}/OpenConsole.exe")
if(TMUX_WIN32_ARCH STREQUAL "x86")
    set(TMUX_WIN32_HOST_ARCHES x86 x64 arm64)
elseif(TMUX_WIN32_ARCH STREQUAL "x64")
    set(TMUX_WIN32_HOST_ARCHES x64 arm64)
else()
    set(TMUX_WIN32_HOST_ARCHES arm64)
endif()

function(tmux_win32_conpty_sources out)
    if(TMUX_WIN32_CONPTY_SOURCE)
        set(${out} "${TMUX_WIN32_CONPTY_SOURCE}" PARENT_SCOPE)
        return()
    endif()
    set(sources)
    set(config "$ENV{APPDATA}/NuGet/NuGet.Config")
    if(EXISTS "${config}")
        file(READ "${config}" xml)
        set(disabled)
        if(xml MATCHES "<disabledPackageSources>(.*)</disabledPackageSources>")
            string(REGEX MATCHALL "<add[^>]*>" entries "${CMAKE_MATCH_1}")
            foreach(entry IN LISTS entries)
                if(entry MATCHES "value=\"[Tt]rue\"" AND entry MATCHES "key=\"([^\"]*)\"")
                    list(APPEND disabled "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endif()
        if(xml MATCHES "<packageSources>(.*)</packageSources>")
            string(REGEX MATCHALL "<add[^>]*>" entries "${CMAKE_MATCH_1}")
            foreach(entry IN LISTS entries)
                if(entry MATCHES "key=\"([^\"]*)\"")
                    set(key "${CMAKE_MATCH_1}")
                    if(NOT key IN_LIST disabled AND entry MATCHES "value=\"(https://[^\"]*)\"")
                        list(APPEND sources "${CMAKE_MATCH_1}")
                    endif()
                endif()
            endforeach()
        endif()
    endif()
    if(NOT sources)
        set(sources "https://api.nuget.org/v3/index.json")
    endif()
    set(${out} "${sources}" PARENT_SCOPE)
endfunction()

function(tmux_win32_fetch_conpty)
    set(id microsoft.windows.console.conpty)
    set(version "${TMUX_WIN32_CONPTY_VERSION}")
    set(deps "${CMAKE_BINARY_DIR}/_deps")
    set(nupkg "${deps}/${id}.${version}.nupkg")
    if(EXISTS "${nupkg}")
        file(SHA256 "${nupkg}" hash)
        string(TOUPPER "${hash}" hash)
        string(TOUPPER "${TMUX_WIN32_CONPTY_SHA256}" expected)
        if(NOT hash STREQUAL expected)
            file(REMOVE "${nupkg}")
        endif()
    endif()
    if(NOT EXISTS "${nupkg}")
        tmux_win32_conpty_sources(sources)
        foreach(source IN LISTS sources)
            file(DOWNLOAD "${source}" "${deps}/nuget-index.json" STATUS status TLS_VERIFY ON)
            list(GET status 0 code)
            if(NOT code EQUAL 0)
                message(STATUS "ConPTY package: ${source}: ${status}")
                continue()
            endif()
            file(READ "${deps}/nuget-index.json" index)
            string(JSON count ERROR_VARIABLE error LENGTH "${index}" resources)
            if(error OR count EQUAL 0)
                continue()
            endif()
            set(base)
            math(EXPR last "${count} - 1")
            foreach(i RANGE ${last})
                string(JSON type ERROR_VARIABLE error GET "${index}" resources ${i} "@type")
                if(NOT error AND type MATCHES "^PackageBaseAddress")
                    string(JSON base GET "${index}" resources ${i} "@id")
                    break()
                endif()
            endforeach()
            if(NOT base)
                continue()
            endif()
            string(REGEX REPLACE "/+$" "" base "${base}")
            message(STATUS "ConPTY package: fetching ${id} ${version} from ${source}")
            file(DOWNLOAD "${base}/${id}/${version}/${id}.${version}.nupkg" "${nupkg}"
                STATUS status TLS_VERIFY ON)
            list(GET status 0 code)
            if(code EQUAL 0)
                break()
            endif()
            message(STATUS "ConPTY package: ${source}: ${status}")
            file(REMOVE "${nupkg}")
        endforeach()
        if(NOT EXISTS "${nupkg}")
            return()
        endif()
        file(SHA256 "${nupkg}" hash)
        string(TOUPPER "${hash}" hash)
        string(TOUPPER "${TMUX_WIN32_CONPTY_SHA256}" expected)
        if(NOT hash STREQUAL expected)
            file(REMOVE "${nupkg}")
            message(FATAL_ERROR "ConPTY package: SHA-256 mismatch for ${id} ${version}: got ${hash}, pinned ${expected}")
        endif()
    endif()
    file(REMOVE_RECURSE "${TMUX_WIN32_CONPTY_PACKAGE}")
    file(ARCHIVE_EXTRACT INPUT "${nupkg}" DESTINATION "${TMUX_WIN32_CONPTY_PACKAGE}")
    if(CMAKE_HOST_WIN32)
        find_program(TMUX_POWERSHELL NAMES pwsh powershell)
        file(WRITE "${deps}/check-conpty-signatures.ps1" [=[
param([string]$Path)
$bins = @(Get-ChildItem -LiteralPath $Path -Recurse -File |
    Where-Object { $_.Extension -in '.dll', '.exe' })
if (-not $bins) { "no binaries in $Path"; exit 1 }
$bad = $bins | Where-Object {
    $s = Get-AuthenticodeSignature -LiteralPath $_.FullName
    $s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation'
}
if ($bad) { $bad.FullName; exit 1 }
]=])
        execute_process(
            COMMAND "${TMUX_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${deps}/check-conpty-signatures.ps1" -Path "${TMUX_WIN32_CONPTY_PACKAGE}"
            RESULT_VARIABLE signed OUTPUT_VARIABLE unsigned)
        if(NOT signed EQUAL 0)
            file(REMOVE_RECURSE "${TMUX_WIN32_CONPTY_PACKAGE}")
            message(FATAL_ERROR "ConPTY package: untrusted signature on ${unsigned}")
        endif()
    endif()
endfunction()

set(TMUX_WIN32_CONPTY_BUNDLE OFF)
if(TMUX_WIN32_CONPTY STREQUAL "bundled" OR TMUX_WIN32_CONPTY STREQUAL "auto")
    set(present OFF)
    if(EXISTS "${TMUX_WIN32_CONPTY_DLL}" AND EXISTS "${TMUX_WIN32_OPENCONSOLE}")
        set(present ON)
    elseif(TMUX_WIN32_CONPTY_FETCH)
        tmux_win32_fetch_conpty()
        if(EXISTS "${TMUX_WIN32_CONPTY_DLL}" AND EXISTS "${TMUX_WIN32_OPENCONSOLE}")
            set(present ON)
        elseif(TMUX_WIN32_CONPTY STREQUAL "auto")
            message(WARNING "ConPTY package could not be fetched; building with the inbox ConPTY only")
        endif()
    endif()
    if(present)
        set(TMUX_WIN32_CONPTY_BUNDLE ON)
    elseif(TMUX_WIN32_CONPTY STREQUAL "bundled")
        message(FATAL_ERROR "TMUX_WIN32_CONPTY=bundled but the package is missing at ${TMUX_WIN32_CONPTY_PACKAGE}; enable TMUX_WIN32_CONPTY_FETCH or set TMUX_WIN32_CONPTY_PACKAGE to an extracted copy")
    endif()
elseif(NOT TMUX_WIN32_CONPTY STREQUAL "inbox")
    message(FATAL_ERROR "TMUX_WIN32_CONPTY must be inbox, bundled or auto")
endif()
find_package(Libevent CONFIG REQUIRED COMPONENTS core)
find_package(unofficial-utf8proc CONFIG REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sources.cmake)
set(TMUX_SOURCES ${TMUX_CORE_SOURCES})
list(REMOVE_ITEM TMUX_SOURCES tmux.c)
list(APPEND TMUX_SOURCES win32/win32-main.c)

option(TMUX_WIN32_IMAGES "Build image (sixel and kitty) support" ON)
if(TMUX_WIN32_IMAGES)
    find_package(ZLIB REQUIRED)
    find_package(PNG REQUIRED)
    list(APPEND TMUX_SOURCES ${TMUX_IMAGE_SOURCES})
endif()
set(TMUX_OSDEP_SOURCES
    osdep-windows.c
)

set(TMUX_COMPAT_SOURCES
    compat/imsg.c
    compat/imsg-buffer.c
    compat/vis.c
    compat/unvis.c
    compat/strlcpy.c
    compat/strlcat.c
    compat/strtonum.c
    compat/asprintf.c
    compat/reallocarray.c
    compat/recallocarray.c
    compat/explicit_bzero.c
    compat/freezero.c
    compat/base64.c
    compat/strndup.c
    compat/memmem.c
    compat/getline.c
    compat/strcasestr.c
    compat/getopt_long.c
    compat/err.c
)

set(TMUX_WIN32_SOURCES
    win32/win32-compat.c
    win32/win32-ipc.c
    win32/win32-process.c
    win32/win32-pty.c
    win32/win32-remote.c
    win32/win32-tls.c
    win32/win32-signal.c
    win32/win32-terminfo.c
    win32/win32-tty.c
)

list(APPEND TMUX_COMPAT_SOURCES compat/utf8proc.c)
list(APPEND TMUX_COMPAT_SOURCES compat/regex/regcomp.c compat/regex/regerror.c
    compat/regex/regexec.c compat/regex/regfree.c)
add_executable(tmux
    ${TMUX_SOURCES}
    ${TMUX_OSDEP_SOURCES}
    ${TMUX_COMPAT_SOURCES}
    ${TMUX_WIN32_SOURCES}
)

target_compile_definitions(tmux PRIVATE
    _WIN32
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
    PACKAGE_NAME="tmux"
    PACKAGE_VERSION="${TMUX_VERSION}"
    TMUX_VERSION="${TMUX_VERSION}"
    TMUX_MOUSE=1
    HAVE_IMSG
    HAVE_EVENT2_EVENT_H
    HAVE_STDINT_H
)

target_include_directories(tmux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/regex
    ${CMAKE_CURRENT_SOURCE_DIR}/win32
    ${CMAKE_CURRENT_SOURCE_DIR}/win32/include
)

target_link_libraries(tmux PRIVATE libevent::core)

target_link_libraries(tmux PRIVATE
    ws2_32
    advapi32
    bcrypt
    secur32
    crypt32
    ncrypt
    kernel32
    user32
    shell32
    ole32
    userenv
    ntdll
)

target_link_libraries(tmux PRIVATE utf8proc)
target_compile_definitions(tmux PRIVATE HAVE_UTF8PROC UTF8PROC_STATIC)
if(TMUX_WIN32_IMAGES)
    target_compile_definitions(tmux PRIVATE ENABLE_IMAGES HAVE_LIBPNG HAVE_LIBZ)
    target_link_libraries(tmux PRIVATE PNG::PNG ZLIB::ZLIB)
endif()

if(MSVC)
    target_compile_options(tmux PRIVATE
        /W3
        /utf-8
        /wd4005  # macro redefinition
        /wd4018  # signed/unsigned mismatch
        /wd4028  # parameter differs
        /wd4047  # indirection levels differ
        /wd4090  # const qualifier
        /wd4133  # incompatible types
        /wd4146  # unary minus on unsigned
        /wd4244  # conversion, possible loss of data
        /wd4267  # size_t to int
        /wd4456  # local declaration hides
        /wd4457  # parameter hides
        /wd4701  # potentially uninitialized
        /wd4702  # unreachable code
        /wd4996  # deprecated POSIX names
    )
    set_property(TARGET tmux PROPERTY C_STANDARD 11)
endif()

target_sources(tmux PRIVATE win32/tmux.exe.manifest)

if(TMUX_WIN32_CONPTY_BUNDLE)
    add_custom_command(TARGET tmux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TMUX_WIN32_CONPTY_DLL}" "$<TARGET_FILE_DIR:tmux>/conpty.dll"
        VERBATIM)
    install(FILES "${TMUX_WIN32_CONPTY_DLL}" DESTINATION bin)
    foreach(host ${TMUX_WIN32_HOST_ARCHES})
        set(src "${TMUX_WIN32_CONPTY_PACKAGE}/build/native/runtimes/${host}/OpenConsole.exe")
        add_custom_command(TARGET tmux POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tmux>/${host}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}" "$<TARGET_FILE_DIR:tmux>/${host}/OpenConsole.exe"
            VERBATIM)
        install(FILES "${src}" DESTINATION bin/${host})
    endforeach()
endif()

install(TARGETS tmux RUNTIME DESTINATION bin)

add_executable(wtmux-service win32/service/wtmux-service.c
    win32/service/service-proto.c win32/service/service-remote.c
    win32/win32-tls.c)
target_include_directories(wtmux-service PRIVATE win32)
target_compile_definitions(wtmux-service PRIVATE TMUX_VERSION="${TMUX_VERSION}"
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(wtmux-service PRIVATE tmux-platform wtsapi32 userenv
    advapi32 shell32 ole32 ws2_32 secur32 crypt32 ncrypt bcrypt)
install(TARGETS wtmux-service RUNTIME DESTINATION bin)

set(WTMUX_RELEASE_DEFINITIONS
    WTMUX_RELEASE="${PROJECT_VERSION}"
    WTMUX_FILE_VERSION="${PROJECT_VERSION}.0"
    WTMUX_RELEASE_MAJOR=${PROJECT_VERSION_MAJOR}
    WTMUX_RELEASE_MINOR=${PROJECT_VERSION_MINOR}
    WTMUX_RELEASE_PATCH=${PROJECT_VERSION_PATCH})
target_compile_definitions(tmux PRIVATE ${WTMUX_RELEASE_DEFINITIONS})
target_compile_definitions(wtmux-service PRIVATE ${WTMUX_RELEASE_DEFINITIONS})
target_sources(tmux PRIVATE win32/wtmux.rc)
target_sources(wtmux-service PRIVATE win32/wtmux.rc)
target_compile_definitions(wtmux-service PRIVATE WTMUX_RC_SERVICE)

message(STATUS "")
message(STATUS "tmux Windows build configuration:")
message(STATUS "  Version:    ${TMUX_VERSION}")
message(STATUS "  libevent:   ${Libevent_VERSION} (vcpkg)")
message(STATUS "  utf8proc:   vcpkg, required on Windows")
if(TMUX_WIN32_IMAGES)
    message(STATUS "  libpng:     ${PNG_VERSION_STRING} (vcpkg), zlib ${ZLIB_VERSION_STRING}")
endif()
message(STATUS "  Min build:  ${TMUX_WIN32_MIN_BUILD}")
if(TMUX_WIN32_CONPTY_BUNDLE)
    message(STATUS "  ConPTY:     bundled ${TMUX_WIN32_CONPTY_VERSION} (${TMUX_WIN32_ARCH}), inbox fallback")
else()
    message(STATUS "  ConPTY:     inbox (kernel32)")
endif()
message(STATUS "")
