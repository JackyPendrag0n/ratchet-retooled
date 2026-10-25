# lv2 syscalls the EBOOT issues directly (`sc`, number from `li r11, N`)

80 distinct numbers across 609 call sites (23 more sites load r11 indirectly).
Names come from RPCS3's syscall table (lv2.cpp).

| Group | Syscalls (call sites) |
|---|---|
| process | 1 getpid (1), 19 process_kill (1) |
| ppu thread | 43 yield (4), 44 join (24), 47 set_priority (18), 48 get_priority (5) |
| interrupt | 88 interrupt_thread_eoi (6) |
| mutex | 100 create (17), 101 destroy (46), 102 lock (140), 103 trylock (7), 104 unlock (119) |
| cond | 105 create (10), 106 destroy (9), 107 wait (8), 108 signal (9), 109 signal_all (1) |
| rwlock | 120 create, 121 destroy, 122 rlock, 124 runlock, 125 wlock, 127 wunlock |
| event | 128 queue_create, 129 queue_destroy, 130 queue_receive, 134 port_create, 135 port_destroy, 136 port_connect_local, 137 port_disconnect, 138 port_send |
| time | 141 timer_usleep (49), 144 get_timezone, 145 get_current_time, 147 get_timebase_frequency |
| raw spu | 151/152 set/get_int_mask, 153/154 set/get_int_stat, 160 raw_spu_create, 163 read_puint_mb |
| spu thread | 169 spu_initialize, 170 group_create, 173 group_start, 174 suspend, 175 resume, 177 terminate, 181 write_ls, 182 read_ls (14), 190 write_spu_mb |
| memory | 330 mmapper_allocate_address, 341 container_create, 342 container_destroy, 348 memory_allocate, 351 get_page_attribute, 352 get_user_memory_size |
| system | 387 sm_get_system_info, 402 tty_read, 403 tty_write (12) |
| fs | 801 open … 834 chmod (open/read/write/close/opendir/readdir/closedir/stat/fstat/mkdir/rename/rmdir/unlink/utime/fcntl/lseek/fsync/truncate/ftruncate) |
