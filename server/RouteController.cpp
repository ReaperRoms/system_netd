/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RouteController.h"

#include "Fwmark.h"

#define LOG_TAG "Netd"
#include "log/log.h"
#include "logwrap/logwrap.h"

#include <arpa/inet.h>
#include <linux/fib_rules.h>
#include <map>
#include <net/if.h>

namespace {

// BEGIN CONSTANTS --------------------------------------------------------------------------------

const uint32_t RULE_PRIORITY_PRIVILEGED_LEGACY     = 11000;
const uint32_t RULE_PRIORITY_SECURE_VPN            = 12000;
const uint32_t RULE_PRIORITY_PER_NETWORK_EXPLICIT  = 13000;
const uint32_t RULE_PRIORITY_PER_NETWORK_INTERFACE = 14000;
const uint32_t RULE_PRIORITY_LEGACY                = 16000;
const uint32_t RULE_PRIORITY_PER_NETWORK_NORMAL    = 17000;
const uint32_t RULE_PRIORITY_DEFAULT_NETWORK       = 19000;
const uint32_t RULE_PRIORITY_MAIN                  = 20000;
// TODO: Uncomment once we are sure everything works.
#if 0
const uint32_t RULE_PRIORITY_UNREACHABLE           = 21000;
#endif

// TODO: These should be turned into per-UID tables once the kernel supports UID-based routing.
const int ROUTE_TABLE_PRIVILEGED_LEGACY = RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX - 901;
const int ROUTE_TABLE_LEGACY            = RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX - 902;

// TODO: These values aren't defined by the Linux kernel, because our UID routing changes are not
// upstream (yet?), so we can't just pick them up from kernel headers. When (if?) the changes make
// it upstream, we'll remove this and rely on the kernel header values. For now, add a static assert
// that will warn us if upstream has given these values some other meaning.
const uint16_t FRA_UID_START = 18;
const uint16_t FRA_UID_END   = 19;
static_assert(FRA_UID_START > FRA_MAX,
             "Android-specific FRA_UID_{START,END} values also assigned in Linux uapi. "
             "Check that these values match what the kernel does and then update this assertion.");

const uid_t INVALID_UID = static_cast<uid_t>(-1);

const uint16_t NETLINK_REQUEST_FLAGS = NLM_F_REQUEST | NLM_F_ACK;
const uint16_t NETLINK_CREATE_REQUEST_FLAGS = NETLINK_REQUEST_FLAGS | NLM_F_CREATE | NLM_F_EXCL;

const sockaddr_nl NETLINK_ADDRESS = {AF_NETLINK, 0, 0, 0};

const uint8_t AF_FAMILIES[] = {AF_INET, AF_INET6};

const char* const IP_VERSIONS[] = {"-4", "-6"};

// Avoids "non-constant-expression cannot be narrowed from type 'unsigned int' to 'unsigned short'"
// warnings when using RTA_LENGTH(x) inside static initializers (even when x is already uint16_t).
constexpr uint16_t U16_RTA_LENGTH(uint16_t x) {
    return RTA_LENGTH(x);
}

// These are practically const, but can't be declared so, because they are used to initialize
// non-const pointers ("void* iov_base") in iovec arrays.
rtattr FRATTR_PRIORITY  = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_PRIORITY };
rtattr FRATTR_TABLE     = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_TABLE };
rtattr FRATTR_FWMARK    = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_FWMARK };
rtattr FRATTR_FWMASK    = { U16_RTA_LENGTH(sizeof(uint32_t)), FRA_FWMASK };
rtattr FRATTR_UID_START = { U16_RTA_LENGTH(sizeof(uid_t)),    FRA_UID_START };
rtattr FRATTR_UID_END   = { U16_RTA_LENGTH(sizeof(uid_t)),    FRA_UID_END };

rtattr RTATTR_TABLE     = { U16_RTA_LENGTH(sizeof(uint32_t)), RTA_TABLE };
rtattr RTATTR_OIF       = { U16_RTA_LENGTH(sizeof(uint32_t)), RTA_OIF };

uint8_t PADDING_BUFFER[RTA_ALIGNTO] = {0, 0, 0, 0};

// END CONSTANTS ----------------------------------------------------------------------------------

std::map<std::string, uint32_t> interfaceToIndex;

uint32_t getRouteTableForInterface(const char* interface) {
    uint32_t index = if_nametoindex(interface);
    if (index) {
        interfaceToIndex[interface] = index;
    } else {
        // If the interface goes away if_nametoindex() will return 0 but we still need to know
        // the index so we can remove the rules and routes.
        std::map<std::string, uint32_t>::iterator it = interfaceToIndex.find(interface);
        if (it != interfaceToIndex.end()) {
            index = it->second;
        }
    }
    return index ? index + RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX : 0;
}

// Sends a netlink request and expects an ack.
// |iov| is an array of struct iovec that contains the netlink message payload.
// The netlink header is generated by this function based on |action| and |flags|.
// Returns -errno if there was an error or if the kernel reported an error.
WARN_UNUSED_RESULT int sendNetlinkRequest(uint16_t action, uint16_t flags, iovec* iov, int iovlen) {
    nlmsghdr nlmsg = {
        .nlmsg_type = action,
        .nlmsg_flags = flags,
    };
    iov[0].iov_base = &nlmsg;
    iov[0].iov_len = sizeof(nlmsg);
    for (int i = 0; i < iovlen; ++i) {
        nlmsg.nlmsg_len += iov[i].iov_len;
    }

    int ret;
    struct {
        nlmsghdr msg;
        nlmsgerr err;
    } response;

    int sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if (sock != -1 &&
            connect(sock, reinterpret_cast<const sockaddr*>(&NETLINK_ADDRESS),
                    sizeof(NETLINK_ADDRESS)) != -1 &&
            writev(sock, iov, iovlen) != -1 &&
            (ret = recv(sock, &response, sizeof(response), 0)) != -1) {
        if (ret == sizeof(response)) {
            ret = response.err.error;  // Netlink errors are negative errno.
            if (ret) {
                ALOGE("netlink response contains error (%s)", strerror(-ret));
            }
        } else {
            ALOGE("bad netlink response message size (%d != %u)", ret, sizeof(response));
            ret = -EBADMSG;
        }
    } else {
        ALOGE("netlink socket/connect/writev/recv failed (%s)", strerror(errno));
        ret = -errno;
    }

    if (sock != -1) {
        close(sock);
    }

    return ret;
}

// Adds or removes a routing rule for IPv4 and IPv6.
//
// + If |table| is non-zero, the rule points at the specified routing table. Otherwise, the rule
//   returns ENETUNREACH.
// + If |mask| is non-zero, the rule matches the specified fwmark and mask. Otherwise, |fwmark| is
//   ignored.
// + If |interface| is non-NULL, the rule matches the specified outgoing interface.
//
// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int modifyIpRule(uint16_t action, uint32_t priority, uint32_t table,
                                    uint32_t fwmark, uint32_t mask, const char* interface,
                                    uid_t uidStart, uid_t uidEnd) {
    // Ensure that if you set a bit in the fwmark, it's not being ignored by the mask.
    if (fwmark & ~mask) {
        ALOGE("mask 0x%x does not select all the bits set in fwmark 0x%x", mask, fwmark);
        return -ERANGE;
    }

    // The interface name must include exactly one terminating NULL and be properly padded, or older
    // kernels will refuse to delete rules.
    uint16_t paddingLength = 0;
    size_t interfaceLength = 0;
    char oifname[IFNAMSIZ];
    if (interface) {
        interfaceLength = strlcpy(oifname, interface, IFNAMSIZ) + 1;
        if (interfaceLength > IFNAMSIZ) {
            ALOGE("interface name too long (%u > %u)", interfaceLength, IFNAMSIZ);
            return -ENAMETOOLONG;
        }
        paddingLength = RTA_SPACE(interfaceLength) - RTA_LENGTH(interfaceLength);
    }

    // Either both start and end UID must be specified, or neither.
    if ((uidStart == INVALID_UID) != (uidEnd == INVALID_UID)) {
        ALOGE("incompatible start and end UIDs (%u vs %u)", uidStart, uidEnd);
        return -EUSERS;
    }
    bool isUidRule = (uidStart != INVALID_UID);

    // Assemble a rule request and put it in an array of iovec structures.
    fib_rule_hdr rule = {
        .action = static_cast<uint8_t>(table ? FR_ACT_TO_TBL : FR_ACT_UNREACHABLE),
    };

    rtattr fraOifname = { U16_RTA_LENGTH(interfaceLength), FRA_OIFNAME };

    iovec iov[] = {
        { NULL,              0 },
        { &rule,             sizeof(rule) },
        { &FRATTR_PRIORITY,  sizeof(FRATTR_PRIORITY) },
        { &priority,         sizeof(priority) },
        { &FRATTR_TABLE,     table ? sizeof(FRATTR_TABLE) : 0 },
        { &table,            table ? sizeof(table) : 0 },
        { &FRATTR_FWMARK,    mask ? sizeof(FRATTR_FWMARK) : 0 },
        { &fwmark,           mask ? sizeof(fwmark) : 0 },
        { &FRATTR_FWMASK,    mask ? sizeof(FRATTR_FWMASK) : 0 },
        { &mask,             mask ? sizeof(mask) : 0 },
        { &FRATTR_UID_START, isUidRule ? sizeof(FRATTR_UID_START) : 0 },
        { &uidStart,         isUidRule ? sizeof(uidStart) : 0 },
        { &FRATTR_UID_END,   isUidRule ? sizeof(FRATTR_UID_END) : 0 },
        { &uidEnd,           isUidRule ? sizeof(uidEnd) : 0 },
        { &fraOifname,       interface ? sizeof(fraOifname) : 0 },
        { oifname,           interfaceLength },
        { PADDING_BUFFER,    paddingLength },
    };

    uint16_t flags = (action == RTM_NEWRULE) ? NETLINK_CREATE_REQUEST_FLAGS : NETLINK_REQUEST_FLAGS;
    for (size_t i = 0; i < ARRAY_SIZE(AF_FAMILIES); ++i) {
        rule.family = AF_FAMILIES[i];
        if (int ret = sendNetlinkRequest(action, flags, iov, ARRAY_SIZE(iov))) {
            return ret;
        }
    }

    return 0;
}

// Adds or deletes an IPv4 or IPv6 route.
// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int modifyIpRoute(uint16_t action, uint32_t table, const char* interface,
                                     const char* destination, const char* nexthop) {
    // At least the destination must be non-null.
    if (!destination) {
        ALOGE("null destination");
        return -EFAULT;
    }

    // Parse the prefix.
    uint8_t rawAddress[sizeof(in6_addr)];
    uint8_t family;
    uint8_t prefixLength;
    int rawLength = parsePrefix(destination, &family, rawAddress, sizeof(rawAddress),
                                &prefixLength);
    if (rawLength < 0) {
        ALOGE("parsePrefix failed for destination %s (%s)", destination, strerror(-rawLength));
        return rawLength;
    }

    if (static_cast<size_t>(rawLength) > sizeof(rawAddress)) {
        ALOGE("impossible! address too long (%d vs %u)", rawLength, sizeof(rawAddress));
        return -ENOBUFS;  // Cannot happen; parsePrefix only supports IPv4 and IPv6.
    }

    // If an interface was specified, find the ifindex.
    uint32_t ifindex;
    if (interface) {
        ifindex = if_nametoindex(interface);
        if (!ifindex) {
            ALOGE("cannot find interface %s", interface);
            return -ENODEV;
        }
    }

    // If a nexthop was specified, parse it as the same family as the prefix.
    uint8_t rawNexthop[sizeof(in6_addr)];
    if (nexthop && inet_pton(family, nexthop, rawNexthop) <= 0) {
        ALOGE("inet_pton failed for nexthop %s", nexthop);
        return -EINVAL;
    }

    // Assemble a rtmsg and put it in an array of iovec structures.
    rtmsg route = {
        .rtm_protocol = RTPROT_STATIC,
        .rtm_type = RTN_UNICAST,
        .rtm_family = family,
        .rtm_dst_len = prefixLength,
    };

    rtattr rtaDst     = { U16_RTA_LENGTH(rawLength), RTA_DST };
    rtattr rtaGateway = { U16_RTA_LENGTH(rawLength), RTA_GATEWAY };

    iovec iov[] = {
        { NULL,          0 },
        { &route,        sizeof(route) },
        { &RTATTR_TABLE, sizeof(RTATTR_TABLE) },
        { &table,        sizeof(table) },
        { &rtaDst,       sizeof(rtaDst) },
        { rawAddress,    static_cast<size_t>(rawLength) },
        { &RTATTR_OIF,   interface ? sizeof(RTATTR_OIF) : 0 },
        { &ifindex,      interface ? sizeof(ifindex) : 0 },
        { &rtaGateway,   nexthop ? sizeof(rtaGateway) : 0 },
        { rawNexthop,    nexthop ? static_cast<size_t>(rawLength) : 0 },
    };

    uint16_t flags = (action == RTM_NEWROUTE) ? NETLINK_CREATE_REQUEST_FLAGS :
                                                NETLINK_REQUEST_FLAGS;
    return sendNetlinkRequest(action, flags, iov, ARRAY_SIZE(iov));
}

WARN_UNUSED_RESULT int modifyPerNetworkRules(unsigned netId, const char* interface,
                                             Permission permission, bool add, bool modifyIptables) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        ALOGE("cannot find interface %s", interface);
        return -ESRCH;
    }

    uint16_t action = add ? RTM_NEWRULE : RTM_DELRULE;

    Fwmark fwmark;
    Fwmark mask;

    // A rule to route traffic based on a chosen outgoing interface.
    //
    // Supports apps that use SO_BINDTODEVICE or IP_PKTINFO options and the kernel that already
    // knows the outgoing interface (typically for link-local communications).
    fwmark.permission = permission;
    mask.permission = permission;
    if (int ret = modifyIpRule(action, RULE_PRIORITY_PER_NETWORK_INTERFACE, table, fwmark.intValue,
                               mask.intValue, interface, INVALID_UID, INVALID_UID)) {
        return ret;
    }

    // A rule to route traffic based on the chosen network.
    //
    // This is for sockets that have not explicitly requested a particular network, but have been
    // bound to one when they called connect(). This ensures that sockets connected on a particular
    // network stay on that network even if the default network changes.
    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;
    if (int ret = modifyIpRule(action, RULE_PRIORITY_PER_NETWORK_NORMAL, table, fwmark.intValue,
                               mask.intValue, NULL, INVALID_UID, INVALID_UID)) {
        return ret;
    }

    // A rule to route traffic based on an explicitly chosen network.
    //
    // Supports apps that use the multinetwork APIs to restrict their traffic to a network.
    //
    // Even though we check permissions at the time we set a netId into the fwmark of a socket, we
    // still need to check it again in the rules here, because a network's permissions may have been
    // updated via modifyNetworkPermission().
    fwmark.explicitlySelected = true;
    mask.explicitlySelected = true;
    if (int ret = modifyIpRule(action, RULE_PRIORITY_PER_NETWORK_EXPLICIT, table, fwmark.intValue,
                               mask.intValue, NULL, INVALID_UID, INVALID_UID)) {
        return ret;
    }

    // An iptables rule to mark incoming packets on a network with the netId of the network.
    //
    // This is so that the kernel can:
    // + Use the right fwmark for (and thus correctly route) replies (e.g.: TCP RST, ICMP errors,
    //   ping replies).
    // + Mark sockets that accept connections from this interface so that the connection stays on
    //   the same interface.
    if (modifyIptables) {
        char markString[UINT32_HEX_STRLEN];
        snprintf(markString, sizeof(markString), "0x%x", netId);
        if (execIptables(V4V6, "-t", "mangle", add ? "-A" : "-D", "INPUT", "-i", interface,
                         "-j", "MARK", "--set-mark", markString, NULL)) {
            ALOGE("failed to change iptables rule that sets incoming packet mark");
            return -EREMOTEIO;
        }
    }

    return 0;
}

WARN_UNUSED_RESULT int modifyVpnRules(unsigned netId, const char* interface, uint16_t action) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        ALOGE("cannot find interface %s", interface);
        return -ESRCH;
    }

    Fwmark fwmark;
    Fwmark mask;

    // A rule to route all traffic from a given set of UIDs to go over the VPN.
    //
    // Notice that this rule doesn't use the netId. I.e., no matter what netId the user's socket may
    // have, if they are subject to this VPN, their traffic has to go through it. Allows the traffic
    // to bypass the VPN if the protectedFromVpn bit is set.
    //
    // TODO: Actually implement the "from a set of UIDs" part.
    fwmark.protectedFromVpn = false;
    mask.protectedFromVpn = true;
    if (int ret = modifyIpRule(action, RULE_PRIORITY_SECURE_VPN, table, fwmark.intValue,
                               mask.intValue, NULL, INVALID_UID, INVALID_UID)) {
        return ret;
    }

    // A rule to allow privileged apps to send traffic over this VPN even if they are not part of
    // the target set of UIDs.
    //
    // This is needed for DnsProxyListener to correctly resolve a request for a user who is in the
    // target set, but where the DnsProxyListener itself is not.
    fwmark.protectedFromVpn = false;
    mask.protectedFromVpn = false;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.permission = PERMISSION_CONNECTIVITY_INTERNAL;
    mask.permission = PERMISSION_CONNECTIVITY_INTERNAL;

    return modifyIpRule(action, RULE_PRIORITY_SECURE_VPN, table, fwmark.intValue, mask.intValue,
                        NULL, INVALID_UID, INVALID_UID);
}

WARN_UNUSED_RESULT int modifyDefaultNetworkRules(const char* interface, Permission permission,
                                                 uint16_t action) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        ALOGE("cannot find interface %s", interface);
        return -ESRCH;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = 0;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(action, RULE_PRIORITY_DEFAULT_NETWORK, table, fwmark.intValue,
                        mask.intValue, NULL, INVALID_UID, INVALID_UID);
}

// Adds or removes an IPv4 or IPv6 route to the specified table and, if it's a directly-connected
// route, to the main table as well.
// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int modifyRoute(const char* interface, const char* destination,
                                   const char* nexthop, uint16_t action,
                                   RouteController::TableType tableType, uid_t /*uid*/) {
    uint32_t table = 0;
    switch (tableType) {
        case RouteController::INTERFACE: {
            table = getRouteTableForInterface(interface);
            break;
        }
        case RouteController::LEGACY: {
            // TODO: Use the UID to assign a unique table per UID instead of this fixed table.
            table = ROUTE_TABLE_LEGACY;
            break;
        }
        case RouteController::PRIVILEGED_LEGACY: {
            // TODO: Use the UID to assign a unique table per UID instead of this fixed table.
            table = ROUTE_TABLE_PRIVILEGED_LEGACY;
            break;
        }
    }
    if (!table) {
        ALOGE("cannot find table for interface %s and tableType %d", interface, tableType);
        return -ESRCH;
    }

    int ret = modifyIpRoute(action, table, interface, destination, nexthop);
    // We allow apps to call requestRouteToHost() multiple times with the same route, so ignore
    // EEXIST failures when adding routes to legacy tables.
    if (ret != 0 && !(action == RTM_NEWROUTE && ret == -EEXIST &&
                      (tableType == RouteController::LEGACY ||
                       tableType == RouteController::PRIVILEGED_LEGACY))) {
        return ret;
    }

    // If there's no nexthop, this is a directly connected route. Add it to the main table also, to
    // let the kernel find it when validating nexthops when global routes are added.
    if (!nexthop) {
        ret = modifyIpRoute(action, RT_TABLE_MAIN, interface, destination, NULL);
        // A failure with action == ADD && errno == EEXIST means that the route already exists in
        // the main table, perhaps because the kernel added it automatically as part of adding the
        // IP address to the interface. Ignore this, but complain about everything else.
        if (ret && !(action == RTM_NEWROUTE && ret == -EEXIST)) {
            return ret;
        }
    }

    return 0;
}

// Returns 0 on success or negative errno on failure.
WARN_UNUSED_RESULT int flushRoutes(const char* interface) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        ALOGE("cannot find interface %s", interface);
        return -ESRCH;
    }
    interfaceToIndex.erase(interface);

    char tableString[UINT32_STRLEN];
    snprintf(tableString, sizeof(tableString), "%u", table);

    for (size_t i = 0; i < ARRAY_SIZE(IP_VERSIONS); ++i) {
        const char* argv[] = {
            IP_PATH,
            IP_VERSIONS[i],
            "route"
            "flush",
            "table",
            tableString,
        };
        if (android_fork_execvp(ARRAY_SIZE(argv), const_cast<char**>(argv), NULL, false, false)) {
            ALOGE("failed to flush routes");
            return -EREMOTEIO;
        }
    }

    return 0;
}

}  // namespace

int RouteController::Init() {
    Fwmark fwmark;
    Fwmark mask;

    // Add a new rule to look up the 'main' table, with the same selectors as the "default network"
    // rule, but with a lower priority. Since the default network rule points to a table with a
    // default route, the rule we're adding will never be used for normal routing lookups. However,
    // the kernel may fall-through to it to find directly-connected routes when it validates that a
    // nexthop (in a route being added) is reachable.
    //
    // TODO: This isn't true if the default network requires non-zero permissions. In that case, an
    // app without those permissions may still be able to access directly-connected routes, since
    // it won't match the default network rule. Fix this by only allowing the root UID (as a proxy
    // for the kernel) to lookup this main table rule.
    fwmark.netId = 0;
    mask.netId = FWMARK_NET_ID_MASK;
    if (int ret = modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_MAIN, RT_TABLE_MAIN, fwmark.intValue,
                               mask.intValue, NULL, INVALID_UID, INVALID_UID)) {
        return ret;
    }

    // Add rules to allow lookup of legacy routes.
    //
    // TODO: Remove these once the kernel supports UID-based routing. Instead, add them on demand
    // when routes are added.
    fwmark.netId = 0;
    mask.netId = 0;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;
    if (int ret = modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_LEGACY, ROUTE_TABLE_LEGACY,
                               fwmark.intValue, mask.intValue, NULL, INVALID_UID, INVALID_UID)) {
        return ret;
    }

    fwmark.permission = PERMISSION_CONNECTIVITY_INTERNAL;
    mask.permission = PERMISSION_CONNECTIVITY_INTERNAL;

    if (int ret = modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_PRIVILEGED_LEGACY,
                               ROUTE_TABLE_PRIVILEGED_LEGACY, fwmark.intValue, mask.intValue, NULL,
                               INVALID_UID, INVALID_UID)) {
        return ret;
    }

// TODO: Uncomment once we are sure everything works.
#if 0
    // Add a rule to preempt the pre-defined "from all lookup main" rule. This ensures that packets
    // that are already marked with a specific NetId don't fall-through to the main table.
    return modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_UNREACHABLE, 0, 0, 0, NULL, INVALID_UID,
                        INVALID_UID);
#else
    return 0;
#endif
}

int RouteController::addInterfaceToNetwork(unsigned netId, const char* interface,
                                           Permission permission) {
    return modifyPerNetworkRules(netId, interface, permission, true, true);
}

int RouteController::removeInterfaceFromNetwork(unsigned netId, const char* interface,
                                                Permission permission) {
    if (int ret = modifyPerNetworkRules(netId, interface, permission, false, true)) {
        return ret;
    }
    return flushRoutes(interface);
}

int RouteController::addInterfaceToVpn(unsigned netId, const char* interface) {
    if (int ret = modifyPerNetworkRules(netId, interface, PERMISSION_NONE, true, true)) {
        return ret;
    }
    return modifyVpnRules(netId, interface, RTM_NEWRULE);
}

int RouteController::removeInterfaceFromVpn(unsigned netId, const char* interface) {
    if (int ret = modifyPerNetworkRules(netId, interface, PERMISSION_NONE, false, true)) {
        return ret;
    }
    if (int ret = modifyVpnRules(netId, interface, RTM_DELRULE)) {
        return ret;
    }
    return flushRoutes(interface);
}

int RouteController::modifyNetworkPermission(unsigned netId, const char* interface,
                                             Permission oldPermission, Permission newPermission) {
    // Add the new rules before deleting the old ones, to avoid race conditions.
    if (int ret = modifyPerNetworkRules(netId, interface, newPermission, true, false)) {
        return ret;
    }
    return modifyPerNetworkRules(netId, interface, oldPermission, false, false);
}

int RouteController::addToDefaultNetwork(const char* interface, Permission permission) {
    return modifyDefaultNetworkRules(interface, permission, RTM_NEWRULE);
}

int RouteController::removeFromDefaultNetwork(const char* interface, Permission permission) {
    return modifyDefaultNetworkRules(interface, permission, RTM_DELRULE);
}

int RouteController::addRoute(const char* interface, const char* destination, const char* nexthop,
                              TableType tableType, uid_t uid) {
    return modifyRoute(interface, destination, nexthop, RTM_NEWROUTE, tableType, uid);
}

int RouteController::removeRoute(const char* interface, const char* destination,
                                 const char* nexthop, TableType tableType, uid_t uid) {
    return modifyRoute(interface, destination, nexthop, RTM_DELROUTE, tableType, uid);
}
