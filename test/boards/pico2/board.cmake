# -----------------------------------------------------------------------------
# board.cmake -- Raspberry Pi Pico 2 (RP2350) on its two Hazard3 cores.
#
# RV32IMAC + Zba/Zbb/Zbs/Zbkb/Zcb/Zcmp, no FPU, compiled as the pico-sdk
# compiles for Hazard3. µOS++ runs SMP on both harts (OS_NCPU=2).
#
# `hwd` images run from flash (linker.ld): the boot ROM sees the RISC-V
# IMAGE_DEF block (src/image-def.S), switches both cores to Hazard3 and
# enters _start on core 0. Clocks 150 MHz, mtime 1 MHz from the TICKS block,
# UART0 on GPIO0/1, LED GPIO25, core 1 launched through the boot ROM FIFO.
# hw.sh drives the Raspberry Pi Debug Probe with the Raspberry Pi OpenOCD.
# `qemu` images run on QEMU virt, RV32, two harts (the RP2350 is not
# modelled).
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")

set (UOS_BOARD_FLAGS
     -march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp -mabi=ilp32
     -mcmodel=medany)
set (UOS_BOARD_NCPU 2)

set (UOS_BOARD_CAPS smp led uart semihosting)

set (UOS_BOARD_LINKER_HW "${UOS_BOARD_SRC_DIR}/linker.ld")
set (UOS_BOARD_LINKER_QEMU "${UOS_RISCV_ROOT}/qemu/virt/linker.ld")

set (UOS_BOARD_LIBS "")

set (UOS_BOARD_DEFINES
     SEMIHOST_TRAP_CHOSEN
     "PORT_GREETING=\"µOS++ Raspberry Pi Pico 2 (RP2350, 2x Hazard3 RV32)\"")
