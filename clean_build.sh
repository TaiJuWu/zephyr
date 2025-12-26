# =========
# work
# west twister -p -b qemu_riscv32/qemu_virt_riscv32 -T tests/kernel/common --sub-test common_1cpu.timeout_order
# west build -p -b mps2/an385 -t run tests/lib/c_lib/thrd/  -T libraries.libc.c11_threads.minimal --  -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y
# west build -p -b native_sim/native/64 -t run tests/kernel/common -T kernel.common.picolibc
# west build -p -b native_sim/native/64 -t debug tests/kernel/common -- -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y
# ==========
# -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y
# -DCONFIG_ASAN=y -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_LOG_MODE_IMMEDIATE=y \
# -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y -DCONFIG_DEBUG=y \
# -DCONFIG_SMP=y -DCONFIG_MP_MAX_NUM_CPUS=4
# =========

# native_sim/native/64
# qemu_riscv32e/qemu_virt_riscv32e
# qemu_riscv32
# ==========
# useful information
# perf: https://docs.zephyrproject.org/latest/services/profiling/perf.html
# optimize tool: https://docs.zephyrproject.org/latest/develop/optimizations/tools.html

#============

# ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 twister --coverage -p qemu_x86 -T tests/kernel

###################


# rm -rf build
# west twister -p qemu_riscv32/qemu_virt_riscv32 -T tests/kernel/
# issue 95728
# Use twister to run the deadline scheduler tests
# ./scripts/twister -b -p qemu_riscv32 tests/kernel/sched -T kernel.scheduler
# west build -p -b mps2/an385 -t run tests/lib/c_lib/thrd/  -T libraries.libc.c11_threads.minimal --  -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y
# west build -p -b native_sim/native/64 -t run tests/kernel/common -T kernel.common.picolibc
# west twister -p qemu_riscv64/qemu_virt_riscv64/smp -T tests/kernel/ -- -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y

# west build -p -b qemu_riscv32e/qemu_virt_riscv32e tests/kernel/sched/preempt -t run -T kernel.scheduler.preempt \
#     --  -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y -DCONFIG_DEBUG=y \
#     -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y -DCONFIG_COVERAGE=y -DCONFIG_COVERAGE_DUMP=y
#     # -DCONFIG_SMP=y -DCONFIG_MP_MAX_NUM_CPUS=4 


west build -p -b qemu_cortex_a53/qemu_cortex_a53/smp tests/kernel/smp_metairq -t run -T kernel.smp_metairq \
    --  -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y -DCONFIG_DEBUG=y \
    -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y


# west build $1 -b mps2/an385 tests/arch/arm/arm_user_stack_test -t $2 -T arch.arm.user.stack  \
#     -- -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y -DCONFIG_EXTRA_EXCEPTION_INFO=y
# west build -p -b qemu_cortex_m0/nrf51822 tests/arch/arm/arm_user_stack_test -t run -T arch.arm.user.stack
# west build $1 -b qemu_cortex_m0/nrf51822 tests/arch/arm/arm_user_stack_test -t $2 -T arch.arm.user.stack \
#     -- -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y -DCONFIG_EXTRA_EXCEPTION_INFO=y \
#     -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y -DCONFIG_DEBUG=y \
#     -DCONFIG_THREAD_STACK_INFO=y
    
#     -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_LOG_MODE_IMMEDIATE=y \
#     -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y  
# -DCONFIG_LOG=y  # -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_LOG_DEFAULT_LEVEL=4 
#  -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y \
    
    
# -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y \
# -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y -DCONFIG_DEBUG=y \


# west build -p -b native_sim/native/64 -t debug -T tests/kernel/common \
#  --sub-test common_1cpu.timeout_order -- \
#  -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y

# west twister -p qemu_riscv32/qemu_virt_riscv32 -T tests/kernel/common --sub-test common_1cpu.timeout_order
# west twister -p qemu_riscv32/qemu_virt_riscv32 -T tests/kernel/

# west build $1 -b qemu_riscv32/qemu_virt_riscv32 tests/kernel/sched/schedule_api -t $2 
# west build -p auto -b qemu_x86 tests/lib/p4workq -t run
#     -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_LOG_MODE_IMMEDIATE=y \
#     -DCONFIG_OUTPUT_DISASSEMBLY=y -DOUTPUT_DISASSEMBLY_WITH_SOURCE=y  \
#     -DCONFIG_LOG=y  -DCONFIG_KERNEL_LOG_LEVEL_DBG=y -DCONFIG_LOG_DEFAULT_LEVEL=4 \
#     -DCONFIG_PTHREAD_LOG_LEVEL_DBG=y 


# west build -p -b qemu_riscv32/qemu_virt_riscv32 samples/synchronization -t run