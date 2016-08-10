/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/protocols/protocol_block_in.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/network.hpp>

namespace libbitcoin {
namespace node {

#define NAME "block"
#define CLASS protocol_block_in

using namespace bc::blockchain;
using namespace bc::message;
using namespace bc::network;
using namespace std::placeholders;

static constexpr auto perpetual_timer = true;
static constexpr auto send_headers_version = 70012u;
static const auto get_blocks_interval = asio::seconds(1);

protocol_block_in::protocol_block_in(p2p& network, channel::ptr channel,
    block_chain& blockchain)
  : protocol_timer(network, channel, perpetual_timer, NAME),
    blockchain_(blockchain),
    stop_hash_(null_hash),

    // TODO: move send_headers to a derived class protocol_block_in_70012.
    headers_from_peer_(peer_version().value >= send_headers_version),
    CONSTRUCT_TRACK(protocol_block_in)
{
}

// Start.
//-----------------------------------------------------------------------------

void protocol_block_in::start()
{
    // Use perpetual protocol timer to prevent stall (our heartbeat).
    protocol_timer::start(get_blocks_interval,
        BIND1(send_get_blocks, _1));

    // TODO: move headers to a derived class protocol_block_in_31800.
    SUBSCRIBE2(headers, handle_receive_headers, _1, _2);

    // TODO: move not_found to a derived class protocol_block_in_70001.
    SUBSCRIBE2(not_found, handle_receive_not_found, _1, _2);

    SUBSCRIBE2(inventory, handle_receive_inventory, _1, _2);
    SUBSCRIBE2(block_message, handle_receive_block, _1, _2);

    // TODO: move send_headers to a derived class protocol_block_in_70012.
    if (headers_from_peer_)
    {
        // Allow peer to send headers vs. inventory block anncements.
        SEND2(send_headers(), handle_send, _1, send_headers::command);
    }

    // Subscribe to block acceptance notifications (for gap fill redundancy).
    blockchain_.subscribe_reorganize(
        BIND4(handle_reorganized, _1, _2, _3, _4));

    // Send initial get_[blocks|headers] message by simulating first heartbeat.
    set_event(error::success);
}

// Send get_[headers|blocks] sequence.
//-----------------------------------------------------------------------------

// This is fired by the callback (i.e. base timer and stop handler).
void protocol_block_in::send_get_blocks(const code& ec)
{
    if (stopped())
        return;

    if (ec && ec != error::channel_timeout)
    {
        log::debug(LOG_NODE)
            << "Failure in block timer for [" << authority() << "] "
            << ec.message();
        stop(ec);
        return;
    }

    blockchain_.fetch_block_locator(
        BIND2(handle_fetch_block_locator, _1, _2));
}

void protocol_block_in::handle_fetch_block_locator(const code& ec,
    const hash_list& locator)
{
    if (stopped() || ec == error::service_stopped)
        return;

    if (ec)
    {
        log::error(LOG_NODE)
            << "Internal failure generating block locator for ["
            << authority() << "] " << ec.message();
        stop(ec);
        return;
    }

    ///////////////////////////////////////////////////////////////////////////
    // TODO: manage the stop_hash_ (see v2).
    ///////////////////////////////////////////////////////////////////////////

    // TODO: move get_headers to a derived class protocol_block_in_31800.
    if (headers_from_peer_)
    {
        const get_headers request{ std::move(locator), stop_hash_ };
        SEND2(request, handle_send, _1, request.command);
    }
    else
    {
        const get_blocks request{ std::move(locator), stop_hash_ };
        SEND2(request, handle_send, _1, request.command);
    }
}

// Receive headers|inventory sequence.
//-----------------------------------------------------------------------------

// TODO: move headers to a derived class protocol_block_in_31800.
// This originates from send_header->annoucements and get_headers requests.
bool protocol_block_in::handle_receive_headers(const code& ec,
    headers_ptr message)
{
    if (stopped())
        return false;

    if (ec)
    {
        log::debug(LOG_NODE)
            << "Failure getting headers from [" << authority() << "] "
            << ec.message();
        stop(ec);
        return false;
    }

    // There is no benefit to this use of headers, in fact it is suboptimal.
    // In v3 headers will be used to build block tree before getting blocks.
    const auto response = std::make_shared<get_data>();
    message->to_inventory(response->inventories, inventory_type_id::block);

    // Remove block hashes found in the orphan pool.
    blockchain_.filter_orphans(response,
        BIND2(handle_filter_orphans, _1, response));
    return true;
}

// This originates from default annoucements and get_blocks requests.
bool protocol_block_in::handle_receive_inventory(const code& ec,
    inventory_ptr message)
{
    if (stopped())
        return false;

    if (ec)
    {
        log::debug(LOG_NODE)
            << "Failure getting inventory from [" << authority() << "] "
            << ec.message();
        stop(ec);
        return false;
    }

    const auto response = std::make_shared<get_data>();
    message->reduce(response->inventories, inventory_type_id::block);

    // Remove block hashes found in the orphan pool.
    blockchain_.filter_orphans(response,
        BIND2(handle_filter_orphans, _1, response));
    return true;
}

void protocol_block_in::handle_filter_orphans(const code& ec,
    get_data_ptr message)
{
    if (stopped() || ec == error::service_stopped ||
        message->inventories.empty())
        return;

    if (ec)
    {
        log::error(LOG_NODE)
            << "Internal failure locating missing orphan hashes for ["
            << authority() << "] " << ec.message();
        stop(ec);
        return;
    }
    
    // Remove block hashes found in the blockchain (dups not allowed).
    blockchain_.filter_blocks(message, BIND2(send_get_data, _1, message));
}

void protocol_block_in::send_get_data(const code& ec, get_data_ptr message)
{
    if (stopped() || ec == error::service_stopped ||
        message->inventories.empty())
        return;

    if (ec)
    {
        log::error(LOG_NODE)
            << "Internal failure locating missing block hashes for ["
            << authority() << "] " << ec.message();
        stop(ec);
        return;
    }

    // inventory|headers->get_data[blocks]
    SEND2(*message, handle_send, _1, message->command);
}

// Receive not_found sequence.
//-----------------------------------------------------------------------------

// TODO: move not_found to a derived class protocol_block_in_70001.
bool protocol_block_in::handle_receive_not_found(const code& ec,
    message::not_found::ptr message)
{
    if (stopped())
        return false;

    if (ec)
    {
        log::debug(LOG_NODE)
            << "Failure getting block not_found from [" << authority() << "] "
            << ec.message();
        stop(ec);
        return false;
    }

    hash_list hashes;
    message->to_hashes(hashes, inventory_type_id::block);

    // The peer cannot locate a block that it told us it had.
    // This only results from reorganization assuming peer is proper.
    for (const auto hash: hashes)
    {
        log::debug(LOG_NODE)
            << "Block not_found [" << encode_hash(hash) << "] from ["
            << authority() << "]";
    }

    return true;
}

// Receive block sequence.
//-----------------------------------------------------------------------------

bool protocol_block_in::handle_receive_block(const code& ec, block_ptr message)
{
    if (stopped())
        return false;

    if (ec)
    {
        log::debug(LOG_NODE)
            << "Failure getting block from [" << authority() << "] "
            << ec.message();
        stop(ec);
        return false;
    }

    // We will pick this up in handle_reorganized.
    message->set_originator(nonce());

    blockchain_.store(message, BIND1(handle_store_block, _1));
    return true;
}

void protocol_block_in::handle_store_block(const code& ec)
{
    if (stopped() || ec == error::service_stopped)
        return;

    // Ignore the block that we already have, a common result.
    if (ec == error::duplicate)
    {
        log::debug(LOG_NODE)
            << "Redundant block from [" << authority() << "] "
            << ec.message();
        return;
    }

    // There are no other expected errors from the store call.
    if (ec)
    {
        log::warning(LOG_NODE)
            << "Error storing block from [" << authority() << "] "
            << ec.message();
        stop(ec);
        return;
    }

    // The block is accepted as an orphan, possibly for immediate acceptance.
    log::debug(LOG_NODE)
        << "Potential block from [" << authority() << "].";
}

// Subscription.
//-----------------------------------------------------------------------------

// At least one block was accepted into the chain, originating from any peer.
bool protocol_block_in::handle_reorganized(const code& ec, size_t fork_point,
    const block_ptr_list& incoming, const block_ptr_list& outgoing)
{
    if (stopped() || ec == error::service_stopped)
        return false;

    if (ec)
    {
        log::error(LOG_NODE)
            << "Failure handling reorganization: " << ec.message();
        stop(ec);
        return false;
    }

    // Report the blocks that originated from this peer.
    for (const auto block: incoming)
        if (block->originator() == nonce())
            log::debug(LOG_NODE)
                << "Accepted block [" << encode_hash(block->header.hash())
                << "] from [" << authority() << "].";

    return true;
}

} // namespace node
} // namespace libbitcoin
