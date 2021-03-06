/*
 * Copyright 2019 Peifeng Yu <peifeng@umich.edu>
 * 
 * This file is part of Salus
 * (see https://github.com/SymbioticLab/Salus).
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "execution/engine/resourcecontext.h"

#include "execution/engine/allocationlistener.h"
#include "utils/containerutils.h"
#include "utils/threadutils.h"

#include <utility>

namespace salus {

ResourceContext::ResourceContext(const ResourceContext &other, const DeviceSpec &spec)
    : resMon(other.resMon)
    , m_graphId(other.m_graphId)
    , m_spec(spec)
    , m_ticket(other.m_ticket)
    , m_hasStaging(false)
    , m_listeners(other.m_listeners)
{
}

ResourceContext::ResourceContext(ResourceMonitor &resMon, uint64_t graphId, const DeviceSpec &spec,
                                 uint64_t ticket)
    : resMon(resMon)
    , m_graphId(graphId)
    , m_spec(spec)
    , m_ticket(ticket)
    , m_hasStaging(true)
{
    CHECK(m_ticket);
}

void ResourceContext::releaseStaging()
{
    bool expected = true;
    if (!m_hasStaging.compare_exchange_strong(expected, false)) {
        return;
    }
    resMon.freeStaging(m_ticket);
}

ResourceContext::~ResourceContext()
{
    releaseStaging();
}

ResourceContext::OperationScope ResourceContext::alloc(ResourceType type) const
{
    OperationScope scope(*this, resMon.lock());

    auto staging = scope.proxy.queryStaging(m_ticket);
    auto num = sstl::optionalGet(staging, {type, m_spec});
    if (!num) {
        return scope;
    }

    scope.res[{type, m_spec}] = *num;
    scope.valid = scope.proxy.allocate(m_ticket, scope.res);

    return scope;
}

ResourceContext::OperationScope ResourceContext::alloc(ResourceType type, size_t num) const
{
    OperationScope scope(*this, resMon.lock());

    scope.res[{type, m_spec}] = num;
    scope.valid = scope.proxy.allocate(m_ticket, scope.res);

    return scope;
}

void ResourceContext::dealloc(ResourceType type, size_t num) const
{
    ResourceTag tag{type, m_spec};
    Resources res{{tag, num}};

    bool last = resMon.free(m_ticket, res);
    for (const auto &l : m_listeners) {
        l->notifyDealloc(m_graphId, ticket(), tag, num, last);
    }
}

void ResourceContext::OperationScope::rollback()
{
    DCHECK(valid);
    valid = false;
    proxy.free(context.ticket(), res);
}

void ResourceContext::OperationScope::commit()
{
    if (!valid) {
        return;
    }

    // the allocation is used by the session (i.e. the session left the scope without rollback)
    for (auto [tag, val] : res) {
        for (const auto &l : context.m_listeners) {
            l->notifyAlloc(context.m_graphId, context.ticket(), tag, val);
        }
    }
}

std::ostream &operator<<(std::ostream &os, const ResourceContext &c)
{
    if (c.ticket() == 0) {
        return os << "AllocationTicket(Invalid)";
    }
    os << "AllocationTicket(" << c.ticket() << ", device=" << c.spec();

#if defined(SALUS_ENABLE_STATIC_STREAM)
    os << ", sess=" << c.sessHandle;
#endif

    os << ")";
    return os;
}

} // namespace salus
