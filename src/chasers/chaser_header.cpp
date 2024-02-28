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
#include <bitcoin/node/chasers/chaser_header.hpp>

#include <functional>
#include <utility>
#include <bitcoin/network.hpp>
#include <bitcoin/node/error.hpp>
#include <bitcoin/node/full_node.hpp>
#include <bitcoin/node/chasers/chaser.hpp>

namespace libbitcoin {
namespace node {
    
using namespace network;
using namespace system;
using namespace system::chain;
using namespace std::placeholders;

BC_PUSH_WARNING(NO_NEW_OR_DELETE)
BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

chaser_header::chaser_header(full_node& node) NOEXCEPT
  : chaser(node),
    minimum_work_(config().bitcoin.minimum_work),
    milestone_(config().bitcoin.milestone),
    checkpoints_(config().bitcoin.checkpoints),
    currency_window_(config().node.currency_window()),
    use_currency_window_(to_bool(config().node.currency_window_minutes))
{
}

chaser_header::~chaser_header() NOEXCEPT
{
}

// protected
const network::wall_clock::duration&
chaser_header::currency_window() const NOEXCEPT
{
    return currency_window_;
}

// protected
bool chaser_header::use_currency_window() const NOEXCEPT
{
    return use_currency_window_;
}

// protected
code chaser_header::start() NOEXCEPT
{
    BC_ASSERT_MSG(node_stranded(), "chaser_header");

    // Initialize cache of top candidate chain state.
    // Spans full chain to obtain cumulative work. This can be optimized by
    // storing it with each header, though the scan is fast. The same occurs
    // when a block first branches below the current chain top. Chain work is
    // a questionable DoS protection scheme only, so could also toss it.
    top_state_ = archive().get_candidate_chain_state(
        config().bitcoin, archive().get_top_candidate());

    return subscribe(
        std::bind(&chaser_header::handle_event,
            this, _1, _2, _3));
}

// protected
void chaser_header::handle_event(const code& ec, chase event_,
    link value) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&chaser_header::do_handle_event,
            this, ec, event_, value));
}

// private
void chaser_header::do_handle_event(const code&, chase event_, link) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "chaser_header");

    if (event_ == chase::stop)
        tree_.clear();
}

void chaser_header::organize(const header::cptr& header,
    organize_handler&& handler) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&chaser_header::do_organize,
            this, header, std::move(handler)));
}

// protected
// ----------------------------------------------------------------------------

void chaser_header::do_organize(const header::cptr& header_ptr,
    const organize_handler& handler) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "chaser_header");

    auto& query = archive();
    const auto& header = *header_ptr;
    const auto& coin = config().bitcoin;
    const auto hash = header.hash();

    // Skip existing/orphan, get state.
    // ------------------------------------------------------------------------

    if (closed())
    {
        handler(network::error::service_stopped, {});
        return;
    }

    if (tree_.contains(hash) || query.is_header(hash))
    {
        handler(error::duplicate_header, {});
        return;
    }

    auto state = get_state(header.previous_block_hash());
    if (!state)
    {
        handler(error::orphan_header, {});
        return;
    }

    // Roll chain state forward from previous to current header.
    state.reset(new chain_state{ *state, header, coin });
    const auto height = state->height();

    // Validate header.
    // ------------------------------------------------------------------------
    // Header validations are not bypassed when under checkpoint/milestone.

    // Checkpoints are considered chain not block/header validation.
    if (checkpoint::is_conflict(coin.checkpoints, hash, height))
    {
        handler(system::error::checkpoint_conflict, height);
        return;
    }

    if (const auto error = header.check(coin.timestamp_limit_seconds,
        coin.proof_of_work_limit, coin.scrypt_proof_of_work))
    {
        handler(error, height);
        return;
    }

    if (const auto error = header.accept(state->context()))
    {
        handler(error, height);
        return;
    }

    // A checkpointed or milestoned branch always gets disk stored. Otherwise
    // branch must be both current and of sufficient chain work to be stored.
    if (!checkpoint::is_at(checkpoints_, height) &&
        !milestone_.equals(hash, height) &&
        !(is_current(header) && state->cumulative_work() >= minimum_work_))
    {
        cache(header_ptr, state);
        handler(error::success, height);
        return;
    }

    // Compute relative work.
    // ------------------------------------------------------------------------

    size_t point{};
    uint256_t work{};
    hashes tree_branch{};
    header_links store_branch{};
    if (!get_branch_work(work, point, tree_branch, store_branch, header))
    {
        handler(error::store_integrity, height);
        return;
    }

    bool strong{};
    if (!get_is_strong(strong, work, point))
    {
        handler(error::store_integrity, height);
        return;
    }

    if (!strong)
    {
        // Header is new top of current weak branch.
        cache(header_ptr, state);
        handler(error::success, height);
        return;
    }

    // Reorganize candidate chain.
    // ------------------------------------------------------------------------

    auto top = top_state_->height();
    if (top < point)
    {
        handler(error::store_integrity, height);
        return;
    }

    // Pop down to the branch point.
    while (top-- > point)
    {
        if (!query.pop_candidate())
        {
            handler(error::store_integrity, height);
            return;
        }
    }

    // Push stored strong headers to candidate chain.
    for (const auto& link: views_reverse(store_branch))
    {
        if (!query.push_candidate(link))
        {
            handler(error::store_integrity, height);
            return;
        }
    }

    // Store strong tree headers and push to candidate chain.
    for (const auto& key: views_reverse(tree_branch))
    {
        if (!push(key))
        {
            handler(error::store_integrity, height);
            return;
        }
    }

    // Push new header as top of candidate chain.
    if (push(header_ptr, state->context()).is_terminal())
    {
        handler(error::store_integrity, height);
        return;
    }

    top_state_ = state;
    const auto branch_point = possible_narrow_cast<height_t>(point);
    notify(error::success, chase::header, { branch_point });
    handler(error::success, height);
}

chain_state::ptr chaser_header::get_state(
    const hash_digest& hash) const NOEXCEPT
{
    if (!top_state_)
        return {};

    // Top state is cached because it is by far the most commonly retrieved.
    if (top_state_->hash() == hash)
        return top_state_;

    const auto it = tree_.find(hash);
    if (it != tree_.end())
        return it->second.state;

    size_t height{};
    const auto& query = archive();
    if (query.get_height(height, query.to_header(hash)))
        return query.get_candidate_chain_state(config().bitcoin, height);

    return {};
}

bool chaser_header::is_current(const header& header) const NOEXCEPT
{
    if (!use_currency_window())
        return true;

    // en.wikipedia.org/wiki/Time_formatting_and_storage_bugs#Year_2106
    const auto time = wall_clock::from_time_t(header.timestamp());
    const auto current = wall_clock::now() - currency_window();
    return time >= current;
}

bool chaser_header::get_branch_work(uint256_t& work, size_t& point,
    hashes& tree_branch, header_links& store_branch,
    const header& header) const NOEXCEPT
{
    // Use pointer to avoid const/copy.
    auto previous = &header.previous_block_hash();
    const auto& query = archive();
    work = header.proof();

    // Sum all branch work from tree.
    for (auto it = tree_.find(*previous); it != tree_.end();
        it = tree_.find(*previous))
    {
        previous = &it->second.header->previous_block_hash();
        tree_branch.push_back(it->second.header->hash());
        work += it->second.header->proof();
    }

    // Sum branch work from store.
    database::height_link link{};
    for (link = query.to_header(*previous); !query.is_candidate_block(link);
        link = query.to_parent(link))
    {
        uint32_t bits{};
        if (link.is_terminal() || !query.get_bits(bits, link))
            return false;

        store_branch.push_back(link);
        work += chain::header::proof(bits);
    }

    // Height of the highest candidate header is the branch point.
    return query.get_height(point, link);
}

// ****************************************************************************
// CONSENSUS: branch with greater work causes candidate reorganization.
// Chasers eventually reorganize candidate branch into confirmed if valid.
// ****************************************************************************
bool chaser_header::get_is_strong(bool& strong, const uint256_t& work,
    size_t point) const NOEXCEPT
{
    strong = false;
    uint256_t candidate_work{};
    const auto& query = archive();

    // Accumulate candidate branch and when exceeds branch return false (weak).
    for (auto height = query.get_top_candidate(); height > point; --height)
    {
        uint32_t bits{};
        if (!query.get_bits(bits, query.to_candidate(height)))
            return false;

        candidate_work += header::proof(bits);
        if (candidate_work >= work)
            return true;
    }

    strong = true;
    return true;
}

void chaser_header::cache(const header::cptr& header,
    const chain_state::ptr& state) NOEXCEPT
{
    tree_.insert({ header->hash(), { header, state } });
}

database::header_link chaser_header::push(const header::cptr& header,
    const context& context) const NOEXCEPT
{
    auto& query = archive();
    const auto link = query.set_link(*header, database::context
    {
        possible_narrow_cast<flags_t>(context.forks),
        possible_narrow_cast<height_t>(context.height),
        context.median_time_past,
    });

    return query.push_candidate(link) ? link : database::header_link{};
}

bool chaser_header::push(const hash_digest& key) NOEXCEPT
{
    const auto value = tree_.extract(key);
    BC_ASSERT_MSG(!value.empty(), "missing tree value");

    auto& query = archive();
    const auto& node = value.mapped();
    const auto link = query.set_link(*node.header, node.state->context());
    return query.push_candidate(link);
}

BC_POP_WARNING()
BC_POP_WARNING()

} // namespace database
} // namespace libbitcoin