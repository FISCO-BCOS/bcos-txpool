set(DESTINATION_INCLUDE_DIR "${CMAKE_INSTALL_INCLUDEDIR}/bcos-txpool")
install(
    DIRECTORY "src/storage"
    DESTINATION "${DESTINATION_INCLUDE_DIR}"
    FILES_MATCHING PATTERN "*.h"
)