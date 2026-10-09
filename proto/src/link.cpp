#include "g2/proto/link.hpp"

#include "g2/proto/emulator.hpp"

namespace g2::proto {

LocalLink::LocalLink(std::unique_ptr<Transport> transport, Client::Options options)
    : transport_(std::move(transport)), client_(*transport_, clock_, options)
{
}

LocalLink::~LocalLink() = default;

std::unique_ptr<LocalLink> LocalLink::virtualG2()
{
    auto emulator = std::make_unique<Emulator>();
    auto transport = std::make_unique<EmulatorTransport>(*emulator);
    transport->plugIn();
    auto link = std::make_unique<LocalLink>(std::move(transport));
    link->emulator_ = std::move(emulator);
    return link;
}

} // namespace g2::proto
