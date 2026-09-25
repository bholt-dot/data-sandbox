# Runs `belter --script SCRIPT` twice and fails unless both runs succeed with identical output.
foreach(run 1 2)
  execute_process(COMMAND "${BELTER}" --script "${SCRIPT}"
                  OUTPUT_VARIABLE out${run} ERROR_VARIABLE err${run} RESULT_VARIABLE rc${run})
  if(NOT rc${run} EQUAL 0)
    message(FATAL_ERROR "run ${run} failed (${rc${run}}):\n${out${run}}\n${err${run}}")
  endif()
endforeach()
if(NOT out1 STREQUAL out2)
  message(FATAL_ERROR "runs differ:\n--- run 1\n${out1}\n--- run 2\n${out2}")
endif()
message(STATUS "identical output:\n${out1}")
