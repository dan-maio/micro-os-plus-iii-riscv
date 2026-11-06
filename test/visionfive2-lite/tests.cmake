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
# These tests run the kernel's SMP scheduler on all four; every other test
# is built for one CPU (CPU 0 = hart 1), exactly as before -- the same rule
# as the Pico 2's (test/pico2/tests.cmake).
set (_vf2_smp_tests smp-test2 smp-mat-test)

function (board_test_ncpu _app _out)
  if ("${_app}" IN_LIST _vf2_smp_tests)
    set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
  else ()
    set (${_out} 1 PARENT_SCOPE)
  endif ()
endfunction ()
