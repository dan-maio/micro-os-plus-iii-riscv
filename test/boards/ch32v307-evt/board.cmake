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
# the PFIC. Until the port has its PFIC path, the board builds only the
# kernel-free tests (riscv-board.hpp). No QEMU image: QEMU does not model
# the CH32V307 or the PFIC.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")

set (UOS_BOARD_FLAGS
     -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany)

set (UOS_BOARD_NCPU 1)

set (UOS_BOARD_CAPS led uart key semihosting)

set (UOS_BOARD_LINKER_HW "${UOS_BOARD_SRC_DIR}/linker.ld")

set (UOS_BOARD_LIBS "")

set (UOS_BOARD_DEFINES
     SEMIHOST_TRAP_CHOSEN
     "PORT_GREETING=\"µOS++ WCH CH32V307V-EVT (QingKe V4F RV32)\"")
