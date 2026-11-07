# -----------------------------------------------------------------------------
# What the VisionFive 2 Lite's tests need beyond the defaults.
#
# Every test is a board test (own os_main(), links micro-os-plus::riscv, ends
# with a RESULT line and a semihosting exit), built only as a hardware image
# (board.cmake sets no QEMU linker) that runs entirely in DDR.
#
# The tests are the portable ones of the Milk-V Duo, adapted where the board
# differs: the clock is clint::now () (the CLINT's mtime -- the U74 has no
# time CSR), and mmu-test uses this board's map and skips the misaligned
# accesses the U74 traps on.
# -----------------------------------------------------------------------------

# SMP: the board has four U74 harts (UOS_BOARD_NCPU 4, CPU n = mhartid n+1).
# EVERY test runs the kernel's SMP scheduler on all four.
#
#   - smp-test2 and smp-mat-test start CPUs 1..3 themselves (their own
#     idle threads and multicore::launch_core calls, as on the Pico 2);
#   - every other test gets src/smp-autostart.cpp: its os_main() is renamed
#     vf2_test_os_main() (-D on the test's own sources only, so the kernel's
#     main() still calls the board's os_main()), and smp-autostart's
#     os_main() starts CPUs 1..3, checks that each runs on its own hart, and
#     then calls the test. A core that does not start fails the test.
set (_vf2_self_launching_tests smp-test2 smp-mat-test)

# -----------------------------------------------------------------------------
# The harness suites (the kernel's tests/sources/<suite>), built only under
# the kernel's harness (platform riscv-visionfive2-lite), as on the Milk-V
# Duo: the directory under test/visionfive2-lite/ carries only the name and
# the verdict (harness-suite.cpp). Unlike the Duo and the Pico 2 (one core),
# every suite here runs on all four U74: its __wrap_os_main()
# (include/harness-suite.hpp) starts CPUs 1..3 with vf2_smp_start() before
# the suite.
set (BOARD_TEST_HARNESS_SUITES
     blinky fp-switch mutex-stress instrumentation rtos-apis cmsis-os-validator)

if (DEFINED PLATFORM_NAME AND NOT TARGET visionfive2-lite-harness-suite)
  # What a harness platform gives a suite and a board app does not:
  #   OS_USE_OS_APP_CONFIG_H  the kernel reads the suite's os-app-config.h
  #                           only under it;
  #   the platform include    that file includes cmsis-plus/platform.h,
  #                           which is the harness platform's;
  #   --wrap=os_main          harness-suite.cpp defines __wrap_os_main().
  add_library (visionfive2-lite-harness-suite INTERFACE)
  target_compile_definitions (visionfive2-lite-harness-suite INTERFACE
                              OS_USE_OS_APP_CONFIG_H UOS_HARNESS_SUITE
                              _POSIX_C_SOURCE=200809L _GNU_SOURCE)
  target_include_directories (visionfive2-lite-harness-suite INTERFACE
                              "${CMAKE_SOURCE_DIR}/platforms/${PLATFORM_NAME}/include")
  target_link_options (visionfive2-lite-harness-suite INTERFACE "-Wl,--wrap=os_main")
endif ()

function (board_test_libs _app _out)
  if ("${_app}" IN_LIST BOARD_TEST_HARNESS_SUITES)
    set (_l test::${_app} visionfive2-lite-harness-suite)
    if (_app STREQUAL "rtos-apis")
      # Its POSIX I/O and FatFs tests run on the kernel's POSIX layer.
      list (APPEND _l xpacks::chan-fatfs micro-os-plus::iii-posix-io)
    elseif (_app STREQUAL "cmsis-os-validator")
      list (APPEND _l xpacks::arm-cmsis-os-validator)
    endif ()
    set (${_out} ${_l} PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()

# Neither the self-launching tests nor the suites get the auto-start rename.
set (_vf2_no_autostart ${_vf2_self_launching_tests} ${BOARD_TEST_HARNESS_SUITES})

function (board_test_ncpu _app _out)
  set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
endfunction ()

function (board_test_defines _app _out)
  if ("${_app}" IN_LIST _vf2_no_autostart)
    set (${_out} "" PARENT_SCOPE)
  else ()
    set (${_out} VF2_SMP_AUTOSTART=1 PARENT_SCOPE)
  endif ()
endfunction ()

function (board_test_options _app _out)
  if ("${_app}" IN_LIST _vf2_no_autostart)
    set (${_out} "" PARENT_SCOPE)
  else ()
    set (${_out} -Dos_main=vf2_test_os_main PARENT_SCOPE)
  endif ()
endfunction ()
