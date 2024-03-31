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
#include <bitcoin/node/chasers/chaser_check.hpp>

#include <algorithm>
#include <bitcoin/network.hpp>
#include <bitcoin/node/chasers/chaser.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/full_node.hpp>

namespace libbitcoin {
namespace node {

#define CLASS chaser_check

using namespace system;
using namespace system::chain;
using namespace network;
using namespace std::placeholders;

// Shared pointers required for lifetime in handler parameters.
BC_PUSH_WARNING(NO_VALUE_OR_CONST_REF_SHARED_PTR)
BC_PUSH_WARNING(SMART_PTR_NOT_NEEDED)
BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

chaser_check::chaser_check(full_node& node) NOEXCEPT
  : chaser(node),
    connections_(node.network_settings().outbound_connections),
    inventory_(system::lesser(node.config().node.maximum_inventory,
        network::messages::max_inventory))
{
}

// start
// ----------------------------------------------------------------------------

code chaser_check::start() NOEXCEPT
{
    const auto fork_point = archive().get_fork();
    const auto added = get_unassociated(maps_, fork_point);
    LOGN("Fork point (" << fork_point << ") unassociated (" << added << ").");

    return SUBSCRIBE_EVENTS(handle_event, _1, _2, _3);
}

void chaser_check::handle_event(const code&, chase event_,
    event_link value) NOEXCEPT
{
    using namespace system;
    switch (event_)
    {
        case chase::header:
        {
            POST(do_add_headers, possible_narrow_cast<height_t>(value));
            break;
        }
        case chase::disorganized:
        {
            POST(do_purge_headers, possible_narrow_cast<height_t>(value));
            break;
        }
        case chase::start:
        case chase::pause:
        case chase::resume:
        case chase::starved:
        case chase::split:
        case chase::stall:
        case chase::purge:
        case chase::block:
        ///case chase::header:
        case chase::download:
        case chase::checked:
        case chase::unchecked:
        case chase::preconfirmable:
        case chase::unpreconfirmable:
        case chase::confirmable:
        case chase::unconfirmable:
        case chase::organized:
        case chase::reorganized:
        ///case chase::disorganized:
        case chase::transaction:
        case chase::template_:
        case chase::stop:
        {
            break;
        }
    }
}

// add headers
// ----------------------------------------------------------------------------

void chaser_check::do_add_headers(height_t branch_point) NOEXCEPT
{
    BC_ASSERT(stranded());

    const auto added = get_unassociated(maps_, branch_point);

    ////LOGN("Branch point (" << branch_point << ") unassociated (" << added
    ////    << ").");

    if (is_zero(added))
        return;

    notify(error::success, chase::download, added);
}

// purge headers
// ----------------------------------------------------------------------------

void chaser_check::do_purge_headers(height_t top) NOEXCEPT
{
    BC_ASSERT(stranded());

    // Candidate chain has been reset (from fork point) to confirmed top.
    // Since all blocks are confirmed through fork point, and all above are to
    // be purged, it simply means purge all hashes (reset all). All channels
    // will get the purge notification before any subsequent download notify.
    maps_.clear();

    // It is possible for the previous candidate chain to have been stronger
    // than confirmed (above fork point), given an unconfirmable block found
    // more than one block above fork point. Yet this stronger candidate(s)
    // will be popped, and all channels purged/dropped, once purge is handled.
    // Subsequently there will be no progress on that stronger chain until a
    // new stronger block is found upon channel restarts. In other words such a
    // disorganization accepts a stall, not to exceed a singl block period. As
    // a disorganization is an extrememly rare event: it requires relay of an
    // invalid block with valid proof of work, on top of another strong block
    // that was conicidentally not yet successfully confirmed. This is worth
    // the higher complexity implementation to avoid.
    notify(error::success, chase::purge, top);
}

// get/put hashes
// ----------------------------------------------------------------------------

void chaser_check::get_hashes(map_handler&& handler) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&chaser_check::do_get_hashes,
            this, std::move(handler)));
}

void chaser_check::put_hashes(const map_ptr& map,
    network::result_handler&& handler) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&chaser_check::do_put_hashes,
            this, map, std::move(handler)));
}

void chaser_check::do_get_hashes(const map_handler& handler) NOEXCEPT
{
    BC_ASSERT(stranded());

    const auto map = get_map(maps_);

    ////LOGN("Hashes -" << map->size() << " ("
    ////    << count_map(maps_) << ") remain.");
    handler(error::success, map);
}

void chaser_check::do_put_hashes(const map_ptr& map,
    const network::result_handler& handler) NOEXCEPT
{
    BC_ASSERT(stranded());

    if (!map->empty())
    {
        maps_.push_back(map);
        notify(error::success, chase::download, map->size());
    }

    ////LOGN("Hashes +" << map->size() << " ("
    ////    << count_map(maps_) << ") remain.");
    handler(error::success);
}

// utilities
// ----------------------------------------------------------------------------

size_t chaser_check::get_unassociated(maps& table, size_t start) const NOEXCEPT
{
    size_t added{};
    while (true)
    {
        const auto map = make_map(start, inventory_);
        if (map->empty()) break;
        table.push_back(map);
        start = map->top().height;
        added += map->size();
    }

    return added;
}

size_t chaser_check::count_map(const maps& table) const NOEXCEPT
{
    return std::accumulate(table.begin(), table.end(), zero,
        [](size_t sum, const map_ptr& map) NOEXCEPT
        {
            return sum + map->size();
        });
}

map_ptr chaser_check::make_map(size_t start,
    size_t count) const NOEXCEPT
{
    // TODO: associated queries need to treat any stored-as-malleated block as
    // not associated and store must accept a distinct block of the same bits
    // (when that block passes check), which may also be later found invalid.
    // So the block will show as associated until it is invalidated.
    // The malleated state is basically the same as not associated (hidden).
    // So when replacement block arrives, it should reset to explicit unknown
    // and can then pass through preconfirmable and confirmable. If distinct
    // are also malleable, this will cycle as long as malleable is invalid in
    // the strong chain. However, the cheap malleable is caught on check and
    // the other is rare.
    return std::make_shared<database::associations>(
        archive().get_unassociated_above(start, count));
}

map_ptr chaser_check::get_map(maps& table) NOEXCEPT
{
    return table.empty() ? std::make_shared<database::associations>() :
        pop_front(table);
}

BC_POP_WARNING()
BC_POP_WARNING()
BC_POP_WARNING()

} // namespace database
} // namespace libbitcoin
