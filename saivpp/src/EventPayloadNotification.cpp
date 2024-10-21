/*
 * Copyright 2016 Microsoft, Inc.
 * Modifications copyright (c) 2023 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "EventPayloadNotification.h"

#include "swss/logger.h"

using namespace saivpp;

EventPayloadNotification::EventPayloadNotification(
        _In_ std::shared_ptr<sairedis::Notification> ntf,
        _In_ const sai_switch_notifications_t& switchNotifications):
    m_ntf(ntf),
    m_switchNotifications(switchNotifications)
{
    SWSS_LOG_ENTER();

    // empty
}

std::shared_ptr<sairedis::Notification> EventPayloadNotification::getNotification() const
{
    SWSS_LOG_ENTER();

    return m_ntf;
}

const sai_switch_notifications_t& EventPayloadNotification::getSwitchNotifications() const
{
    SWSS_LOG_ENTER();

    return m_switchNotifications;
}
