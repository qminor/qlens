# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.21

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/local/Cellar/cmake/3.21.2/bin/cmake

# The command to remove a file.
RM = /usr/local/Cellar/cmake/3.21.2/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /Users/qminor/git_testdir/qlens-dev/qlens-beta

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /Users/qminor/git_testdir/qlens-dev/qlens-beta/build

# Utility rule file for multinest-distclean.

# Include any custom commands dependencies for this target.
include CMakeFiles/multinest-distclean.dir/compiler_depend.make

# Include the progress variables for this target.
include CMakeFiles/multinest-distclean.dir/progress.make

CMakeFiles/multinest-distclean:
	cd /Users/qminor/git_testdir/qlens-dev/qlens-beta && /usr/local/Cellar/cmake/3.21.2/bin/cmake -E remove_directory contrib/MultiNest
	cd /Users/qminor/git_testdir/qlens-dev/qlens-beta && /usr/local/Cellar/cmake/3.21.2/bin/cmake -E remove_directory /Users/qminor/git_testdir/qlens-dev/qlens-beta/build/multinest-prefix

multinest-distclean: CMakeFiles/multinest-distclean
multinest-distclean: CMakeFiles/multinest-distclean.dir/build.make
.PHONY : multinest-distclean

# Rule to build all files generated by this target.
CMakeFiles/multinest-distclean.dir/build: multinest-distclean
.PHONY : CMakeFiles/multinest-distclean.dir/build

CMakeFiles/multinest-distclean.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/multinest-distclean.dir/cmake_clean.cmake
.PHONY : CMakeFiles/multinest-distclean.dir/clean

CMakeFiles/multinest-distclean.dir/depend:
	cd /Users/qminor/git_testdir/qlens-dev/qlens-beta/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /Users/qminor/git_testdir/qlens-dev/qlens-beta /Users/qminor/git_testdir/qlens-dev/qlens-beta /Users/qminor/git_testdir/qlens-dev/qlens-beta/build /Users/qminor/git_testdir/qlens-dev/qlens-beta/build /Users/qminor/git_testdir/qlens-dev/qlens-beta/build/CMakeFiles/multinest-distclean.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/multinest-distclean.dir/depend

