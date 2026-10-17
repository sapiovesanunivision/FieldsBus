// AF_PACKET backend of L2Port (Linux; development and CI). Needs CAP_NET_RAW
// (run as root or: sudo setcap cap_net_raw+ep ./pn_device_demo).
#include "softpn/l2_port.hpp"

#include "pn_defs.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

namespace softpn {
namespace {

bool readMac(const std::string& ifname, MacAddress& mac)
{
    std::ifstream f("/sys/class/net/" + ifname + "/address");
    std::string text;
    return f && std::getline(f, text) && MacAddress::parse(text, mac);
}

uint32_t readGateway(const std::string& ifname)
{
    // /proc/net/route: Iface Destination Gateway Flags ... (hex, little-endian on x86/ARM)
    std::ifstream f("/proc/net/route");
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        char iface[64] = {};
        unsigned dest = 0, gw = 0;
        if (std::sscanf(line.c_str(), "%63s %x %x", iface, &dest, &gw) == 3 && ifname == iface && dest == 0)
            return ntohl(gw);
    }
    return 0;
}

class PacketSocketPort : public L2Port {
public:
    PacketSocketPort(AdapterInfo info, int fd, int wakeFd) : info_(std::move(info)), fd_(fd), wakeFd_(wakeFd) {}

    ~PacketSocketPort() override
    {
        ::close(fd_);
        ::close(wakeFd_);
    }

    bool send(const uint8_t* frame, size_t len) override
    {
        uint8_t padded[kMinFrameSize] = {};
        if (len < kMinFrameSize) {
            std::memcpy(padded, frame, len);
            frame = padded;
            len = kMinFrameSize;
        }
        std::lock_guard<std::mutex> lock(txMutex_);
        return ::send(fd_, frame, len, 0) == ssize_t(len);
    }

    bool receive(std::vector<uint8_t>& frame, int timeoutMs) override
    {
        pollfd fds[2] = {{fd_, POLLIN, 0}, {wakeFd_, POLLIN, 0}};
        for (;;) {
            int r = ::poll(fds, 2, timeoutMs);
            if (r <= 0)
                return false;
            if (fds[1].revents & POLLIN) {
                uint64_t v;
                ssize_t ignored = ::read(wakeFd_, &v, sizeof v);
                (void)ignored;
                return false;
            }
            uint8_t buf[kMaxFrameSize + 4];
            sockaddr_ll from{};
            socklen_t fromLen = sizeof from;
            ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n < 14)
                return false;
            if (from.sll_pkttype == PACKET_OUTGOING)
                continue; // our own transmission
            const uint16_t type = uint16_t((buf[12] << 8) | buf[13]);
            const uint16_t inner = n >= 18 ? uint16_t((buf[16] << 8) | buf[17]) : 0;
            const bool wanted = type == kEtherTypeProfinet || type == kEtherTypeLldp ||
                                (type == kEtherTypeVlan && inner == kEtherTypeProfinet) ||
                                (extraType_ != 0 && type == extraType_);
            if (!wanted || std::memcmp(buf + 6, info_.mac.b.data(), 6) == 0)
                continue;
            frame.assign(buf, buf + n);
            return true;
        }
    }

    void interrupt() override
    {
        uint64_t one = 1;
        ssize_t ignored = ::write(wakeFd_, &one, sizeof one);
        (void)ignored;
    }

    const AdapterInfo& info() const override { return info_; }

    LinkStatus link() const override
    {
        // /sys/class/net/<if>/speed is -1 or unreadable without link and on virtual interfaces (veth).
        LinkStatus ls;
        std::ifstream speed("/sys/class/net/" + info_.name + "/speed");
        long mbps = 0;
        if (speed >> mbps && mbps > 0)
            ls.speedMbps = uint32_t(mbps);
        std::ifstream duplex("/sys/class/net/" + info_.name + "/duplex");
        std::string d;
        if (duplex >> d)
            ls.fullDuplex = d != "half";
        return ls;
    }

    uint16_t extraType_ = 0;

private:
    AdapterInfo info_;
    int fd_;
    int wakeFd_;
    std::mutex txMutex_;
};

} // namespace

std::vector<AdapterInfo> listAdapters(std::string* error)
{
    std::vector<AdapterInfo> list;
    ifaddrs* all = nullptr;
    if (getifaddrs(&all) != 0) {
        if (error)
            *error = std::string("getifaddrs: ") + std::strerror(errno);
        return list;
    }
    for (ifaddrs* a = all; a; a = a->ifa_next) {
        if (!a->ifa_name || (a->ifa_flags & IFF_LOOPBACK))
            continue;
        AdapterInfo* info = nullptr;
        for (AdapterInfo& x : list)
            if (x.name == a->ifa_name)
                info = &x;
        if (!info) {
            AdapterInfo n;
            n.name = n.friendlyName = a->ifa_name;
            n.backend = "af_packet";
            if (!readMac(n.name, n.mac) || n.mac.isZero())
                continue;
            n.gateway = readGateway(n.name);
            list.push_back(n);
            info = &list.back();
        }
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && info->ipv4 == 0) {
            info->ipv4 = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
            if (a->ifa_netmask)
                info->netmask = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
        }
    }
    freeifaddrs(all);
    return list;
}

std::vector<AdapterInfo> listAdapters(L2Backend, std::string* error)
{
    return listAdapters(error); // AF_PACKET only: the Windows backend choice does not apply
}

std::unique_ptr<L2Port> openL2Port(const std::string& selector, const L2Options& options, std::string* error)
{
    std::vector<AdapterInfo> list = listAdapters(error);
    MacAddress mac;
    uint32_t ip = 0;
    const bool byMac = MacAddress::parse(selector, mac);
    const bool byIp = parseIpv4(selector, ip);
    const AdapterInfo* found = nullptr;
    for (const AdapterInfo& a : list)
        if (a.name == selector || (byMac && a.mac == mac) || (byIp && a.ipv4 == ip))
            found = &a;
    if (!found) {
        if (error)
            *error = "adapter '" + selector + "' not found (pn_probe --list shows the names)";
        return nullptr;
    }

    int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        if (error)
            *error = std::string("socket(AF_PACKET): ") + std::strerror(errno) + " (needs CAP_NET_RAW)";
        return nullptr;
    }
    const int ifindex = int(if_nametoindex(found->name.c_str()));
    sockaddr_ll addr{};
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = ifindex;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (error)
            *error = std::string("bind(AF_PACKET): ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    auto join = [&](const uint8_t* group) {
        packet_mreq mr{};
        mr.mr_ifindex = ifindex;
        mr.mr_type = PACKET_MR_MULTICAST;
        mr.mr_alen = 6;
        std::memcpy(mr.mr_address, group, 6);
        setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr);
    };
    join(dcp::kMulticastIdentify);
    join(lldp::kMulticast);
    if (options.promiscuous) {
        packet_mreq mr{};
        mr.mr_ifindex = ifindex;
        mr.mr_type = PACKET_MR_PROMISC;
        setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr);
    }
    int wakeFd = ::eventfd(0, EFD_NONBLOCK);
    if (wakeFd < 0) {
        if (error)
            *error = std::string("eventfd: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    auto port = std::make_unique<PacketSocketPort>(*found, fd, wakeFd);
    port->extraType_ = options.extraEtherType;
    return port;
}

} // namespace softpn
