# -----------------------------------------------------------------------------
# What the CH32V307V-EVT's tests need beyond the defaults.
#
# The board has no CLINT (riscv-board.hpp): until the port's PFIC path
# exists, every test here is kernel-free (micro-os-plus::riscv-bare). Images
# run from flash (board linker.ld); there is no QEMU image.
# -----------------------------------------------------------------------------

set (BOARD_TEST_NO_KERNEL board-basics)
