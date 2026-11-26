# -----------------------------------------------------------------------------
# micro-os-plus-iii-riscv -- the RISC-V port of µOS++ III.
#
# The fourth architecture project, beside -cortexm, -aarch32 and -aarch64,
# and built the same way. It consumes the kernel; the kernel does not know
# this project exists, so the dependency runs one way.
#
# It holds no copy of the kernel. Clone it beside this one:
#
#   git clone .../micro-os-plus-iii.git
#
# and it is found automatically (also as micro-os-plus-iii.git, -smp, -smp.git).
#
# WHAT THIS PORT IS. RV64GC, machine mode, one hart. src/rtos/os-core.cpp is
# the upstream Cortex-M port's shape translated to RISC-V: the machine
# software interrupt (CLINT MSIP) stands in for PendSV, the machine timer for
# SysTick, mstatus.MIE for PRIMASK. OS_NCPU=1 and the kernel's SMP branches
# compile out, exactly as on the single-core Cortex-M boards.
#
# Boards are build configurations of this one port: -DBOARD=milkv-duo
# (default). A board is a directory test/boards/<id>/ with a board.cmake.
#
# Exports:
#   micro-os-plus::riscv            port + kernel + the selected board
#   micro-os-plus::riscv-bare       the board + start-up, trap entry and a
#                                   kernel-free runtime (no kernel)
#   micro-os-plus::riscv-qemu-virt  port + kernel + QEMU's virt machine, the
#                                   stand-in for a board QEMU does not model
# -----------------------------------------------------------------------------
cmake_minimum_required (VERSION 3.20)

project (micro-os-plus-iii-riscv LANGUAGES C CXX ASM)

set (UOS_RISCV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# -----------------------------------------------------------------------------
# Under the kernel's tests/ (micro-os-plus-iii-smp2): a leaf library.
#
# Like every µOS++ port there, this port then adds no kernel and reads no
# board. The consumer (the tests, an application) adds the kernel; the board
# -- its facts, flags, start-up support and linker scripts -- is the
# consumer's too: the test platform tests/platforms/riscv-<board>/ builds a
# `micro-os-plus::board` target and links it with this one. The targets below
# name kernel targets, which CMake resolves when it generates, so the kernel
# may be added after the port.
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
#
# Built as the top-level project, the port keeps its standalone layout
# (below): it adds the kernel beside it, reads test/boards/<BOARD>/ and builds
# test/<BOARD>/. The boards that have moved to the kernel's tests/platforms/
# are built from there.
if (NOT PROJECT_IS_TOP_LEVEL)
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
  return ()
endif ()

# -----------------------------------------------------------------------------
# Standalone (top-level) build.
#
# The kernel: a sibling clone.
get_filename_component (_uos_siblings "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set (UOS_SMP_DIR "${_uos_siblings}/micro-os-plus-iii"
     CACHE PATH "µOS++ III kernel working copy")

if (NOT TARGET micro-os-plus::iii)
  foreach (_cand IN ITEMS "${UOS_SMP_DIR}" "${UOS_SMP_DIR}.git"
                          "${_uos_siblings}/micro-os-plus-iii-smp"
                          "${_uos_siblings}/micro-os-plus-iii-smp.git")
    if (NOT EXISTS "${UOS_SMP_DIR}/CMakeLists.txt"
        AND EXISTS "${_cand}/CMakeLists.txt")
      set (UOS_SMP_DIR "${_cand}" CACHE PATH "µOS++ III kernel working copy" FORCE)
    endif ()
  endforeach ()

  if (NOT EXISTS "${UOS_SMP_DIR}/CMakeLists.txt")
    message (FATAL_ERROR
      "Cannot find the kernel repository at ${UOS_SMP_DIR}.\n"
      "This project keeps no copy of it. Clone it beside this one:\n"
      "    git clone <remote>/micro-os-plus-iii.git\n"
      "or point at a working copy: cmake -DUOS_SMP_DIR=/path/to/it ...")
  endif ()

  add_subdirectory ("${UOS_SMP_DIR}" micro-os-plus-iii-smp)
endif ()

# -----------------------------------------------------------------------------
# Which board. Boards are DISCOVERED: every test/boards/<id>/board.cmake.
file (GLOB _board_decls CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/test/boards/*/board.cmake")
set (_board_ids "")
foreach (_decl IN LISTS _board_decls)
  get_filename_component (_dir "${_decl}" DIRECTORY)
  get_filename_component (_id "${_dir}" NAME)
  list (APPEND _board_ids "${_id}")
endforeach ()
list (SORT _board_ids)

set (BOARD "milkv-duo" CACHE STRING "Target board (one of: ${_board_ids})")
set_property (CACHE BOARD PROPERTY STRINGS ${_board_ids})

if (NOT BOARD IN_LIST _board_ids)
  message (FATAL_ERROR
    "BOARD=${BOARD} is not a board of this port.\n"
    "Available: ${_board_ids}\n"
    "A board is a directory under test/boards/ containing a board.cmake.")
endif ()

include ("${CMAKE_CURRENT_SOURCE_DIR}/test/boards/${BOARD}/board.cmake")

foreach (_required IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_FLAGS UOS_BOARD_NCPU
                            UOS_BOARD_CAPS UOS_BOARD_LINKER_HW
                            UOS_BOARD_DEFINES)
  if (NOT DEFINED ${_required})
    message (FATAL_ERROR
      "test/boards/${BOARD}/board.cmake does not set ${_required}.")
  endif ()
endforeach ()

get_filename_component (UOS_BOARD_SRC_DIR "${UOS_BOARD_SRC_DIR}" ABSOLUTE)
foreach (_v IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_NCPU UOS_BOARD_CAPS
                     UOS_BOARD_LINKER_HW UOS_BOARD_LINKER_QEMU)
  set (${_v} "${${_v}}" CACHE INTERNAL "board fact")
endforeach ()

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

# One port target = the ISA half + one board (its include/ and src/).
#
#   uos_riscv_add_port (<name> <board dir> FLAGS ... DEFINES ... [LINKER x])
#
# defines <name>-board-interface (the board alone) and <name>-interface (that
# plus the port and the kernel). LINKER, when given, is added as -T: the QEMU
# stand-in carries its own script; a real board's script is chosen per
# variant by test/CMakeLists.txt instead.
function (uos_riscv_add_port _name _board_dir)
  cmake_parse_arguments (P "" "LINKER" "FLAGS;LINK_FLAGS;DEFINES;LIBS" ${ARGN})

  file (GLOB _bsrc CONFIGURE_DEPENDS
        "${_board_dir}/src/*.c" "${_board_dir}/src/*.cpp" "${_board_dir}/src/*.S")

  add_library (${_name}-board-interface INTERFACE)
  target_include_directories (${_name}-board-interface INTERFACE
                              "${_board_dir}/include"
                              "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_sources (${_name}-board-interface INTERFACE ${_bsrc})
  # No __ARM_EABI__: the kernel recognises bare-metal RISC-V from the
  # compiler's own macros (__riscv without __linux__). This needs a kernel
  # with that check.
  target_compile_definitions (${_name}-board-interface INTERFACE ${P_DEFINES})
  target_compile_options (${_name}-board-interface INTERFACE ${P_FLAGS})
  target_link_options (${_name}-board-interface INTERFACE ${P_FLAGS}
    # One RAM region holds text and data (the image is loaded into DDR and
    # runs there), so the single LOAD segment is RWX by construction.
    "LINKER:--no-warn-rwx-segments")
  # Link-only flags, after FLAGS: the last -march chooses the multilib
  # (board.cmake, UOS_BOARD_LINK_FLAGS).
  if (P_LINK_FLAGS)
    target_link_options (${_name}-board-interface INTERFACE ${P_LINK_FLAGS})
  endif ()
  if (P_LINKER)
    target_link_options (${_name}-board-interface INTERFACE "-T${P_LINKER}")
  endif ()
  if (P_LIBS)
    target_link_libraries (${_name}-board-interface INTERFACE ${P_LIBS})
  endif ()

  # Without the kernel (BOARD_TEST_NO_KERNEL): the board, the start-up and
  # trap entry, and a kernel-free runtime (src/bare/runtime.cpp); the test
  # support headers (hw_result.hpp) but not its board contract, which is
  # about OS_NCPU and the scheduler.
  add_library (${_name}-bare-interface INTERFACE)
  target_sources (${_name}-bare-interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/startup.S"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/trap.S"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bare/runtime.cpp")
  target_link_libraries (${_name}-bare-interface INTERFACE
    ${_name}-board-interface)
  target_include_directories (${_name}-bare-interface INTERFACE
    "$<TARGET_PROPERTY:micro-os-plus-iii-test-support-interface,INTERFACE_INCLUDE_DIRECTORIES>")

  add_library (${_name}-interface INTERFACE)
  target_sources (${_name}-interface INTERFACE ${_uos_riscv_port_sources})
  target_link_libraries (${_name}-interface INTERFACE
    ${_name}-board-interface
    micro-os-plus::iii
    # hw_result.hpp + the compile-time board contract (board-contract.cpp).
    micro-os-plus::test-support)
endfunction ()

# The selected board.
uos_riscv_add_port (micro-os-plus-iii-riscv "${UOS_BOARD_SRC_DIR}"
                    FLAGS ${UOS_BOARD_FLAGS}
                    LINK_FLAGS ${UOS_BOARD_LINK_FLAGS}
                    DEFINES ${UOS_BOARD_DEFINES}
                    LIBS ${UOS_BOARD_LIBS})
add_library (micro-os-plus::riscv ALIAS micro-os-plus-iii-riscv-interface)
add_library (micro-os-plus::riscv-bare ALIAS micro-os-plus-iii-riscv-bare-interface)

# The QEMU virt stand-in: generic RV64GC, no tuning for a particular core.
uos_riscv_add_port (micro-os-plus-iii-riscv-qemu-virt
                    "${CMAKE_CURRENT_SOURCE_DIR}/qemu/virt"
                    FLAGS -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -mcmodel=medany
                    DEFINES SEMIHOST_TRAP_CHOSEN
                            "PORT_GREETING=\"µOS++ QEMU virt (RV64GC)\""
                    LINKER "${CMAKE_CURRENT_SOURCE_DIR}/qemu/virt/linker.ld")
add_library (micro-os-plus::riscv-qemu-virt
             ALIAS micro-os-plus-iii-riscv-qemu-virt-interface)

# The same virt machine as an RV32 hart without an FPU, compiled exactly as
# the RP2350's Hazard3 cores are (the pico-sdk's -march): the stand-in for
# RV32 boards, and the proof that the port is XLEN-generic.
set (UOS_RISCV_RV32_FLAGS
     -march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp -mabi=ilp32
     -mcmodel=medany
     CACHE INTERNAL "RV32 (Hazard3) code generation")
uos_riscv_add_port (micro-os-plus-iii-riscv-qemu-virt32
                    "${CMAKE_CURRENT_SOURCE_DIR}/qemu/virt"
                    FLAGS ${UOS_RISCV_RV32_FLAGS}
                    # The libraries with M, as the Pico 2 board links them
                    # (test/boards/pico2/board.cmake).
                    LINK_FLAGS -march=rv32imc_zicsr_zifencei
                    DEFINES SEMIHOST_TRAP_CHOSEN
                            "PORT_GREETING=\"µOS++ QEMU virt (RV32IMAC, Hazard3 code)\""
                    LINKER "${CMAKE_CURRENT_SOURCE_DIR}/qemu/virt/linker.ld")
add_library (micro-os-plus::riscv-qemu-virt32
             ALIAS micro-os-plus-iii-riscv-qemu-virt32-interface)

# Which stand-in a board's `qemu` images use. A board sets its default
# (UOS_BOARD_QEMU_LIB); this cache entry overrides it, e.g. to build the
# Duo's tests as RV32 (-DUOS_QEMU_STANDIN=micro-os-plus::riscv-qemu-virt32).
set (UOS_QEMU_STANDIN "" CACHE STRING
     "Override the board's QEMU stand-in library (empty: the board's choice)")

# Standalone: this repository is the top-level build (its own test/ builder
# registers CTest cases). Under the kernel's harness (tests/), the harness
# platform registers them instead.
set (UOS_RISCV_STANDALONE ${PROJECT_IS_TOP_LEVEL} CACHE INTERNAL "")

set (UOS_PORT_LIB      micro-os-plus::riscv      CACHE INTERNAL "")
set (UOS_PORT_BARE_LIB micro-os-plus::riscv-bare CACHE INTERNAL "")

if (PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  # The harness's naming: <app>-<variant>.elf, plus a <app>-hwd symlink to it
  # for the hardware runner (test/CMakeLists.txt).
  set (CMAKE_EXECUTABLE_SUFFIX ".elf")
  enable_testing ()
  add_subdirectory (test)
endif ()

message (STATUS "µOS++ III RISC-V port: micro-os-plus::riscv, board ${BOARD}")
