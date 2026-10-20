Stress test for the z_try_abort_timeout() -EAGAIN retry loops (SMP only).

8 threads wait in k_poll() on a signal with a 1-tick timeout; one thread pinned to
the last CPU raises the signals in a loop. Prints the longest k_poll_signal_raise()
and the longest gap of a 1 ms k_timer, counting a stall when either exceeds 20 ms.

  west build -b qemu_x86_64 tests/kernel/timer/timeout_livelock -- -DTL_SECS=120
  west build -t run      # prints "RESULT: stalls(raise/timer)=... worst_raise=... worst_gap=..."
