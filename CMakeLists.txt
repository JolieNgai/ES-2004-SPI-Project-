cmake_minimum_required(VERSION 3.13)

# --- Find & import the Pico SDK ---
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")
elseif (DEFINED PICO_SDK_PATH AND EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
elseif (DEFINED ENV{PICO_SDK_PATH} AND EXISTS "$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
    include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
else()
    message(FATAL_ERROR
        "Could not find pico_sdk_import.cmake.\n"
        "Copy it into this folder OR configure with -DPICO_SDK_PATH=C:/.../pico-sdk")
endif()

project(spi_flash C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- FatFs + SD over SPI (vendored under ./lib/) ---
set(FATFS_SPI_DIR "${CMAKE_CURRENT_LIST_DIR}/lib/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI")
if (NOT EXISTS "${FATFS_SPI_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "FatFs_SPI not found at:\n  ${FATFS_SPI_DIR}\n"
        "Clone the repo to spi_flash/lib/no-OS-FatFS-SD-SPI-RPi-Pico")
endif()
add_subdirectory("${FATFS_SPI_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/FatFs_SPI_build")

# --- App sources ---
add_executable(spi_flash
    main.c
)


# Make sd_driver/*.h visible
set(FATFS_SPI_ROOT "${CMAKE_CURRENT_LIST_DIR}/lib/no-OS-FatFS-SD-SPI-RPi-Pico")
target_include_directories(spi_flash PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${FATFS_SPI_ROOT}/FatFs_SPI/sd_driver
)

# Keep any old main() in spi_flash.c disabled
target_compile_definitions(spi_flash PRIVATE SPI_FLASH_STANDALONE=0)

target_link_libraries(spi_flash
    pico_stdlib
    hardware_spi
    hardware_gpio
    hardware_dma
    hardware_rtc
    FatFs_SPI
)

pico_enable_stdio_usb(spi_flash 1)
pico_enable_stdio_uart(spi_flash 0)
pico_add_extra_outputs(spi_flash)
