Scheduler torture tests
#######################

Each scenario follows a Linux scheduler stress tool and keeps its
semantics: same thread roles, same priorities relative to each other, the
same defaults where they fit, and the same pass criterion. Everything
Zephyr-specific or added is either an option that is off by default or a
separate scenario, and is listed below.

All scenarios scale with the number of CPUs, up to
``CONFIG_SCHED_TORTURE_MAX_THREADS`` threads. A progress monitor reports a
thread that makes no progress for ``CONFIG_SCHED_TORTURE_STALL_MS``, like
the stall warnings of rcutorture.

.. list-table::
   :header-rows: 1

   * - Scenario
     - Linux original
     - Zephyr implementation
     - Deviations
   * - 1 hackbench
     - ``hackbench -g 10 -f 20 -l 100 -s 100``: groups of senders and
       receivers, every sender writes every receiver; prints the time
     - message queues stand in for pipes; all threads at one priority
     - 2 senders and receivers per group by default (RAM); receivers check
       the sender, order and data of every message, which hackbench does
       not do
   * - 2 schbench
     - ``schbench -m 1 -t <ncpus>``: message threads wake their workers,
       which record wakeup latency, run for ``-c`` and sleep; p50 to p99.9
     - semaphores for futexes; latency split by same or other CPU
     - CPU hogs only with ``CONFIG_SCHED_TORTURE_SCHBENCH_HOGS``
   * - 3 cyclictest
     - ``cyclictest -S -i 1000 -d 500``: one pinned thread per CPU at the
       highest priority, absolute sleeps; Min, Avg, Max
     - highest preemptible priority; absolute tick timeouts
     - no load by default, as in cyclictest;
       ``CONFIG_SCHED_TORTURE_CYCLICTEST_LOAD`` adds message churn and hogs;
       the interval is rounded to whole ticks
   * - 4 locktorture
     - ``locktorture torture_type=spin_lock_irq,mutex_lock``: 2 * ncpus
       writers, short holds with a rare ``long_hold``, stutter, shuffle;
       fails when two writers hold the lock
     - ``k_spinlock``, ``k_mutex`` and a binary ``k_sem``, run one after
       the other
     - ``k_spinlock`` always masks interrupts, so only the ``_irq`` variant
       exists; shuffling suspends a writer to change its CPU mask, because
       Zephyr cannot change the mask of a runnable thread; no reader locks
   * - 5 pi_stress
     - ``pi_stress``: groups of low, medium and high threads; low holds the
       mutex, high blocks on it, medium spins until high is done; a
       deadlock means priority inheritance failed
     - ``k_mutex`` priority inheritance; ncpus - 1 groups
     - ``CONFIG_SCHED_TORTURE_PI_UNIPROCESSOR`` is ``--uniprocessor``
   * - 6 fork
     - ``stress-ng --fork``: create children that exit at once, reap them
     - ``k_thread_create`` and ``k_thread_join``; thread list checked for
       leaks afterwards
     - none
   * - 7 kill
     - ``stress-ng --kill``: start a child and kill it
     - ``k_thread_abort`` of a running, possibly remote, or sleeping child
     - none
   * - 8 affinity
     - ``stress-ng --affinity``: keep changing CPU affinity; a thread must
       only run on allowed CPUs
     - needs ``CONFIG_SCHED_CPU_MASK`` on SMP
     - a controller suspends the thread to change its mask (see 4)
   * - 9 rr fairness
     - ``SCHED_RR``: equal-priority threads share the CPU; a preempted
       thread keeps the rest of its slice
     - two busy threads at one priority on CPU 0 and a 1 ms ticker above
       them; fails if one gets less than a fifth
     - none; Zephyr restarts the slice of every thread it switches to
       (``z_time_slice_reset()`` in ``z_swap``), so a frequently preempted
       thread may keep the CPU from its peer
   * - 10 mixed ops
     - none
     - random mix of mutexes, semaphores, a spinlock shared with a timer
       ISR, yields, sleeps and priority changes
     - Zephyr-specific scenario
   * - 11 condvar
     - none
     - producers and consumers on a bounded ring under a ``k_mutex``, timed
       waits on two ``k_condvar``, signal or broadcast at random; no
       overflow, no loss, per-producer order kept
     - Zephyr-specific scenario
   * - 12 fifo and lifo
     - none
     - a fixed set of items moves between a free ``k_fifo`` and a work
       ``k_fifo`` and ``k_lifo``, by threads with timeouts and by a timer
       ISR, with ``k_fifo_cancel_wait()`` now and then; every item is handed
       out once and all come back
     - Zephyr-specific scenario
   * - 13 work queues
     - none
     - threads and a timer ISR submit, schedule, reschedule, cancel and
       flush plain and delayable work items on the system work queue and a
       second queue; no item runs twice at once or more often than queued,
       all are idle after ``cancel_sync``
     - Zephyr-specific scenario

Running
*******

.. code-block:: console

   west twister -T tests/kernel/torture/sched -p qemu_x86_64

``CONFIG_SCHED_TORTURE_ONLY=<n>`` runs one scenario and
``CONFIG_SCHED_TORTURE_DURATION_MS`` sets how long each timed scenario runs.
Spinlock hold-time checking (``CONFIG_SPIN_LOCK_TIME_LIMIT``) can be added
on platforms with ``CONFIG_SYSTEM_CLOCK_LOCK_FREE_COUNT``; set it above the
locktorture long hold.

Each run picks a new seed for the threads' random numbers and prints it;
``CONFIG_SCHED_TORTURE_SEED`` replays a run (on SMP the interleaving still
differs). ``CONFIG_SCHED_TORTURE_RANDOM_PARAMS`` (off by default, not part of
locktorture) draws locktorture's long_hold, stutter and shuffle from the
seed, between off and twice their setting, as FreeBSD's stress2 randomizes
its parameters.
