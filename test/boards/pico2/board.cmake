# -----------------------------------------------------------------------------
# board.cmake -- Raspberry Pi Pico 2 (RP2350) on its two Hazard3 cores.
#
# RV32IMAC + Zba/Zbb/Zbs/Zbkb/Zcb/Zcmp, no FPU, compiled as the pico-sdk
# compiles for Hazard3. µOS++ runs SMP on both harts (OS_NCPU=2).
#
# Bring-up status: EMULATED ONLY. The `qemu` images run on QEMU virt, RV32,
# two harts (the RP2350 is not modelled). The hardware side (RP2350 SIO timer
# and soft IRQ, Xh3irq, clocks, UART0, LED, boot-ROM core-1 launch, linker
# scripts, hw.sh on the Raspberry Pi Debug Probe) comes next; until then
# UOS_BOARD_LINKER_HW is empty and no `hwd` image is built.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")

set (UOS_BOARD_FLAGS
     -march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp -mabi=ilp32
     -mcmodel=medany)
set (UOS_BOARD_NCPU 2)

set (UOS_BOARD_CAPS smp led uart semihosting)

set (UOS_BOARD_LINKER_HW "")
set (UOS_BOARD_LINKER_QEMU "${UOS_RISCV_ROOT}/qemu/virt/linker.ld")

set (UOS_BOARD_LIBS "")

set (UOS_BOARD_DEFINES
     SEMIHOST_TRAP_CHOSEN
     "PORT_GREETING=\"µOS++ Raspberry Pi Pico 2 (RP2350, 2x Hazard3 RV32)\"")
