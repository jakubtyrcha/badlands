# The editor's shader tree: hashed, staged, and made the ONLY thing anything
# reads at run time.
#
# WHY THIS EXISTS. shapeshifter compiles its Slang from disk at startup, and it
# used to do so from an absolute path into the source tree. That couples a built
# binary to whatever happens to be in the repo at launch, and the failure is
# total and silent: a binary older than the shader tree fails every pipeline,
# the renderer comes back null, and the window is black with the reason on a
# stderr that a Finder-launched app throws away. It happened -- adding
# `import output_transform;` to the four entry points made every previously
# built bundle unable to compile them.
#
# So the source tree stops being a run-time input. This file produces ONE
# canonical layout that both the app bundle and the test binaries consume:
#
#   <root>/slang/shapeshifter/*.slang   the four editor entry points
#   <root>/*.h                          shared_types.h, sdf_scene.h, ground_grid.h
#   <root>/common/*.slang               engine modules, incl. output_transform
#   <root>/MANIFEST                     the tree's content hash
#
# and a generated header carrying the same hash, so a mismatch between a binary
# and the tree it is reading is DETECTED rather than rendered.

set(SHAPESHIFTER_SHADER_SRC ${CMAKE_SOURCE_DIR}/editors/shapeshifter/shaders)
set(BADLANDS_COMMON_SLANG_SRC ${CMAKE_SOURCE_DIR}/shaders/slang/common)

# The staged tree, and the one path baked into the binaries.
#
# `shaders_staged`, NEVER `shaders`. Tier selection at run time asks whether
# <resources>/shaders/MANIFEST exists, and CFBundleCopyResourcesDirectoryURL
# reports the EXECUTABLE'S OWN DIRECTORY for a non-bundled binary -- so a test
# binary in build/ sitting next to a build/shaders/MANIFEST would silently
# select the bundle tier. Measured, not theorised.
set(SHAPESHIFTER_STAGED_SHADERS ${CMAKE_BINARY_DIR}/shaders_staged)
set(SHAPESHIFTER_SHADER_GEN_DIR ${CMAKE_BINARY_DIR}/gen/shapeshifter)

# CONFIGURE_DEPENDS covers files that did not exist when this was configured:
# adding a shader re-runs configure, which puts it in the DEPENDS list below.
# Content changes to files ALREADY in that list are covered by the list itself.
#
# EVERY file, matching what the staging script copies and hashes. Listing
# *.slang and *.h here meant a shader asset with any other extension did not
# even TRIGGER a restage -- so it reached the bundle only when some unrelated
# shader edit happened to run the script, and then rode along unhashed.
file(GLOB_RECURSE SHAPESHIFTER_SHADER_FILES CONFIGURE_DEPENDS
    ${SHAPESHIFTER_SHADER_SRC}/*
    ${BADLANDS_COMMON_SLANG_SRC}/*)
# Dotfiles are not inputs. A .DS_Store from opening the folder in Finder must
# not restage, rehash and relink the editor; the staging script skips them too.
list(FILTER SHAPESHIFTER_SHADER_FILES EXCLUDE REGEX "/\\.[^/]*$")

file(MAKE_DIRECTORY ${SHAPESHIFTER_SHADER_GEN_DIR})

# The input LIST, written to a file and depended on.
#
# Without this, DELETING a shader does not restage. add_custom_command decides
# to re-run by comparing timestamps against its DEPENDS, and a deleted file
# simply leaves that list -- every remaining input is still older than the
# outputs, so the command is "up to date" and the removed shader survives in the
# staged tree, and therefore in the bundle, until something unrelated forces a
# run. Measured. configure_file rewrites only when the content differs, so this
# file's timestamp moves exactly when the set of inputs changes.
set(SHAPESHIFTER_SHADER_LIST ${SHAPESHIFTER_SHADER_GEN_DIR}/shader_inputs.txt)
string(REPLACE ";" "\n" SHAPESHIFTER_SHADER_LIST_TEXT "${SHAPESHIFTER_SHADER_FILES}")
file(CONFIGURE OUTPUT ${SHAPESHIFTER_SHADER_LIST}
     CONTENT "${SHAPESHIFTER_SHADER_LIST_TEXT}\n" @ONLY)

add_custom_command(
    OUTPUT ${SHAPESHIFTER_SHADER_GEN_DIR}/shader_manifest.h
           ${SHAPESHIFTER_STAGED_SHADERS}/MANIFEST
    COMMAND ${CMAKE_COMMAND}
            -DEDITOR_SHADERS=${SHAPESHIFTER_SHADER_SRC}
            -DCOMMON_SLANG=${BADLANDS_COMMON_SLANG_SRC}
            -DSTAGED=${SHAPESHIFTER_STAGED_SHADERS}
            -DGEN_HEADER=${SHAPESHIFTER_SHADER_GEN_DIR}/shader_manifest.h
            -P ${CMAKE_SOURCE_DIR}/cmake/StageShapeshifterShaders.cmake
    DEPENDS ${SHAPESHIFTER_SHADER_FILES}
            ${SHAPESHIFTER_SHADER_LIST} # so a DELETION restages; see above
            ${CMAKE_SOURCE_DIR}/cmake/StageShapeshifterShaders.cmake
    COMMENT "shapeshifter: staging and hashing the shader tree"
    VERBATIM)

add_custom_target(shapeshifter_shaders
    DEPENDS ${SHAPESHIFTER_SHADER_GEN_DIR}/shader_manifest.h)

# The run-time search paths, in the staged layout. ONE definition, consumed by
# the core (baked), by the test compiler helper, and by the per-entry-point
# slangc tests -- those three lists used to be maintained by hand and had
# already drifted, which cost a build.
set(SHAPESHIFTER_STAGED_INCLUDES
    ${SHAPESHIFTER_STAGED_SHADERS}/slang/shapeshifter
    ${SHAPESHIFTER_STAGED_SHADERS}
    ${SHAPESHIFTER_STAGED_SHADERS}/common)
