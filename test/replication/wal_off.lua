#!/usr/bin/env tarantool

box.cfg({
    listen              = os.getenv("LISTEN"),
    replication         = os.getenv("MASTER"),
    memtx_memory        = 107374182,
    wal_mode            = 'none',
    log_nonblock 	= false,
})

require('console').listen(os.getenv('ADMIN'))
