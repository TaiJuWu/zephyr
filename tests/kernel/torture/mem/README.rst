Memory allocator torture tests
##############################

Scenarios with a Linux counterpart follow ``stress-ng --malloc --verify``:
instances randomly allocate, reallocate and free blocks, fill them with a
pattern and check it before freeing, so overlapping or reused blocks show
up as corruption. The other scenarios cover Zephyr allocators that have no
Linux counterpart and are marked as such.

Every heap is validated with ``sys_heap_validate()`` during the run, and
``CONFIG_SYS_HEAP_RUNTIME_STATS`` must report nothing allocated once
everything was freed, which catches leaks in the allocator's bookkeeping.

.. list-table::
   :header-rows: 1

   * - Scenario
     - Linux original
     - Zephyr implementation
     - Deviations
   * - 1 heap churn
     - ``stress-ng --malloc --verify``
     - ``k_heap_aligned_alloc``, ``k_heap_realloc`` and ``k_heap_free`` on
       one ``k_heap``, one instance per CPU and more
     - aligned allocation added, as ``k_heap`` offers it; the heap is
       ``CONFIG_MEM_TORTURE_HEAP_SIZE`` bytes, so failed allocations are
       counted, not treated as errors
   * - 2 exhaustion
     - none
     - more threads than a 4 KiB heap can serve, blocking allocations with
       a timeout; a free must wake a waiter
     - Zephyr-specific scenario (``k_heap`` can block, ``malloc`` cannot)
   * - 3 slab and ISR
     - none
     - ``k_mem_slab`` shared by threads and a timer ISR; double allocation
       detected with an ownership mark
     - Zephyr-specific scenario
   * - 4 k_malloc
     - ``stress-ng --malloc --verify``, with ``--malloc-zerofree`` style
       zero checks for calloc
     - ``k_malloc``, ``k_calloc``, ``k_free`` on the system heap
     - none
   * - 5 PSRAM
     - ``stress-ng --malloc --verify``
     - ``shared_multi_heap`` external RAM region on the ESP32-S3
     - ESP32-S3 with ``CONFIG_ESP_SPIRAM`` only

Running
*******

.. code-block:: console

   west twister -T tests/kernel/torture/mem -p qemu_x86_64

Each run picks a new seed for the threads' random numbers and prints it;
``CONFIG_MEM_TORTURE_SEED`` replays a run (on SMP the interleaving still
differs).
