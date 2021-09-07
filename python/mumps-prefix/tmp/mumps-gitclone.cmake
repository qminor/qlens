
if(NOT "/Users/qminor/git_testdir/qlens-dev/qlens-beta/python/mumps-prefix/src/mumps-stamp/mumps-gitinfo.txt" IS_NEWER_THAN "/Users/qminor/git_testdir/qlens-dev/qlens-beta/python/mumps-prefix/src/mumps-stamp/mumps-gitclone-lastrun.txt")
  message(STATUS "Avoiding repeated git clone, stamp file is up to date: '/Users/qminor/git_testdir/qlens-dev/qlens-beta/python/mumps-prefix/src/mumps-stamp/mumps-gitclone-lastrun.txt'")
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "/Users/qminor/git_testdir/qlens-dev/qlens-beta/contrib/mumps"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: '/Users/qminor/git_testdir/qlens-dev/qlens-beta/contrib/mumps'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "/usr/bin/git"  clone --no-checkout --progress --config "advice.detachedHead=false" "https://github.com/scivision/mumps.git" "mumps"
    WORKING_DIRECTORY "/Users/qminor/git_testdir/qlens-dev/qlens-beta/contrib"
    RESULT_VARIABLE error_code
    )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(STATUS "Had to git clone more than once:
          ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/scivision/mumps.git'")
endif()

execute_process(
  COMMAND "/usr/bin/git"  checkout v5.4.0.0 --
  WORKING_DIRECTORY "/Users/qminor/git_testdir/qlens-dev/qlens-beta/contrib/mumps"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: 'v5.4.0.0'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "/usr/bin/git"  submodule update --recursive --init 
    WORKING_DIRECTORY "/Users/qminor/git_testdir/qlens-dev/qlens-beta/contrib/mumps"
    RESULT_VARIABLE error_code
    )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: '/Users/qminor/git_testdir/qlens-dev/qlens-beta/contrib/mumps'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy
    "/Users/qminor/git_testdir/qlens-dev/qlens-beta/python/mumps-prefix/src/mumps-stamp/mumps-gitinfo.txt"
    "/Users/qminor/git_testdir/qlens-dev/qlens-beta/python/mumps-prefix/src/mumps-stamp/mumps-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: '/Users/qminor/git_testdir/qlens-dev/qlens-beta/python/mumps-prefix/src/mumps-stamp/mumps-gitclone-lastrun.txt'")
endif()

