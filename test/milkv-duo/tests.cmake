# -----------------------------------------------------------------------------
# What the Milk-V Duo's tests need beyond the defaults.
#
# Every test is a board test: it has its own os_main(), links
# micro-os-plus::riscv and ends with a RESULT line and a semihosting exit.
#
# The `qemu` image. QEMU does not model the CV1800B, so the emulated image of
# each test links the port against QEMU's virt machine instead of this board
# (micro-os-plus::riscv-qemu-virt: the same port core, a virt CLINT, an
# NS16550 console, a virtual LED, its own linker script). The test's sources
# are unchanged; only what runs under them differs -- exactly how the
# Cortex-M port's pico2 runs its tests on QEMU.
# -----------------------------------------------------------------------------

function (board_test_qemu_libs _app _out)
  set (${_out} micro-os-plus::riscv-qemu-virt PARENT_SCOPE)
endfunction ()
