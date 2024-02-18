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
#include <bitcoin/node/protocols/protocol_header_in_31800.hpp>

#include <utility>
#include <bitcoin/system.hpp>
#include <bitcoin/database.hpp>
#include <bitcoin/network.hpp>
#include <bitcoin/node/define.hpp>

namespace libbitcoin {
namespace node {

#define CLASS protocol_header_in_31800

using namespace system;
using namespace network;
using namespace network::messages;
using namespace std::placeholders;

// Shared pointers required for lifetime in handler parameters.
BC_PUSH_WARNING(NO_NEW_OR_DELETE)
BC_PUSH_WARNING(SMART_PTR_NOT_NEEDED)
BC_PUSH_WARNING(NO_VALUE_OR_CONST_REF_SHARED_PTR)

// Start.
// ----------------------------------------------------------------------------

void protocol_header_in_31800::start() NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_header_in_31800");

    if (started())
        return;

    // header sync is always CANDIDATEs.
    state_ = archive().get_candidate_chain_state(config().bitcoin);
    BC_ASSERT_MSG(state_, "Store not initialized.");

    SUBSCRIBE_CHANNEL2(headers, handle_receive_headers, _1, _2);
    SEND1(create_get_headers(), handle_send, _1);
    protocol::start();
}

// Inbound (headers).
// ----------------------------------------------------------------------------

bool protocol_header_in_31800::handle_receive_headers(const code& ec,
    const headers::cptr& message) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "protocol_header_in_31800");

    if (stopped(ec))
        return false;

    const auto& coin = config().bitcoin;

    LOGP("Headers (" << message->header_ptrs.size()
        << ") from [" << authority() << "].");

    // Store each header, drop channel if invalid.
    for (const auto& header_ptr: message->header_ptrs)
    {
        if (stopped())
            return false;

        const auto& header = *header_ptr;
        const auto hash = header.hash();
        if (header.previous_block_hash() != state_->hash())
        {
            // Out of order or invalid.
            LOGP("Orphan header [" << encode_hash(hash)
                << "] from [" << authority() << "].");
            stop(network::error::protocol_violation);
            return false;
        }

        auto error = header.check(coin.timestamp_limit_seconds,
            coin.proof_of_work_limit, coin.scrypt_proof_of_work);
        if (error)
        {
            LOGR("Invalid header (check) [" << encode_hash(hash)
                << "] from [" << authority() << "] " << error.message());
            stop(network::error::protocol_violation);
            return false;
        }

        // Checkpoints are considered chain not header validation.
        if (chain::checkpoint::is_conflict(coin.checkpoints, hash,
            add1(state_->height())))
        {
            LOGR("Invalid header (checkpoint) [" << encode_hash(hash)
                << "] from [" << authority() << "].");
            stop(network::error::protocol_violation);
            return false;
        }

        // Rolling forward chain_state eliminates database cost.
        BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)
        state_.reset(new chain::chain_state(*state_, header, coin));
        BC_POP_WARNING()

        auto context = state_->context();
        error = header.accept(context);
        if (error)
        {
            LOGR("Invalid header (accept) [" << encode_hash(hash)
                << "] from [" << authority() << "] " << error.message());
            stop(network::error::protocol_violation);
            return false;
        }

        // context is moved, so use here first.
        if (is_zero(context.height % 1'000))
            reporter::fire(event_header, context.height);

        organize(header_ptr, std::move(context));
    }

    // Protocol presumes max_get_headers unless complete.
    if (message->header_ptrs.size() == max_get_headers)
    {
        SEND1(create_get_headers({ message->header_ptrs.back()->hash() }),
            handle_send, _1);
    }
    else
    {
        // Completeness assumes empty response from peer if caught up at 2000.
        complete();
    }

    return true;
}

// This could be the end of a catch-up sequence, or a singleton announcement.
// The distinction is ultimately arbitrary, but this signals peer completeness.
void protocol_header_in_31800::complete() NOEXCEPT
{
    ////reporter::fire(event_current_headers, state_->height());
    LOGN("Headers from [" << authority() << "] complete at ("
        << state_->height() << ").");
}

// private
get_headers protocol_header_in_31800::create_get_headers() NOEXCEPT
{
    // Header sync is from the archived (strong) candidate chain.
    // Until the header tree is current the candidate chain remains empty.
    // So all channels will fully sync from the top candidate at their startup.
    return create_get_headers(archive().get_candidate_hashes(
        get_headers::heights(archive().get_top_candidate())));
}

// private
get_headers protocol_header_in_31800::create_get_headers(
    hashes&& hashes) NOEXCEPT
{
    if (!hashes.empty())
    {
        LOGP("Request headers after [" << encode_hash(hashes.front())
            << "] from [" << authority() << "].");
    }

    return { std::move(hashes) };
}

BC_POP_WARNING()
BC_POP_WARNING()
BC_POP_WARNING()

} // namespace node
} // namespace libbitcoin
