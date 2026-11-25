# Runs GUEST under LOADER --run (with ARGS, a comma-separated list, after "--") and checks from outside: exit code 0,
# the line PASS, the loader's "game exited with code 0", and, when MAPPED is set, a loader line matching MAPPED, which
# shows the test exercised rt::Loader rather than the host loader. As check_loaded_bundled.cmake, for any fixture.
get_filename_component(guest_dir "${GUEST}" DIRECTORY)
get_filename_component(guest_name "${GUEST}" NAME)
set(args)
if(ARGS)
  string(REPLACE "," ";" args "${ARGS}")
  list(PREPEND args --)
endif()
execute_process(
  COMMAND ${RUNNER} "${LOADER}" --run "${guest_name}" ${args}
  WORKING_DIRECTORY "${guest_dir}"
  TIMEOUT 90
  RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE output)
string(REPLACE "\r\n" "\n" output "${output}")
set(why "")
if(NOT code STREQUAL "0")
  string(APPEND why " exit code ${code};")
endif()
if(NOT output MATCHES "(^|\n)${PASS}(\n|$)")
  string(APPEND why " no '${PASS}' line;")
endif()
if(NOT output MATCHES "(^|\n)game exited with code 0(\n|$)")
  string(APPEND why " no 'game exited with code 0' line;")
endif()
if(MAPPED AND NOT output MATCHES "${MAPPED}")
  string(APPEND why " no loader line matching '${MAPPED}';")
endif()
if(why)
  message(FATAL_ERROR "${guest_name} ${ARGS} under the loader:${why}\n${output}")
endif()
