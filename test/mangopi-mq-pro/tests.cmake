# -----------------------------------------------------------------------------
# What the MangoPi MQ-Pro's tests need beyond the defaults.
#
# Every test is a board test: it has its own os_main(), links
# micro-os-plus::riscv and ends with a RESULT line.  On hardware that verdict
# comes back over UART0 and the test resets the SoC into FEL (test/<board>/
# include/hw_result.hpp); under QEMU it goes through semihosting.
#
# The test set is the portable RISC-V one of the Milk-V Duo, adapted for the
# D1: one C906 hart, no USB device stack, no CV1800B timers/PLIC test.  The
# `qemu` image of every test runs on QEMU's virt machine, as on the Duo.
# -----------------------------------------------------------------------------

# No JTAG probe on this setup: semihosting-test needs a debugger/emulator, so
# it is built for QEMU only and never appears as a hardware image.
set (BOARD_TEST_QEMU_ONLY semihosting-test)

# The kernels' harness suites: their sources live in the kernel's harness
# (tests/sources/<suite>), and the directory here carries only the name and
# the verdict.  Built only when the kernel harness drives the build.
set (BOARD_TEST_HARNESS_SUITES
     blinky fp-switch mutex-stress instrumentation rtos-apis cmsis-os-validator)

if (DEFINED PLATFORM_NAME AND NOT TARGET mangopi-mq-pro-harness-suite)
  add_library (mangopi-mq-pro-harness-suite INTERFACE)
  target_compile_definitions (mangopi-mq-pro-harness-suite INTERFACE
                              OS_USE_OS_APP_CONFIG_H UOS_HARNESS_SUITE
                              _POSIX_C_SOURCE=200809L _GNU_SOURCE)
  target_include_directories (mangopi-mq-pro-harness-suite INTERFACE
                              "${CMAKE_SOURCE_DIR}/platforms/${PLATFORM_NAME}/include")
  target_link_options (mangopi-mq-pro-harness-suite INTERFACE
                       "-Wl,--wrap=os_main")
endif ()

function (board_test_qemu_libs _app _out)
  if (UOS_QEMU_STANDIN)
    set (${_out} ${UOS_QEMU_STANDIN} PARENT_SCOPE)
  else ()
    set (${_out} micro-os-plus::riscv-qemu-virt PARENT_SCOPE)
  endif ()
endfunction ()

function (board_test_libs _app _out)
  if ("${_app}" IN_LIST BOARD_TEST_HARNESS_SUITES)
    set (_l test::${_app} mangopi-mq-pro-harness-suite)
    if (_app STREQUAL "rtos-apis")
      # Its POSIX I/O and FatFs tests run on the kernel's POSIX layer.
      list (APPEND _l xpacks::chan-fatfs micro-os-plus::iii-posix-io)
    elseif (_app STREQUAL "cmsis-os-validator")
      list (APPEND _l xpacks::arm-cmsis-os-validator)
    endif ()
    set (${_out} ${_l} PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()
