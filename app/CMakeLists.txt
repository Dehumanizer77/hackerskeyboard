# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.4.1)

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.

add_library( # Sets the name of the library.
             jni_pckeyboard

             # Sets the library as a shared library.
             SHARED

             # Provides a relative path to your source file(s).
             src/main/cpp/org_pocketworkstation_pckeyboard_BinaryDictionary.cpp
             src/main/cpp/dictionary.cpp
             src/main/cpp/char_utils.cpp )

# Searches for a specified prebuilt library and stores the path as a
# variable. Because CMake includes system libraries in the search path by
# default, you only need to specify the name of the public NDK library
# you want to add. CMake verifies that the library exists before
# completing its build.

find_library( # Sets the name of the path variable.
              log-lib

              # Specifies the name of the NDK library that
              # you want CMake to locate.
              log )

# Specifies libraries CMake should link to your target library. You
# can link multiple libraries, such as libraries you define in this
# build script, prebuilt third-party libraries, or system libraries.

target_link_libraries( # Specifies the target library.
                       jni_pckeyboard

                       # Links the target library to the log library
                       # included in the NDK.
                       ${log-lib} )

# Hardening. The dictionary parser reads attacker-influenced input (see
# SECURITY-REVIEW.md, HK-01/HK-02), so the usual mitigations are made explicit
# rather than left to whatever the toolchain happens to default to.
target_link_options( jni_pckeyboard PRIVATE
                     # Full RELRO: relocations resolved and made read-only at load.
                     -Wl,-z,relro -Wl,-z,now
                     # Non-executable stack.
                     -Wl,-z,noexecstack
                     # 16 KB-aligned segments for Android 15+ devices with 16 KB
                     # pages (the NDK r28 default; stated so a downgrade of the
                     # NDK cannot silently regress it).
                     -Wl,-z,max-page-size=16384 )