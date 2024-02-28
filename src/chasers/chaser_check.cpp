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

#include <functional>
#include <variant>
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

BC_PUSH_WARNING(NO_THROW_IN_NOEXCEPT)

chaser_check::chaser_check(full_node& node) NOEXCEPT
  : chaser(node)
{
}

chaser_check::~chaser_check() NOEXCEPT
{
}

code chaser_check::start() NOEXCEPT
{
    // Initialize from genesis block.
    handle_header(zero);

    BC_ASSERT_MSG(node_stranded(), "chaser_check");
    return subscribe(
        std::bind(&chaser_check::handle_event,
            this, _1, _2, _3));
}

// protected
void chaser_check::handle_event(const code& ec, chase event_,
    link value) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&chaser_check::do_handle_event,
            this, ec, event_, value));
}

// private
void chaser_check::do_handle_event(const code&, chase event_,
    link value) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "chaser_check");

    if (event_ == chase::header)
    {
        BC_ASSERT(std::holds_alternative<height_t>(value));
        handle_header(std::get<height_t>(value));
    }
}

void chaser_check::get_hashes(handler&& handler) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&chaser_check::do_get_hashes,
            this, std::move(handler)));
}

void chaser_check::put_hashes(const chaser_check::map& map,
    network::result_handler&& handler) NOEXCEPT
{
    boost::asio::post(strand(),
        std::bind(&chaser_check::do_put_hashes,
            this, map, std::move(handler)));
}

// protected
// ----------------------------------------------------------------------------

void chaser_check::do_get_hashes(const handler&) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "chaser_check");
}

void chaser_check::do_put_hashes(const chaser_check::map&,
    const network::result_handler&) NOEXCEPT
{
    BC_ASSERT_MSG(stranded(), "chaser_check");
}

void chaser_check::handle_header(height_t branch_point) NOEXCEPT
{
    // Map and peer maps may have newly stale blocks.
    // All stale branches can just be allowed to complete.
    // The connect chaser will verify proper advancement.
    const auto& query = archive();
    map_.merge(query.get_all_unassociated_above(
        query.get_last_associated_from(branch_point)));
}

BC_POP_WARNING()

} // namespace database
} // namespace libbitcoin