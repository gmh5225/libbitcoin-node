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
#ifndef LIBBITCOIN_NODE_CHASERS_CHASER_ORGANIZE_IPP
#define LIBBITCOIN_NODE_CHASERS_CHASER_ORGANIZE_IPP

#include <bitcoin/database.hpp>
#include <bitcoin/network.hpp>
#include <bitcoin/node/chasers/chaser.hpp>
#include <bitcoin/node/define.hpp>

namespace libbitcoin {
namespace node {

// Public
// ----------------------------------------------------------------------------

TEMPLATE
CLASS::chaser_organize(full_node& node) NOEXCEPT
  : chaser(node),
    settings_(config().bitcoin)
{
}

TEMPLATE
code CLASS::start() NOEXCEPT
{
    using namespace system;
    using namespace std::placeholders;
    const auto& query = archive();

    // Initialize cache of top candidate chain state.
    // Spans full chain to obtain cumulative work. This can be optimized by
    // storing it with each header, though the scan is fast. The same occurs
    // when a block first branches below the current chain top. Chain work
    // is a questionable DoS protection scheme only, so could also toss it.
    state_ = query.get_candidate_chain_state(settings_,
        query.get_top_candidate());

    LOGN("Candidate top [" << encode_hash(state_->hash()) << ":"
        << state_->height() << "].");

    return SUBSCRIBE_EVENTS(handle_event, _1, _2, _3);
}

TEMPLATE
void CLASS::organize(const typename Block::cptr& block_ptr,
    organize_handler&& handler) NOEXCEPT
{
    POST(do_organize, block_ptr, std::move(handler));
}

// Properties
// ----------------------------------------------------------------------------

TEMPLATE
const system::settings& CLASS::settings() const NOEXCEPT
{
    return settings_;
}

TEMPLATE
const typename CLASS::block_tree& CLASS::tree() const NOEXCEPT
{
    return tree_;
}

// Methods
// ----------------------------------------------------------------------------

TEMPLATE
void CLASS::handle_event(const code&, chase event_, event_link value) NOEXCEPT
{
    switch (event_)
    {
        case chase::unchecked:
        case chase::unpreconfirmed:
        case chase::unconfirmed:
        {
            BC_ASSERT(std::holds_alternative<header_t>(value));
            POST(do_disorganize, std::get<header_t>(value));
            break;
        }
        case chase::header:
        case chase::download:
        case chase::starved:
        case chase::split:
        case chase::stall:
        case chase::purge:
        case chase::pause:
        case chase::resume:
        case chase::bump:
        case chase::checked:
        ////case chase::unchecked:
        case chase::preconfirmed:
        ////case chase::unpreconfirmed:
        case chase::confirmed:
        ////case chase::unconfirmed:
        case chase::disorganized:
        case chase::transaction:
        case chase::template_:
        case chase::block:
        case chase::stop:
        {
            break;
        }
    }
}

TEMPLATE
void CLASS::do_organize(typename Block::cptr& block_ptr,
    const organize_handler& handler) NOEXCEPT
{
    BC_ASSERT(stranded());

    using namespace system;
    const auto& block = *block_ptr;
    const auto hash = block.hash();
    const auto header = get_header(block);
    auto& query = archive();

    // Skip existing/orphan, get state.
    // ........................................................................

    if (closed())
    {
        handler(network::error::service_stopped, {});
        return;
    }

    const auto it = tree_.find(hash);
    if (it != tree_.end())
    {
        handler(error_duplicate(), it->second.state->height());
        return;
    }

    // If exists test for prior invalidity.
    const auto link = query.to_header(hash);
    if (!link.is_terminal())
    {
        size_t height{};
        if (!query.get_height(height, link))
        {
            handler(error::store_integrity, {});
            close(error::store_integrity);
            return;
        }

        const auto ec = query.get_header_state(link);
        if (ec == database::error::block_unconfirmable)
        {
            handler(ec, height);
            return;
        }

        if (!is_block() || ec != database::error::unassociated)
        {
            handler(error_duplicate(), height);
            return;
        }
    }

    // Obtains from state_, tree, or store as applicable.
    auto state = get_chain_state(header.previous_block_hash());
    if (!state)
    {
        handler(error_orphan(), {});
        return;
    }

    // Roll chain state forward from previous to current header.
    // ........................................................................

    const auto prev_forks = state->forks();
    const auto prev_version = state->minimum_block_version();

    // Do not use block parameter in chain_state{} as that is for tx pool.

    BC_PUSH_WARNING(NO_NEW_OR_DELETE)
    state.reset(new chain::chain_state{ *state, header, settings_ });
    BC_POP_WARNING()

    const auto height = state->height();
    const auto next_forks = state->forks();
    if (prev_forks != next_forks)
    {
        const binary prev{ fork_bits, to_big_endian(prev_forks) };
        const binary next{ fork_bits, to_big_endian(next_forks) };
        LOGN("Forked from ["
            << prev << "] to ["
            << next << "] at ["
            << height << ":" << encode_hash(hash) << "].");
    }

    const auto next_version = state->minimum_block_version();
    if (prev_version != next_version)
    {
        LOGN("Minimum block version ["
            << prev_version << "] changed to ["
            << next_version << "] at ["
            << height << ":" << encode_hash(hash) << "].");
    }

    // Validation and currency.
    // ........................................................................

    if (chain::checkpoint::is_conflict(settings_.checkpoints, hash, height))
    {
        handler(system::error::checkpoint_conflict, height);
        return;
    };

    if (const auto ec = validate(block, *state))
    {
        handler(ec, height);
        return;
    }

    if (!is_storable(block, *state))
    {
        cache(block_ptr, state);
        handler(error::success, height);
        return;
    }

    // Compute relative work.
    // ........................................................................

    uint256_t work{};
    hashes tree_branch{};
    size_t branch_point{};
    header_links store_branch{};
    if (!get_branch_work(work, branch_point, tree_branch, store_branch, header))
    {
        handler(error::store_integrity, height);
        close(error::store_integrity);
        return;
    }

    bool strong{};
    if (!get_is_strong(strong, work, branch_point))
    {
        handler(error::store_integrity, height);
        close(error::store_integrity);
        return;
    }

    if (!strong)
    {
        // New top of current weak branch.
        cache(block_ptr, state);
        handler(error::success, height);
        return;
    }

    // Reorganize candidate chain.
    // ........................................................................

    auto top = state_->height();
    if (top < branch_point)
    {
        handler(error::store_integrity, height);
        close(error::store_integrity);
        return;
    }

    // Pop down to the branch point.
    while (top-- > branch_point)
    {
        if (!query.pop_candidate())
        {
            handler(error::store_integrity, height);
            close(error::store_integrity);
            return;
        }

        fire(events::header_reorganized, height);
    }

    // Push stored strong headers to candidate chain.
    for (const auto& id: views_reverse(store_branch))
    {
        if (!query.push_candidate(id))
        {
            handler(error::store_integrity, height);
            close(error::store_integrity);
            return;
        }

        fire(events::header_organized, height);
    }

    // Store strong tree headers and push to candidate chain.
    for (const auto& key: views_reverse(tree_branch))
    {
        if (!push(key))
        {
            handler(error::store_integrity, height);
            close(error::store_integrity);
            return;
        }

        fire(events::header_archived, height);
        fire(events::header_organized, height);
    }

    // Push new header as top of candidate chain.
    {
        if (push(block_ptr, state->context()).is_terminal())
        {
            handler(error::store_integrity, height);
            close(error::store_integrity);
            return;
        }

        fire(events::header_archived, height);
        fire(events::header_organized, height);
    }

    // Reset top chain state cache and notify.
    // ........................................................................

    // Delay headers so can get current before block download starts.
    // Checking currency before notify also avoids excessive work backlog.
    if (is_block() || is_current(header.timestamp()))
        notify(error::success, chase_object(), branch_point);

    state_ = state;
    handler(error::success, height);
}

TEMPLATE
void CLASS::do_disorganize(header_t link) NOEXCEPT
{
    BC_ASSERT(stranded());

    using namespace system;

    // Skip already reorganized out, get height.
    // ........................................................................

    // Upon restart candidate chain validation will hit unconfirmable block.
    if (closed())
        return;

    // If header is not a current candidate it has been reorganized out.
    // If header becomes candidate again its unconfirmable state is handled.
    auto& query = archive();
    if (!query.is_candidate_block(link))
        return;

    size_t height{};
    if (!query.get_height(height, link) || is_zero(height))
    {
        close(error::internal_error);
        return;
    }

    // Must reorganize down to fork point, since entire branch is now weak.
    const auto fork_point = query.get_fork();
    if (height <= fork_point)
    {
        close(error::internal_error);
        return;
    }

    // Mark candidates above and pop at/above height.
    // ........................................................................

    // Pop from top down to and including header marking each as unconfirmable.
    // Unconfirmability isn't necessary for validation but adds query context.
    for (auto index = query.get_top_candidate(); index > height; --index)
    {
        if (!query.set_block_unconfirmable(query.to_candidate(index)) ||
            !query.pop_candidate())
        {
            close(error::store_integrity);
            return;
        }
    }

    // Candidate at height is already marked as unconfirmable by notifier.
    {
        if (!query.pop_candidate())
        {
            close(error::store_integrity);
            return;
        }

        fire(events::block_disorganized, height);
    }

    // Reset top chain state cache to fork point.
    // ........................................................................

    const auto top_candidate = state_->height();
    const auto prev_forks = state_->forks();
    const auto prev_version = state_->minimum_block_version();
    state_ = query.get_candidate_chain_state(settings_, fork_point);
    if (!state_)
    {
        close(error::store_integrity);
        return;
    }

    const auto next_forks = state_->forks();
    if (prev_forks != next_forks)
    {
        const binary prev{ fork_bits, to_big_endian(prev_forks) };
        const binary next{ fork_bits, to_big_endian(next_forks) };
        LOGN("Forks reverted from ["
            << prev << "] at candidate ("
            << top_candidate << ") to ["
            << next << "] at confirmed ["
            << fork_point << ":" << encode_hash(state_->hash()) << "].");
    }

    const auto next_version = state_->minimum_block_version();
    if (prev_version != next_version)
    {
        LOGN("Minimum block version reverted ["
            << prev_version << "] at candidate ("
            << top_candidate << ") to ["
            << next_version << "] at confirmed ["
            << fork_point << ":" << encode_hash(state_->hash()) << "].");
    }

    // Copy candidates from above fork point to top into header tree.
    // ........................................................................

    auto state = state_;
    for (auto index = add1(fork_point); index <= top_candidate; ++index)
    {
        typename Block::cptr block{};
        if (!get_block(block, index))
        {
            close(error::store_integrity);
            return;
        }

        // Do not use block parameter in chain_state{} as that is for tx pool.
        const auto& header = get_header(*block);

        BC_PUSH_WARNING(NO_NEW_OR_DELETE)
        state.reset(new chain::chain_state{ *state, header, settings_ });
        BC_POP_WARNING()

        cache(block, state);
    }

    // Pop candidates from top to above fork point.
    // ........................................................................
    for (auto index = top_candidate; index > fork_point; --index)
    {
        LOGN("Deorganizing candidate [" << index << "].");

        if (!query.pop_candidate())
        {
            close(error::store_integrity);
            return;
        }
    }

    // Push confirmed headers from above fork point onto candidate chain.
    // ........................................................................
    const auto top_confirmed = query.get_top_confirmed();
    for (auto index = add1(fork_point); index <= top_confirmed; ++index)
    {
        if (!query.push_candidate(query.to_confirmed(index)))
        {
            close(error::store_integrity);
            return;
        }
    }

    // Notify check/download/confirmation to reset to top (clear).
    // As this organizer controls the candidate array, height is definitive.
    notify(error::success, chase::disorganized, top_confirmed);
}

// Private
// ----------------------------------------------------------------------------

TEMPLATE
void CLASS::cache(const typename Block::cptr& block_ptr,
    const system::chain::chain_state::ptr& state) NOEXCEPT
{
    tree_.insert({ block_ptr->hash(), { block_ptr, state } });
}

TEMPLATE
system::chain::chain_state::ptr CLASS::get_chain_state(
    const system::hash_digest& hash) const NOEXCEPT
{
    if (!state_)
        return {};

    // Top state is cached because it is by far the most commonly retrieved.
    if (state_->hash() == hash)
        return state_;

    const auto it = tree_.find(hash);
    if (it != tree_.end())
        return it->second.state;

    // Branch forms from a candidate block below top candidate (expensive).
    size_t height{};
    const auto& query = archive();
    if (query.get_height(height, query.to_header(hash)))
        return query.get_candidate_chain_state(settings_, height);

    return {};
}

// Also obtains branch point for work summation termination.
// Also obtains ordered branch identifiers for subsequent reorg.
TEMPLATE
bool CLASS::get_branch_work(uint256_t& work, size_t& branch_point,
    system::hashes& tree_branch, header_links& store_branch,
    const system::chain::header& header) const NOEXCEPT
{
    // Use pointer to avoid const/copy.
    auto previous = &header.previous_block_hash();
    const auto& query = archive();
    work = header.proof();

    // Sum all branch work from tree.
    for (auto it = tree_.find(*previous); it != tree_.end();
        it = tree_.find(*previous))
    {
        const auto& next = get_header(*it->second.block);
        previous = &next.previous_block_hash();
        tree_branch.push_back(next.hash());
        work += next.proof();
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
        work += system::chain::header::proof(bits);
    }

    // Height of the highest candidate header is the branch point.
    return query.get_height(branch_point, link);
}

// ****************************************************************************
// CONSENSUS: branch with greater work causes candidate reorganization.
// Chasers eventually reorganize candidate branch into confirmed if valid.
// ****************************************************************************
TEMPLATE
bool CLASS::get_is_strong(bool& strong, const uint256_t& work,
    size_t branch_point) const NOEXCEPT
{
    strong = false;
    uint256_t candidate_work{};
    const auto& query = archive();
    const auto top = query.get_top_candidate();

    for (auto height = top; height > branch_point; --height)
    {
        uint32_t bits{};
        if (!query.get_bits(bits, query.to_candidate(height)))
            return false;

        // Not strong is candidate work equals or exceeds new work.
        candidate_work += system::chain::header::proof(bits);
        if (candidate_work >= work)
            return true;
    }

    strong = true;
    return true;
}

TEMPLATE
database::header_link CLASS::push(const typename Block::cptr& block_ptr,
    const system::chain::context& context) const NOEXCEPT
{
    using namespace system;
    auto& query = archive();
    const auto link = query.set_link(*block_ptr, database::context
    {
        possible_narrow_cast<database::context::flag::integer>(context.forks),
        possible_narrow_cast<database::context::block::integer>(context.height),
        context.median_time_past,
    });

    return query.push_candidate(link) ? link : database::header_link{};
}

TEMPLATE
bool CLASS::push(const system::hash_digest& key) NOEXCEPT
{
    const auto value = tree_.extract(key);
    BC_ASSERT_MSG(!value.empty(), "missing tree value");

    auto& query = archive();
    const auto& it = value.mapped();
    const auto link = query.set_link(*it.block, it.state->context());
    return query.push_candidate(link);
}

} // namespace node
} // namespace libbitcoin

#endif
