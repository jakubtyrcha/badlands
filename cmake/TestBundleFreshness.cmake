# T5: if an app bundle has been built, its shaders must match this checkout.
#
# The direct check for "the shaders changed but the bundle did not" -- the exact
# state that produced a black viewport with the reason on a discarded stderr.
#
# SKIPS rather than fails when no bundle exists. The .app is built by xcodebuild,
# which `scripts/build.sh` does not run, so a fresh clone must stay green.
#
# Inputs: STAGED_MANIFEST, DERIVED_DATA.

if(NOT EXISTS ${STAGED_MANIFEST})
    message(FATAL_ERROR
        "T5: ${STAGED_MANIFEST} is missing -- the CMake build did not stage the shaders")
endif()
file(READ ${STAGED_MANIFEST} EXPECTED)
string(STRIP "${EXPECTED}" EXPECTED)

# Globbed rather than hardcoded: DerivedData's directory name carries a hash
# that differs per machine and per checkout path.
#
# THEN FILTERED TO THIS CHECKOUT, which is not optional. Another clone of this
# repo -- or the standalone shapeshifter repo this was moved in from -- has its
# own DerivedData directory under the same `Shapeshifter-*` pattern, and its
# bundle says nothing about this working tree. Unfiltered, this test failed
# against a sibling checkout's app on its first run. Xcode records the owning
# project in info.plist, so that is what decides.
file(GLOB CANDIDATES ${DERIVED_DATA}/Shapeshifter-*)
set(BUNDLES "")
foreach(dir ${CANDIDATES})
    if(NOT EXISTS ${dir}/info.plist)
        continue()
    endif()
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :WorkspacePath" ${dir}/info.plist
        OUTPUT_VARIABLE workspace OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        continue()
    endif()
    if(workspace STREQUAL ${EXPECTED_PROJECT})
        file(GLOB found ${dir}/Build/Products/*/Shapeshifter.app)
        list(APPEND BUNDLES ${found})
    endif()
endforeach()

if(NOT BUNDLES)
    message(STATUS "T5 skipped: no Shapeshifter.app built from ${EXPECTED_PROJECT} "
                   "(run xcodebuild -scheme Shapeshifter)")
    return()
endif()

foreach(app ${BUNDLES})
    set(m ${app}/Contents/Resources/shaders/MANIFEST)
    if(NOT EXISTS ${m})
        message(FATAL_ERROR
            "T5: ${app} carries no bundled shaders.\n"
            "  The 'Copy staged shaders into Resources' phase did not run, so the app "
            "would fall back to the staged tree -- and would ship unable to find shaders at all.")
    endif()
    file(READ ${m} GOT)
    string(STRIP "${GOT}" GOT)
    if(NOT GOT STREQUAL EXPECTED)
        message(FATAL_ERROR
            "T5: ${app} is stale.\n"
            "  bundled: ${GOT}\n"
            "  current: ${EXPECTED}\n"
            "  Rebuild the app: shaders changed without the bundle following.")
    endif()
    message(STATUS "T5 ok: ${app} matches ${EXPECTED}")
endforeach()
