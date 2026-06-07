# Generates a C++ header that embeds the iframe bootstrap script (built from
# shell/app/src/bootstrap.ts) as a raw string literal. Invoked via `cmake -P`.
#
# Required cache args:
#   -DBOOTSTRAP_JS=<path to the built bootstrap.js>
#   -DOUTPUT_HEADER=<path to the generated header>

if(NOT DEFINED BOOTSTRAP_JS)
    message(FATAL_ERROR "EmbedBootstrap: BOOTSTRAP_JS is not set")
endif()
if(NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "EmbedBootstrap: OUTPUT_HEADER is not set")
endif()
if(NOT EXISTS "${BOOTSTRAP_JS}")
    message(FATAL_ERROR "EmbedBootstrap: bootstrap source not found: ${BOOTSTRAP_JS}")
endif()

file(READ "${BOOTSTRAP_JS}" BOOTSTRAP_CONTENT)

# The R"PRISMABOOT( ... )PRISMABOOT" delimiter cannot collide with the minified
# IIFE; JavaScript output never contains the sequence )PRISMABOOT".
file(WRITE "${OUTPUT_HEADER}"
"#pragma once
// Generated from shell/app/src/bootstrap.ts. Do not edit.
namespace PrismaUI::Cef {
    inline constexpr char kBootstrapScript[] = R\"PRISMABOOT(${BOOTSTRAP_CONTENT})PRISMABOOT\";
}
")
