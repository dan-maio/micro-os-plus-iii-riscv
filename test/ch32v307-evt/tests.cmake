# -----------------------------------------------------------------------------
# What the CH32V307V-EVT's tests need beyond the defaults.
#
# The kernel runs on the port's PFIC path (board.cmake RISCV_PFIC). Images
# run from flash (board linker.ld); there is no QEMU image.
#
#   board-basics   kernel-free (micro-os-plus::riscv-bare): start-up,
#                  clocks, console, LED, key, semihosting
#   smp-test1      the kernel, single core (the Pico 2's bring-up test)
# -----------------------------------------------------------------------------

set (BOARD_TEST_NO_KERNEL board-basics)
