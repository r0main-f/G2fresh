# libg2usb: the G2's USB connection through libusb (LibusbTransport). libusb
# is built from source with our toolchain, so universal macOS and MSVC builds
# work without a system package.
include(FetchContent)
set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBUSB_ENABLE_UDEV OFF CACHE BOOL "" FORCE) # netlink hotplug, no libudev dependency
set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libusb
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG v1.0.30-0
  GIT_SHALLOW TRUE)
if(APPLE)
  # The SDK declares pipe2 for newer macOS only: the configure check finds it,
  # but it is missing at run time on the systems we support.
  set(HAVE_PIPE2 FALSE CACHE INTERNAL "")
endif()
FetchContent_MakeAvailable(libusb)
# Linked into plugin shared objects too; third-party code, its warnings off.
set_target_properties(usb-1.0 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(usb-1.0 PRIVATE /w)
else()
  target_compile_options(usb-1.0 PRIVATE -w)
endif()

add_library(g2usb STATIC src/libusb_transport.cpp)
target_include_directories(g2usb PUBLIC include)
target_link_libraries(g2usb PUBLIC g2proto PRIVATE usb-1.0)
set_target_properties(g2usb PROPERTIES POSITION_INDEPENDENT_CODE ON)
