cmake_minimum_required(VERSION 3.16)
project(mdp_msquic C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(MDP_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(MDP_ENABLE_MSQUIC_SMOKE_TEST "Enable end-to-end MsQuic smoke test (requires openssl, bash, python3, and binding a UDP port)" OFF)

add_library(mdp_core
    src/mdp.c
)

target_include_directories(mdp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(mdp_server src/server.c)
add_executable(mdp_client src/client.c)

target_link_libraries(mdp_server PRIVATE mdp_core)
target_link_libraries(mdp_client PRIVATE mdp_core)

find_path(MSQUIC_INCLUDE_DIR msquic.h)
find_library(MSQUIC_LIBRARY NAMES msquic)

if (MSQUIC_INCLUDE_DIR AND MSQUIC_LIBRARY)
    target_include_directories(mdp_server PRIVATE ${MSQUIC_INCLUDE_DIR})
    target_include_directories(mdp_client PRIVATE ${MSQUIC_INCLUDE_DIR})
    target_link_libraries(mdp_server PRIVATE ${MSQUIC_LIBRARY})
    target_link_libraries(mdp_client PRIVATE ${MSQUIC_LIBRARY})
    if (UNIX)
        target_link_libraries(mdp_server PRIVATE pthread dl)
        target_link_libraries(mdp_client PRIVATE pthread dl)
    endif()
else()
    message(WARNING "MsQuic not found. mdp_server and mdp_client will not be built. Provide MSQUIC_INCLUDE_DIR and MSQUIC_LIBRARY to enable them.")
    set_target_properties(mdp_server PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(mdp_client PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

enable_testing()

add_executable(test_mdp_core tests/test_mdp.c)
target_link_libraries(test_mdp_core PRIVATE mdp_core)
add_test(NAME test_mdp_core COMMAND test_mdp_core)

if (MDP_ENABLE_MSQUIC_SMOKE_TEST)
    add_test(
        NAME smoke_msquic
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_msquic.sh $<TARGET_FILE:mdp_server> $<TARGET_FILE:mdp_client>
    )
endif()



if (MDP_WARNINGS_AS_ERRORS)
    if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(mdp_core PRIVATE -Wall -Wextra -Werror)
        target_compile_options(mdp_server PRIVATE -Wall -Wextra -Werror)
        target_compile_options(mdp_client PRIVATE -Wall -Wextra -Werror)
        target_compile_options(test_mdp_core PRIVATE -Wall -Wextra -Werror)
    endif()
else()
    if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(mdp_core PRIVATE -Wall -Wextra)
        target_compile_options(mdp_server PRIVATE -Wall -Wextra)
        target_compile_options(mdp_client PRIVATE -Wall -Wextra)
        target_compile_options(test_mdp_core PRIVATE -Wall -Wextra)
    endif()
endif()
