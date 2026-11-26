# -----------------------------------------------------------------------------
# micro-os-plus-iii-riscv -- the RISC-V port of µOS++ III.
#
# The fifth architecture project, beside -cortexm, -aarch32, -aarch64 and
# -posix-arch, and built the same way: a leaf library. It consumes the
# kernel; the kernel does not know this project exists, so the dependency
# runs one way.
#
# WHAT THIS PORT IS. RV64 or RV32, with or without an FPU, machine mode, one
# hart or SMP (OS_NCPU harts). src/rtos/os-core.cpp is the upstream Cortex-M
# port's shape translated to RISC-V: a synchronous ecall switches threads,
# the machine timer is the tick, mstatus.MIE stands in for PRIMASK; on a
# QingKe core (RISCV_PFIC) the PFIC's software interrupt and SysTick do.
#
# The boards and their tests are the test platforms of the kernel's tests/
# (micro-os-plus-iii-smp2: tests/platforms/riscv-<board>/, and the QEMU virt
# platforms qemu-riscv64 and qemu-riscv32).
# -----------------------------------------------------------------------------
cmake_minimum_required (VERSION 3.20)

project (micro-os-plus-iii-riscv LANGUAGES C CXX ASM)

set (UOS_RISCV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# -----------------------------------------------------------------------------
# A leaf library: like every µOS++ port in micro-os-plus-iii-smp2, this port
# adds no kernel and reads no board. The consumer (the tests, an application)
# adds the kernel; the board -- its facts, flags, start-up support and linker
# scripts -- is the consumer's too: the test platform
# tests/platforms/riscv-<board>/ builds a `micro-os-plus::board` target and
# links it with this one. The targets below name kernel targets, which CMake
# resolves when it generates, so the kernel may be added after the port.
#
# Exports:
#   micro-os-plus::riscv            the ISA half (start-up, trap entry, the
#                                   scheduler port), linked with a board
#   micro-os-plus::riscv-bare       start-up, trap entry and a kernel-free
#                                   runtime (the tests that compile no kernel)
#   micro-os-plus::riscv-qemu-virt  micro-os-plus::riscv on QEMU's virt
#                                   machine (RV64GC): the stand-in that
#                                   replaces a board QEMU does not model
#   micro-os-plus::riscv-qemu-virt32  the same as an RV32 hart without an FPU
#   micro-os-plus::riscv-qemu-virt[32]-board  the virt board alone, without
#                                   the port or the linker script: for a
#                                   platform that is the virt machine
# -----------------------------------------------------------------------------

# The ISA half: the same on every board.
set (_uos_riscv_port_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/src/startup.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/trap.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/trap.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/startup.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/trace.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/syscalls.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/syscalls-time.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mmu.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/soft-irq.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/semihosting-exit.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rtos/os-core.cpp"
)

if (NOT TARGET micro-os-plus::riscv)
  # What every image of this port is compiled with, whatever the board.
  #
  # No __ARM_EABI__: the kernel recognises bare-metal RISC-V from the
  # compiler's own macros (__riscv without __linux__) and compiles its
  # embedded code (malloc, exit/atexit, _sbrk, the C++ runtime hooks, the
  # free-store set-up) for it.
  add_library (micro-os-plus-iii-riscv-common-interface INTERFACE)
  target_include_directories (micro-os-plus-iii-riscv-common-interface
                              INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_options (micro-os-plus-iii-riscv-common-interface INTERFACE
    # One RAM region holds text and data (the image is loaded into RAM and
    # runs there), so the single LOAD segment is RWX by construction.
    "LINKER:--no-warn-rwx-segments")

  # The port: the ISA half, on the kernel, with the test support
  # (hw_result.hpp and the compile-time board contract, board-contract.cpp).
  add_library (micro-os-plus-iii-riscv-interface INTERFACE)
  target_sources (micro-os-plus-iii-riscv-interface INTERFACE
                  ${_uos_riscv_port_sources})
  target_link_libraries (micro-os-plus-iii-riscv-interface INTERFACE
    micro-os-plus-iii-riscv-common-interface
    micro-os-plus::iii-core
    micro-os-plus::test-support)
  add_library (micro-os-plus::riscv ALIAS micro-os-plus-iii-riscv-interface)

  # Without the kernel: the start-up, the trap entry and a kernel-free
  # runtime (src/bare/runtime.cpp); the test support headers (hw_result.hpp)
  # but not its board contract, which is about OS_NCPU and the scheduler.
  add_library (micro-os-plus-iii-riscv-bare-interface INTERFACE)
  target_sources (micro-os-plus-iii-riscv-bare-interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/startup.S"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/trap.S"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bare/runtime.cpp")
  target_link_libraries (micro-os-plus-iii-riscv-bare-interface INTERFACE
    micro-os-plus-iii-riscv-common-interface)
  target_include_directories (micro-os-plus-iii-riscv-bare-interface INTERFACE
    "$<TARGET_PROPERTY:micro-os-plus-iii-test-support-interface,INTERFACE_INCLUDE_DIRECTORIES>")
  add_library (micro-os-plus::riscv-bare ALIAS micro-os-plus-iii-riscv-bare-interface)

  # QEMU's virt machine, the port's own generic target, in two forms:
  #
  #   micro-os-plus::riscv-qemu-virt[32]-board  the board alone (qemu/virt/:
  #       a virt CLINT, an NS16550 console, a virtual LED; its flags), for a
  #       platform that IS the virt machine (tests/platforms/qemu-riscv64,
  #       qemu-riscv32), which names qemu/virt/linker.ld itself;
  #   micro-os-plus::riscv-qemu-virt[32]  that board with the port and the
  #       linker script, for a platform whose silicon QEMU does not model and
  #       whose `qemu` images replace the port and the board with it.
  function (uos_riscv_add_qemu_virt _name)
    cmake_parse_arguments (V "" "" "FLAGS;LINK_FLAGS;DEFINES" ${ARGN})
    set (_virt "${CMAKE_CURRENT_SOURCE_DIR}/qemu/virt")
    file (GLOB _vsrc CONFIGURE_DEPENDS "${_virt}/src/*.cpp")
    add_library (${_name}-board-interface INTERFACE)
    target_include_directories (${_name}-board-interface INTERFACE "${_virt}/include")
    target_sources (${_name}-board-interface INTERFACE ${_vsrc})
    target_compile_definitions (${_name}-board-interface INTERFACE ${V_DEFINES})
    target_compile_options (${_name}-board-interface INTERFACE ${V_FLAGS})
    # Link-only flags after FLAGS: the last -march chooses the multilib.
    target_link_options (${_name}-board-interface INTERFACE ${V_FLAGS}
                         ${V_LINK_FLAGS})
    add_library (${_name}-interface INTERFACE)
    target_link_libraries (${_name}-interface INTERFACE
      micro-os-plus::riscv ${_name}-board-interface)
    target_link_options (${_name}-interface INTERFACE "-T${_virt}/linker.ld")
  endfunction ()

  # Generic RV64GC, no tuning for a particular core.
  uos_riscv_add_qemu_virt (micro-os-plus-iii-riscv-qemu-virt
    FLAGS -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -mcmodel=medany
    DEFINES SEMIHOST_TRAP_CHOSEN "PORT_GREETING=\"µOS++ QEMU virt (RV64GC)\"")
  add_library (micro-os-plus::riscv-qemu-virt
               ALIAS micro-os-plus-iii-riscv-qemu-virt-interface)
  add_library (micro-os-plus::riscv-qemu-virt-board
               ALIAS micro-os-plus-iii-riscv-qemu-virt-board-interface)

  # An RV32 hart without an FPU, compiled exactly as the RP2350's Hazard3
  # cores are (the pico-sdk's -march); linked with the libraries with M.
  uos_riscv_add_qemu_virt (micro-os-plus-iii-riscv-qemu-virt32
    FLAGS -march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp
          -mabi=ilp32 -mcmodel=medany
    LINK_FLAGS -march=rv32imc_zicsr_zifencei
    DEFINES SEMIHOST_TRAP_CHOSEN
            "PORT_GREETING=\"µOS++ QEMU virt (RV32IMAC, Hazard3 code)\"")
  add_library (micro-os-plus::riscv-qemu-virt32
               ALIAS micro-os-plus-iii-riscv-qemu-virt32-interface)
  add_library (micro-os-plus::riscv-qemu-virt32-board
               ALIAS micro-os-plus-iii-riscv-qemu-virt32-board-interface)

  message (STATUS "µOS++ IIIe riscv port: micro-os-plus::riscv")
endif ()
