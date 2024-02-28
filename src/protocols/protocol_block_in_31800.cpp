/**
 * Copyright (c) 2011-2023 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/protocols/protocol_block_in_31800.hpp>

#include <functional>
#include <bitcoin/system.hpp>
#include <bitcoin/database.hpp>
#include <bitcoin/network.hpp>
#include <bitcoin/node/chasers/chasers.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/error.hpp>

namespace libbitcoin {
namespace node {

#define CLASS protocol_block_in_31800

using namespace system;
using namespace network;
using namespace network::messages;
using namespace std::placeholders;

// Shared pointers required for lifetime in handler parameters.
BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)
BC_PUSH_WARNING(SMART_PTR_NOT_NEEDED)
BC_PUSH_WARNING(NO_VALUE_OR_CONST_REF_SHARED_PTR)

// Performance polling.
// ----------------------------------------------------------------------------

void protocol_block_in_31800::handle_performance_timer(const code& ec) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "expected channel strand");

    if (stopped() || ec == network::error::operation_canceled)
        return;

    if (ec)
    {
        LOGF("Performance timer error, " << ec.message());
        stop(ec);
        return;
    }

    // Compute rate in bytes per second.
    const auto now = steady_clock::now();
    const auto gap = std::chrono::duration_cast<seconds>(now - start_).count();
    const auto rate = floored_divide(bytes_, gap);
    LOGN("Rate ["
        << identifier() << "] ("
        << bytes_ << "/"
        << gap << " = "
        << rate << ").");

    // Reset counters and log rate.
    bytes_ = zero;
    start_ = now;

    // Bounces to network strand, performs work, then calls handler.
    // Channel will continue to process blocks while this call excecutes on the
    // network strand. Timer will not be restarted until this call completes.
    performance(identifier(), rate, BIND1(handle_performance, ec));
}

void protocol_block_in_31800::handle_performance(const code& ec) NOEXCEPT
{
    POST1(do_handle_performance, ec);
}

void protocol_block_in_31800::do_handle_performance(const code& ec) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "expected network strand");

    if (stopped())
        return;

    // stalled_channel or slow_channel
    if (ec)
    {
        LOGF("Performance action, " << ec.message());
        stop(ec);
        return;
    };

    performance_timer_->start(BIND1(handle_performance_timer, _1));
}

// Start/stop.
// ----------------------------------------------------------------------------

void protocol_block_in_31800::start() NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_block_in_31800");

    if (started())
        return;

    if (report_performance_)
    {
        start_ = steady_clock::now();
        performance_timer_->start(BIND1(handle_performance_timer, _1));
    }

    SUBSCRIBE_CHANNEL2(block, handle_receive_block, _1, _2);
    get_hashes(BIND2(handle_get_hashes, _1, _2));
    protocol::start();
}

void protocol_block_in_31800::stopping(const code& ec) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_block_in_31800");

    performance_timer_->stop();
    put_hashes(map_, BIND1(handle_put_hashes, _1));
    protocol::stopping(ec);
}

// Inbound (blocks).
// ----------------------------------------------------------------------------
// TODO: need map pointer from chaser to avoid large map copies here.

void protocol_block_in_31800::handle_get_hashes(const code& ec,
    const chaser_check::map& map) NOEXCEPT
{
    POST2(do_handle_get_hashes, ec, map);
}

// private
void protocol_block_in_31800::do_handle_get_hashes(const code& ec,
    const chaser_check::map& map) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_block_in_31800");
    BC_ASSERT_MSG(map.size() < max_inventory, "inventory overflow");

    if (ec)
    {
        LOGF("Error getting block hashes for [" << authority() << "] "
            << ec.message());
        stop(ec);
        return;
    }

    if (map.empty())
    {
        LOGP("Exhausted block hashes at [" << authority() << "] "
            << ec.message());
        return;
    }

    SEND1(create_get_data(map), handle_send, _1);
}

void protocol_block_in_31800::handle_put_hashes(const code& ec) NOEXCEPT
{
    if (ec)
    {
        LOGF("Error putting block hashes for [" << authority() << "] "
            << ec.message());
    }
}

bool protocol_block_in_31800::handle_receive_block(const code& ec,
    const block::cptr& message) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_block_in_31800");

    if (stopped(ec))
        return false;

    const auto& block = *message->block_ptr;
    const auto hash = block.hash();
    const auto it = map_.find(hash);

    if (it == map_.end())
    {
        // TODO: store and signal invalid block state (reorgs header chaser).
        LOGR("Unrequested block [" << encode_hash(hash) << "] from ["
            << authority() << "].");
        stop(node::error::unknown);
        return false;
    }

    code error{};
    const auto& ctx = it->second;
    if (((error = block.check())) || ((error = block.check(ctx))))
    {
        // TODO: store and signal invalid block state (reorgs header chaser).
        LOGR("Invalid block [" << encode_hash(hash) << "] from ["
            << authority() << "] " << error.message());
        stop(error);
        return false;
    }

    // TODO: optimize using header_fk with txs (or remove header_fk).
    if (archive().set_link(block).is_terminal())
    {
        // TODO: store and signal invalid block state (reorgs header chaser).
        LOGF("Failure storing block [" << encode_hash(hash) << "].");
        stop(node::error::store_integrity);
        return false;
    }

    // Block check accounted for.
    map_.erase(it);
    bytes_ += message->cached_size;

    // Get some more work from chaser.
    if (is_zero(map_.size()))
    {
        LOGP("Getting more block hashes for [" << authority() << "].");
        get_hashes(BIND2(handle_get_hashes, _1, _2));
    }

    return true;
}

// private
// ----------------------------------------------------------------------------

get_data protocol_block_in_31800::create_get_data(
    const chaser_check::map& map) const NOEXCEPT
{
    // clang emplace_back bug (no matching constructor), using push_back.
    // bip144: get_data uses witness constant but inventory does not.

    get_data getter{};
    getter.items.reserve(map.size());
    for (const auto& item: map)
        getter.items.push_back({ block_type_, item.first });

    return getter;
}

BC_POP_WARNING()
BC_POP_WARNING()
BC_POP_WARNING()

} // namespace node
} // namespace libbitcoin