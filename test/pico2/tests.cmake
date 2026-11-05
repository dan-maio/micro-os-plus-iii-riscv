# -----------------------------------------------------------------------------
# tests.cmake -- the Pico 2 (RISC-V) test set.
#
# The `qemu` images are built on the RV32 virt stand-in. The SMP tests drive
# both harts there too (qemu.sh starts QEMU with as many harts as the image
# was built for); the single-core ones run on one.
# -----------------------------------------------------------------------------

set (_pico2_smp_tests smp-test2)

function (board_test_qemu_libs _app _out)
  if (UOS_QEMU_STANDIN)
    set (${_out} ${UOS_QEMU_STANDIN} PARENT_SCOPE)
  else ()
    set (${_out} micro-os-plus::riscv-qemu-virt32 PARENT_SCOPE)
  endif ()
endfunction ()

function (board_test_ncpu _app _out)
  if ("${_app}" IN_LIST _pico2_smp_tests)
    set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
  else ()
    set (${_out} 1 PARENT_SCOPE)
  endif ()
endfunction ()

function (board_test_qemu_ncpu _app _out)
  board_test_ncpu ("${_app}" _n)
  set (${_out} "${_n}" PARENT_SCOPE)
endfunction ()
