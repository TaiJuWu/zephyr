Stress test for the z_try_abort_timeout() -EAGAIN retry loops (SMP only).

8 threads wait in k_poll() on a signal with a 1-tick timeout; one thread pinned to
the last CPU raises the signals in a loop. Prints the longest k_poll_signal_raise()
and the longest gap of a 1 ms k_timer, counting a stall when either exceeds 20 ms.

  west build -b qemu_x86_64 tests/kernel/timer/timeout_livelock -- -DTL_SECS=120
  west build -t run      # prints "RESULT: stalls(raise/timer)=... worst_raise=... worst_gap=..."

QEMU icount must stay off (it is off on qemu_x86_64 by default): with icount the vCPUs run one
after the other and the race cannot happen.

Counting the retries directly needs the debug-only commit on this branch that
instruments z_unpend_thread() ("DNM: kernel: sched: count ..."):

  west build -b qemu_x86_64 tests/kernel/timer/timeout_livelock -- -DTL_SECS=600 \
      -DTL_INSTR=1 -DEXTRA_CONF_FILE=tsc.conf
  # prints "RETRY_RESULT: calls=... iters=... max_iters=... max_loop_us=..."

tsc.conf uses the TSC deadline timer instead of HPET, for KVM: there every HPET
read exits to QEMU, which adds tens of ms of noise. Set
CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC to the host's TSC frequency, and run QEMU with
"-enable-kvm -cpu host,+invtsc" instead of "-cpu qemu64,..." (see build.ninja for
the rest of the command line), each instance pinned to its own cores with taskset.
