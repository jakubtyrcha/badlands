# T3: the hash is sensitive to content and its coverage is DERIVED.
#
# Run as a ctest via `cmake -P`, because the thing under test is a CMake script
# rather than C++. It drives StageShapeshifterShaders.cmake against throwaway
# copies, so it exercises the real staging code and not a re-implementation.
#
# Inputs: EDITOR_SHADERS, COMMON_SLANG, STAGE_SCRIPT, WORK.

function(stage_and_hash out_var editor_dir)
    # `keep` = do NOT wipe the staged root first, which is the only way to
    # exercise incremental staging -- the case where a file must be PRUNED
    # because it no longer exists in the sources.
    if(NOT ARGV2 STREQUAL "keep")
        file(REMOVE_RECURSE ${WORK}/staged)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND}
                -DEDITOR_SHADERS=${editor_dir}
                -DCOMMON_SLANG=${COMMON_SLANG}
                -DSTAGED=${WORK}/staged
                -DGEN_HEADER=${WORK}/staged_manifest.h
                -P ${STAGE_SCRIPT}
        RESULT_VARIABLE rc OUTPUT_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "staging failed (${rc}) for ${editor_dir}")
    endif()
    file(READ ${WORK}/staged/MANIFEST H)
    string(STRIP "${H}" H)
    set(${out_var} "${H}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

# --- baseline --------------------------------------------------------------
file(COPY ${EDITOR_SHADERS}/ DESTINATION ${WORK}/src)
stage_and_hash(BASE ${WORK}/src)
if(BASE STREQUAL "")
    message(FATAL_ERROR "T3: the baseline hash is empty")
endif()

# --- stability: the same bytes hash the same ------------------------------
stage_and_hash(AGAIN ${WORK}/src)
if(NOT AGAIN STREQUAL BASE)
    message(FATAL_ERROR "T3: unstable hash -- ${BASE} then ${AGAIN}")
endif()

# --- sensitivity: ONE BYTE in an existing shader ---------------------------
file(READ ${WORK}/src/slang/shapeshifter/raymarch.slang ORIG)
file(WRITE ${WORK}/src/slang/shapeshifter/raymarch.slang "${ORIG} ")
stage_and_hash(EDITED ${WORK}/src)
if(EDITED STREQUAL BASE)
    message(FATAL_ERROR "T3: a one-byte shader edit did not change the hash")
endif()
file(WRITE ${WORK}/src/slang/shapeshifter/raymarch.slang "${ORIG}")

# --- sensitivity: a shared HEADER, not just an entry point -----------------
# sdf_scene.h is #included by the shaders rather than imported, and it compiles
# as C++/MSL/Slang from one source -- a change there is a shader change.
file(READ ${WORK}/src/sdf_scene.h HORIG)
file(WRITE ${WORK}/src/sdf_scene.h "${HORIG}\n// t3\n")
stage_and_hash(HEDIT ${WORK}/src)
if(HEDIT STREQUAL BASE)
    message(FATAL_ERROR "T3: an edit to a shared shader header did not change the hash")
endif()
file(WRITE ${WORK}/src/sdf_scene.h "${HORIG}")

# --- coverage: a file nobody registered anywhere ---------------------------
# THE POINT OF GLOBBING. If the covered set were a hand-written list, a new
# shader would ship unhashed and undetected.
file(WRITE ${WORK}/src/slang/shapeshifter/t3_newly_added.slang "module t3_newly_added;\n")
stage_and_hash(ADDED ${WORK}/src)
if(ADDED STREQUAL BASE)
    message(FATAL_ERROR "T3: a newly added shader was not covered by the hash")
endif()
file(REMOVE ${WORK}/src/slang/shapeshifter/t3_newly_added.slang)

# --- restoration: undoing every edit returns the baseline ------------------
stage_and_hash(RESTORED ${WORK}/src)
if(NOT RESTORED STREQUAL BASE)
    message(FATAL_ERROR
        "T3: restoring the sources did not restore the hash -- ${BASE} vs ${RESTORED}")
endif()

# --- pruning: a deleted shader must leave the STAGED tree too ---------------
# INCREMENTALLY, with no wipe between the two stagings, because copy_if_different
# only ever adds. Without a prune step the deleted file lingers in the staged
# tree and keeps being hashed, bundled and importable -- a file surviving its
# own deletion. This case is why the prune exists; it failed before it was added.
file(WRITE ${WORK}/src/slang/shapeshifter/t3_doomed.slang "module t3_doomed;\n")
stage_and_hash(WITH_DOOMED ${WORK}/src keep)
if(WITH_DOOMED STREQUAL BASE)
    message(FATAL_ERROR "T3: the added file was not hashed, so the prune case proves nothing")
endif()
file(REMOVE ${WORK}/src/slang/shapeshifter/t3_doomed.slang)
stage_and_hash(PRUNED ${WORK}/src keep)
if(NOT PRUNED STREQUAL BASE)
    message(FATAL_ERROR
        "T3: a deleted shader is lingering in the staged tree -- expected ${BASE}, got ${PRUNED}")
endif()
if(EXISTS ${WORK}/staged/slang/shapeshifter/t3_doomed.slang)
    message(FATAL_ERROR "T3: the deleted shader is still present in the staged tree")
endif()

file(REMOVE_RECURSE ${WORK})
message(STATUS "T3 ok: hash is stable, content-sensitive, and covers new files")
