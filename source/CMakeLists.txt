# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# S2 has no UHCI peripheral. esp-ml307/uart-eth-modem pull in 78/uart-uhci
# privately with a rule that only excludes esp32, and that component aborts
# the configure step when hal/uhci_ll.h is missing. Keep the header-only part
# of esp-ml307 but drop the UHCI component itself from the build.
if(IDF_TARGET STREQUAL "esp32s2")
    set(EXCLUDE_COMPONENTS "78__uart-uhci" "uart-uhci")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.0")
project(xiaozhi)
