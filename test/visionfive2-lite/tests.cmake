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

function (board_test_ncpu _app _out)
  set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
endfunction ()

function (board_test_defines _app _out)
  if ("${_app}" IN_LIST _vf2_self_launching_tests)
    set (${_out} "" PARENT_SCOPE)
  else ()
    set (${_out} VF2_SMP_AUTOSTART=1 PARENT_SCOPE)
  endif ()
endfunction ()

function (board_test_options _app _out)
  if ("${_app}" IN_LIST _vf2_self_launching_tests)
    set (${_out} "" PARENT_SCOPE)
  else ()
    set (${_out} -Dos_main=vf2_test_os_main PARENT_SCOPE)
  endif ()
endfunction ()
