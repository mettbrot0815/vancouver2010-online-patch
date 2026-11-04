# Script variables expected via -D inputs:
#	PROPS_FILE
#	SOURCE_DIR
#	DEST_DIR

file(TO_CMAKE_PATH "${PROPS_FILE}" PROPS_FILE)
file(TO_CMAKE_PATH "${SOURCE_DIR}" SOURCE_DIR)
file(TO_CMAKE_PATH "${DEST_DIR}" DEST_DIR)

if(NOT EXISTS "${PROPS_FILE}")
	message(FATAL_ERROR "TitleBinary.props does not exist at: ${PROPS_FILE}")
endif()

# Read the XML/Props file.
file(READ "${PROPS_FILE}" PROPS_CONTENT)

# Match <TitleBinaryPath>TagValue</TitleBinaryPath> using regex.
string(REGEX MATCH "<TitleBinaryPath>([^<]*)</TitleBinaryPath>" MATCHED_TAG "${PROPS_CONTENT}")
set(TITLE_BINARY_PATH "${CMAKE_MATCH_1}")

# Abort immediately if the tag is missing.
if(NOT MATCHED_TAG)
	message(FATAL_ERROR "TitleBinary.props could not be parsed. The <TitleBinaryPath> tag is missing.")
endif()

# Formulate final paths and copy everything.
set(FINAL_DESTINATION "${DEST_DIR}/${TITLE_BINARY_PATH}")

# Clean up path formatting (strips duplicate/trailing slashes cleanly).
get_filename_component(FINAL_DESTINATION "${FINAL_DESTINATION}" ABSOLUTE)

message(STATUS "Copying all binaries from ${SOURCE_DIR} to ${FINAL_DESTINATION}")

execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOURCE_DIR}" "${FINAL_DESTINATION}"
	RESULT_VARIABLE COPY_RESULT
)

if(NOT COPY_RESULT EQUAL 0)
	message(FATAL_ERROR "Failed to copy binaries over to target destination.")
endif()
