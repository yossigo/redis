proc rediscli {port {opts {}}} {
    set cmd [list src/redis-cli -p $port]
    if {$::tls} {
        lappend cmd --tls --cert tests/tls/redis.crt --key tests/tls/redis.key --cacert tests/tls/ca-bundle.crt
    }
    if [llength opts] {
        lappend cmd {*}$opts
    }
    return $cmd
}
