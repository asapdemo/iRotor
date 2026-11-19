# Copies the build outputs into <bin_dir>/release/ with the version in the name, ready to attach
# to a Gitea release:  iRotor-v1.0.1.bin / .hex / .elf + SHA256SUMS
# Runs after every link (POST_BUILD in CMakeLists.txt).
#
#   cmake -DBIN_DIR=<dir> -DNAME=<project> -DVERSION_HEADER=<fw_version.h> -P tools/package_release.cmake

file(READ "${VERSION_HEADER}" header)
if(NOT header MATCHES "#define FW_GIT_DESCRIBE \"([^\"]*)\"")
    message(FATAL_ERROR "FW_GIT_DESCRIBE not found in ${VERSION_HEADER}")
endif()
set(version "${CMAKE_MATCH_1}")

set(out_dir "${BIN_DIR}/release")
file(REMOVE_RECURSE "${out_dir}")
file(MAKE_DIRECTORY "${out_dir}")

set(sums "")
foreach(ext bin hex elf)
    set(source "${BIN_DIR}/${NAME}.${ext}")
    set(target_name "${NAME}-${version}.${ext}")
    file(COPY_FILE "${source}" "${out_dir}/${target_name}")
    file(SHA256 "${out_dir}/${target_name}" hash)
    string(APPEND sums "${hash}  ${target_name}\n")
endforeach()
file(WRITE "${out_dir}/SHA256SUMS" "${sums}")

if(version MATCHES "dirty|^[0-9a-f]+$|-g[0-9a-f]+")
    message(STATUS "Packaged ${NAME}-${version}: NOT a release build (needs a clean checkout of a vX.Y.Z tag)")
else()
    message(STATUS "Packaged release ${NAME}-${version} in ${out_dir}")
endif()
