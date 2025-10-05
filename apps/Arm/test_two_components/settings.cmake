#
# settings.cmake for test_two_components
#
# This minimal settings file is required for CAmkES VM application builds
#

# Set the rootserver/runtime for the seL4 elfloader
set(ElfloaderImage "binary" CACHE STRING "" FORCE)
set(ElfloaderMode "hypervisor" CACHE STRING "" FORCE)

# No customization needed - using defaults
