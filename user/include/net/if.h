#ifndef _NET_IF_H
#define _NET_IF_H

#define IFNAMSIZ 16

struct ifreq {
    char ifr_name[IFNAMSIZ];
};

#endif
