# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.19

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
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/gmartine/qlens-beta

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/gmartine/qlens-beta/cmake

# Utility rule file for multinest.

# Include the progress variables for this target.
include CMakeFiles/multinest.dir/progress.make

CMakeFiles/multinest: CMakeFiles/multinest-complete


CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-install
CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-mkdir
CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-download
CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-update
CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-patch
CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-configure
CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-build
CMakeFiles/multinest-complete: multinest-prefix/src/multinest-stamp/multinest-install
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Completed 'multinest'"
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/cmake/CMakeFiles
	/usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/CMakeFiles/multinest-complete
	/usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp/multinest-done

multinest-prefix/src/multinest-stamp/multinest-install: multinest-prefix/src/multinest-stamp/multinest-build
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "No install step for 'multinest'"
	cd /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build && /usr/bin/cmake -E echo_append
	cd /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build && /usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp/multinest-install

multinest-prefix/src/multinest-stamp/multinest-mkdir:
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Creating directories for 'multinest'"
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/contrib/MultiNest
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/cmake/multinest-prefix
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/cmake/multinest-prefix/tmp
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/cmake/multinest-prefix/src
	/usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp
	/usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp/multinest-mkdir

multinest-prefix/src/multinest-stamp/multinest-download: multinest-prefix/src/multinest-stamp/multinest-gitinfo.txt
multinest-prefix/src/multinest-stamp/multinest-download: multinest-prefix/src/multinest-stamp/multinest-mkdir
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_4) "Performing download step (git clone) for 'multinest'"
	cd /home/gmartine/qlens-beta/contrib && /usr/bin/cmake -P /home/gmartine/qlens-beta/cmake/multinest-prefix/tmp/multinest-gitclone.cmake
	cd /home/gmartine/qlens-beta/contrib && /usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp/multinest-download

multinest-prefix/src/multinest-stamp/multinest-update: multinest-prefix/src/multinest-stamp/multinest-download
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_5) "Performing update step for 'multinest'"
	cd /home/gmartine/qlens-beta/contrib/MultiNest && /usr/bin/cmake -P /home/gmartine/qlens-beta/cmake/multinest-prefix/tmp/multinest-gitupdate.cmake

multinest-prefix/src/multinest-stamp/multinest-patch: multinest-prefix/src/multinest-stamp/multinest-update
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_6) "Performing patch step for 'multinest'"
	cd /home/gmartine/qlens-beta/contrib/MultiNest && /usr/bin/cmake -E make_directory /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build
	cd /home/gmartine/qlens-beta/contrib/MultiNest && /usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp/multinest-patch

multinest-prefix/src/multinest-stamp/multinest-configure: multinest-prefix/tmp/multinest-cfgcmd.txt
multinest-prefix/src/multinest-stamp/multinest-configure: multinest-prefix/src/multinest-stamp/multinest-patch
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_7) "Performing configure step for 'multinest'"
	cd /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build && /usr/bin/cmake -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_Fortran_COMPILER=/usr/bin/gfortran -DMPI_C_COMPILER=MPI_C_COMPILER-NOTFOUND -DMPI_CXX_COMPILER=MPI_CXX_COMPILER-NOTFOUND -DMPI_Fortran_COMPILER=MPI_Fortran_COMPILER-NOTFOUND -DBLA_VENDOR=All -DMPI_C_FOUND=FALSE -DMPI_C_INCLUDE_PATH= -DMPI_C_LIBRARIES= -DMPI_C_COMPILE_FLAGS= -DMPI_CXX_FOUND=FALSE -DMPI_CXX_INCLUDE_PATH= -DMPI_CXX_LIBRARIES= -DMPI_CXX_COMPILE_FLAGS= -DMPI_Fortran_FOUND=FALSE -DMPI_Fortran_INCLUDE_PATH= MPI_Fortran_LIBRARIES= -DMPI_Fortran_COMPILE_FLAGS= "-DCMAKE_Fortran_FLAGS=-fPIC -w -fallow-argument-mismatch" -Wno-dev ..
	cd /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build && /usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp/multinest-configure

multinest-prefix/src/multinest-stamp/multinest-build: multinest-prefix/src/multinest-stamp/multinest-configure
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --blue --bold --progress-dir=/home/gmartine/qlens-beta/cmake/CMakeFiles --progress-num=$(CMAKE_PROGRESS_8) "Performing build step for 'multinest'"
	cd /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build && make multinest_static
	cd /home/gmartine/qlens-beta/contrib/MultiNest/MultiNest_v3.12_CMake/multinest/build && /usr/bin/cmake -E touch /home/gmartine/qlens-beta/cmake/multinest-prefix/src/multinest-stamp/multinest-build

multinest: CMakeFiles/multinest
multinest: CMakeFiles/multinest-complete
multinest: multinest-prefix/src/multinest-stamp/multinest-build
multinest: multinest-prefix/src/multinest-stamp/multinest-configure
multinest: multinest-prefix/src/multinest-stamp/multinest-download
multinest: multinest-prefix/src/multinest-stamp/multinest-install
multinest: multinest-prefix/src/multinest-stamp/multinest-mkdir
multinest: multinest-prefix/src/multinest-stamp/multinest-patch
multinest: multinest-prefix/src/multinest-stamp/multinest-update
multinest: CMakeFiles/multinest.dir/build.make

.PHONY : multinest

# Rule to build all files generated by this target.
CMakeFiles/multinest.dir/build: multinest

.PHONY : CMakeFiles/multinest.dir/build

CMakeFiles/multinest.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/multinest.dir/cmake_clean.cmake
.PHONY : CMakeFiles/multinest.dir/clean

CMakeFiles/multinest.dir/depend:
	cd /home/gmartine/qlens-beta/cmake && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/gmartine/qlens-beta /home/gmartine/qlens-beta /home/gmartine/qlens-beta/cmake /home/gmartine/qlens-beta/cmake /home/gmartine/qlens-beta/cmake/CMakeFiles/multinest.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/multinest.dir/depend
