# -----------------------------------------------------------------------------
# StarFive VisionFive 2 Lite -- StarFive JH-7110S, four SiFive U74 harts
# (RV64GC + Zba/Zbb, Sv39; mhartid 1..4) and one S7 monitor hart (mhartid 0,
# rv64imac, not used). µOS++ runs on the U74 harts in machine mode (SMP on
# all four, or on hart 1 alone), entirely in DDR.
#
# Sources (jh7110.dtsi / jh7110-common.dtsi, Linux):
#   cpu@1..4  riscv,isa "rv64imafdc_zba_zbb", mmu-type "riscv,sv39"
#   timebase-frequency = <4000000>
#
# One file per board, discovered by the presence of this file.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")

# medany: the image lives at 0x40200000, outside medlow's +-2 GiB of 0 range
# for the MMIO it addresses as constants (harmless either way, as on the Duo).
# Zba/Zbb change nothing in the multilib selection (the same
# rv64imafdc_zicsr_zaamo_zalrsc/lp64d as the Duo; -print-multi-directory).
set (UOS_BOARD_FLAGS -march=rv64imafdc_zicsr_zifencei_zba_zbb -mabi=lp64d
                     -mcmodel=medany -mtune=sifive-7-series)
# The four U74 harts (mhartid 1..4) are µOS++ CPUs 0..3: RISCV_HART_BASE=1
# below (include/port_ctx.hpp). Which tests run SMP is test/visionfive2-lite/
# tests.cmake's choice; the others are built for one CPU.
set (UOS_BOARD_NCPU  4)

set (UOS_BOARD_CAPS  led uart semihosting mmu fpu)

# The hardware image: loaded into DDR at 0x40200000 by OpenOCD (hw.sh), with
# the board stopped at the U-Boot prompt (DDR trained, UART0 configured).
set (UOS_BOARD_LINKER_HW "${UOS_BOARD_SRC_DIR}/linker.ld")

# No QEMU image: hardware only (UOS_BOARD_LINKER_QEMU unset).

set (UOS_BOARD_LIBS "")

set (UOS_BOARD_DEFINES
     SEMIHOST_TRAP_CHOSEN
     RISCV_HART_BASE=1
     "PORT_GREETING=\"µOS++ StarFive VisionFive 2 Lite (JH-7110S, SiFive U74 RV64GC)\"")
