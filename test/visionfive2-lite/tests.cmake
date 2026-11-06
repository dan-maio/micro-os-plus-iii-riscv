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
