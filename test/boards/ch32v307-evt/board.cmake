# -----------------------------------------------------------------------------
# board.cmake -- WCH CH32V307V-EVT-R1 (CH32V307VCT6, one QingKe V4F core).
#
# RV32IMAFC, single-precision FPU (misa 0x40901125, measured), ilp32f.
# Code runs from user flash (linker.ld, 256 KiB code + 64 KiB SRAM as configured).
# Clocks 144 MHz from the 8 MHz HSE; console USART1 PA9/PA10 115200 through
# the on-board WCH-LinkE's CDC port; LED1 on PD0 (jumper, active low); key S2
# on PA1 (active low). hw.sh drives the on-board WCH-LinkE with the WCH
# (MounRiver) OpenOCD.
#
# No CLINT and no machine timer/software interrupt: interrupts go through
# the PFIC, and the port runs its PFIC path (RISCV_PFIC). No QEMU image:
# QEMU does not model the CH32V307 or the PFIC.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")

set (UOS_BOARD_FLAGS
     -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany)

set (UOS_BOARD_NCPU 1)

set (UOS_BOARD_CAPS led uart key semihosting)

set (UOS_BOARD_LINKER_HW "${UOS_BOARD_SRC_DIR}/linker.ld")

set (UOS_BOARD_LIBS "")

# RISCV_PFIC: the port's QingKe PFIC path (include/pfic.hpp): the software
# interrupt as PendSV, SysTick as the tick, the PFIC threshold as BASEPRI,
# nested interrupts.
set (UOS_BOARD_DEFINES
     RISCV_PFIC
     SEMIHOST_TRAP_CHOSEN
     "PORT_GREETING=\"µOS++ WCH CH32V307V-EVT (QingKe V4F RV32)\"")
