m c j o i n - tiny multicast testing tool
=========================================
[![Travis Status][]][Travis] [![Coverity Status][]][Coverity Scan]

`mcjoin` is a very simple and easy-to-use tool to test IPv4 multicast.
Simply start a multicast generator (server) on one end and one or more
data sinks (clients).

By default the group `225.1.2.3` and the UDP port `1234` is used, you
may want to use the `MCAST_TEST_NET` from RFC5771, `233.252.0.0/24`, or
the `ompoing(8)` test group `232.43.211.234`, defined in this IETF draft
<http://tools.ietf.org/html/draft-ietf-mboned-ssmping-08> and UDP port
`4321`.  At the moment max 250 groups can be joined.

The latest release is always available from GitHub at  
> https://github.com/troglobit/mcjoin/releases

example
-------

```shell
sender$ mcjoin -s
^C
sender$
```

```shell
receiver$ mcjoin
joined group 225.1.2.3 on eth0 ...
..................................................................^C
Received total: 66 packets
receiver$
```


troubleshooting
---------------

the multicast producer, `mcjoin -s`, can send without a default route,
but the sink need a default route to be able to receive the UDP stream.

in particular, this issue will arise if you run `mcjoin` in isolated
network namespaces in Linux.  e.g.

    ip netns add sink
    ip link set eth2 netns sink
    ip netns exec sink /bin/bash
    ip address add 127.0.0.1/8 dev lo
    ip link set lo up
    ip link set eth2 name eth0
    ip address add 10.0.0.42/24 dev eth0
    ip link set eth0 up
    ip route add default via 10.0.0.1
    mcjoin


usage
-----

```shell
    $ mcjoin -h
    
    Usage: mcjoin [-dhjqsv] [-c COUNT] [-i IFNAME] [-p PORT] [-r SEC] [-t TTL]
                  [[SOURCE,]GROUP0 .. [SOURCE,]GROUPN | [SOURCE,]GROUP+NUM]
    
    Options:
      -c COUNT     Exit after COUNT number of received and/or sent packets
      -d           Debug output
      -h           This help text
      -i IFNAME    Interface to use for multicast groups, default eth0
      -j           Join groups, default unless acting as sender
      -p PORT      UDP port number to listen to, default: 1234
      -q           Quiet mode
      -r SEC       Do a join/leave every SEC seconds
      -s           Act as sender, sends packets to select groups
      -t TTL       TTL to use when sending multicast packets, default 1
      -v           Display program version
    
    Bug report address: https://github.com/troglobit/mcjoin/issues
    Project homepage: https://github.com/troglobit/mcjoin/
```


caveat
------

usually there is a limit of 20 group joins per socket in UNIX, this is
the `IP_MAX_MEMBERSHIPTS` define.  on Linux this can be tweaked using a
`/proc` setting:

    echo 40 > /proc/sys/net/ipv4/igmp_max_memberships

mcjoin has a different approach, it opens a unique socket per each group
to join and for each socket disables the odd `IP_MULTICAST_ALL` socket
option, which is enabled by default.  Citing the Linux `ip(7)` man page,
emphasis added:

> **IP_MULTICAST_ALL** *(since Linux 2.6.31)*
>
> This option can be used to modify the delivery policy of multicast
> messages to sockets bound to the wildcard INADDR_ANY address.  The
> argument is a boolean integer (defaults to 1).  If set to 1, the
> socket will **receive messages from all the groups that have been
> joined globally on the whole system**.  Otherwise, it will deliver
> messages only from the groups that have been explicitly joined (for
> example via the IP_ADD_MEMBERSHIP option) on this particular socket.

hence, by default all multicast applications in UNIX will receive all
multicast frames from all groups joined by all other applications on
the same system ...

... which IMO is a weird default since multicast by default is opt-in,
not opt-out, which is what POSIX makes it.  OK, may it's not mandated by
POSIX, and (unregulated) multicast is akin to broadcast, but still!  I
bet most developer's don't know about this.


build & install
---------------

the [GNU Configure & Build][buildsystem] system use `/usr/local` as the
default install prefix.  for most use-cases this is fine, but if you
want to change this to `/usr` use the `--prefix=/usr` configure option:

    $ ./configure --prefix=/usr
    $ make -j5
    $ sudo make install-strip


building from git
-----------------

if you want to contribute, or simply just try out the latest but
unreleased features, then you need to know a few things about the
[GNU Configure & Build][buildsystem] system:

- `configure.ac` and a per-directory `Makefile.am` are key files
- `configure` and `Makefile.in` are generated from `autogen.sh`,
  they are not stored in GIT but automatically generated for the
  release tarballs
- `Makefile` is generated by `configure` script

to build from GIT; clone the repository and run the `autogen.sh` script.
this requires `automake` and `autoconf` to be installed on your system.
(if you build from a released tarball you don't need them.)

    git clone https://github.com/troglobit/mcjoin.git
    cd mcjoin/
    ./autogen.sh
    ./configure && make
    sudo make install-strip

**NOTE:** GIT sources are a moving target and are not recommended for
  production systems, unless you know what you are doing!


[Travis]:          https://travis-ci.org/troglobit/mcjoin
[Travis Status]:   https://travis-ci.org/troglobit/mcjoin.png?branch=master
[Coverity Scan]:   https://scan.coverity.com/projects/9108
[Coverity Status]: https://scan.coverity.com/projects/9108/badge.svg
[buildsystem]:     https://airs.com/ian/configure/
