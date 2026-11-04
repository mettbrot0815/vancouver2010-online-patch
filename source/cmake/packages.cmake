CPMAddPackage(
    NAME opus
    GIT_REPOSITORY https://gitlab.xiph.org/xiph/opus.git
    GIT_TAG v1.6.1
    OPTIONS
        "OPUS_STATIC_RUNTIME ON"
        "OPUS_X86_PRESUME_SSE4_1 OFF"
        "OPUS_X86_PRESUME_AVX2 OFF"
        "OPUS_X86_MAY_HAVE_SSE4_1 ON"
        "OPUS_X86_MAY_HAVE_AVX2 ON"
)

CPMAddPackage(
    NAME RapidXML
    URL https://sourceforge.net/projects/rapidxml/files/rapidxml/rapidxml%201.13/rapidxml-1.13.zip/download
    DOWNLOAD_ONLY TRUE
    URL_HASH SHA256=c3f0b886374981bb20fabcf323d755db4be6dba42064599481da64a85f5b3571
)
if(RapidXML_ADDED)
    add_library(RapidXML INTERFACE)
    target_include_directories(RapidXML SYSTEM INTERFACE ${RapidXML_SOURCE_DIR})
    message(STATUS "RapidXML: ${RapidXML_SOURCE_DIR}")
endif()

# GlitchyScripts' fork, the same revision xlln-modules pins. It reads bare 0x-prefixed numbers, which
# stock rapidjson rejects outright as a parse error.
CPMAddPackage(
    NAME RapidJSON
    GIT_REPOSITORY https://gitlab.com/GlitchyScripts/rapidjson.git
    GIT_TAG 924556bbbb834b02bfe7c2e53fb59e12160504be
    DOWNLOAD_ONLY TRUE
)
if(RapidJSON_ADDED)
    add_library(RapidJSON INTERFACE)
    target_include_directories(RapidJSON SYSTEM INTERFACE ${RapidJSON_SOURCE_DIR}/include)
    message(STATUS "RapidJSON: ${RapidJSON_SOURCE_DIR}")
endif()

CPMAddPackage(
    NAME imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    DOWNLOAD_ONLY TRUE
)
if(imgui_ADDED)
    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    )
    target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR})
    set_target_properties(imgui PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    # imgui is third-party, so it does not take the target-level optimisation choice.
    target_compile_options(imgui PRIVATE $<$<CONFIG:Debug>:/Od>)
    set_target_properties(imgui PROPERTIES FOLDER "Source Files/third-party/imgui")
endif()

CPMAddPackage(
    NAME stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 5c205738c191bcb0abc65c4febfa9bd25ff35234
    DOWNLOAD_ONLY TRUE
)
if(stb_ADDED)
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
endif()

# include("${CMAKE_CURRENT_LIST_DIR}/FindDirectXSDK.cmake") # not needed with the Windows SDK
