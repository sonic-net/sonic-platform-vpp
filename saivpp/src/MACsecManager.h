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
#pragma once

#include "MACsecAttr.h"
#include "MACsecFilter.h"
#include "MACsecForwarder.h"

namespace saivpp
{
    class MACsecManager
    {
        public:

            MACsecManager();

            virtual ~MACsecManager();

            bool create_macsec_port(
                    _In_ const MACsecAttr &attr);

            bool create_macsec_sc(
                    _In_ const MACsecAttr &attr);

            bool create_macsec_sa(
                    _In_ const MACsecAttr &attr);

            bool delete_macsec_port(
                    _In_ const MACsecAttr &attr);

            bool delete_macsec_sc(
                    _In_ const MACsecAttr &attr);

            bool delete_macsec_sa(
                    _In_ const MACsecAttr &attr);

            bool enable_macsec_filter(
                    _In_ const std::string &macsecInterface,
                    _In_ bool enable);

            bool update_macsec_sa_pn(
                    _In_ const MACsecAttr &attr,
                    _In_ sai_uint64_t pn);

            bool get_macsec_sa_pn(
                    _In_ const MACsecAttr &attr,
                    _Out_ sai_uint64_t &pn) const;

            void cleanup_macsec_device() const;

        protected:

            bool create_macsec_egress_sc(
                    _In_ const MACsecAttr &attr);

            bool create_macsec_ingress_sc(
                    _In_ const MACsecAttr &attr);

            bool create_macsec_egress_sa(
                    _In_ const MACsecAttr &attr);

            bool create_macsec_ingress_sa(
                    _In_ const MACsecAttr &attr);

            bool delete_macsec_egress_sc(
                    _In_ const MACsecAttr &attr);

            bool delete_macsec_ingress_sc(
                    _In_ const MACsecAttr &attr);

            bool delete_macsec_egress_sa(
                    _In_ const MACsecAttr &attr);

            bool delete_macsec_ingress_sa(
                    _In_ const MACsecAttr &attr);

            bool add_macsec_filter(
                    _In_ const std::string &macsecInterface);

            bool add_macsec_forwarder(
                    _In_ const std::string &macsecInterface);

            bool delete_macsec_forwarder(
                    _In_ const std::string &macsecInterface);

            bool add_macsec_manager(
                    _In_ const std::string &macsecInterface,
                    _In_ std::shared_ptr<HostInterfaceInfo> info);

            bool delete_macsec_manager(
                    _In_ const std::string &macsecInterface);

            bool get_macsec_device_info(
                    _In_ const std::string &macsecDevice,
                    _Out_ std::string &info) const;

            bool is_macsec_device_existing(
                    _In_ const std::string &macsecDevice) const;

            bool get_macsec_sc_info(
                    _In_ const std::string &macsecDevice,
                    _In_ sai_int32_t direction,
                    _In_ const std::string &sci,
                    _Out_ std::string &info) const;

            bool is_macsec_sc_existing(
                    _In_ const std::string &macsecDevice,
                    _In_ sai_int32_t direction,
                    _In_ const std::string &sci) const;

            bool get_macsec_sa_info(
                    _In_ const std::string &macsecDevice,
                    _In_ sai_int32_t direction,
                    _In_ const std::string &sci,
                    _In_ macsec_an_t an,
                    _Out_ std::string &info) const;

            bool is_macsec_sa_existing(
                    _In_ const std::string &macsecDevice,
                    _In_ sai_int32_t direction,
                    _In_ const std::string &sci,
                    _In_ macsec_an_t an) const;

            size_t get_macsec_sa_count(
                    _In_ const std::string &macsecDevice,
                    _In_ sai_int32_t direction,
                    _In_ const std::string &sci) const;

            std::string shellquote(
                    _In_ const std::string &str) const;

            virtual bool exec(
                    _In_ const std::string &command,
                    _Out_ std::string &output) const;

            bool exec(
                    _In_ const std::string &command) const;

            struct MACsecTrafficManager
            {
                std::shared_ptr<HostInterfaceInfo> m_info;
                std::shared_ptr<MACsecFilter> m_ingressFilter;
                std::shared_ptr<MACsecFilter> m_egressFilter;
                std::shared_ptr<MACsecForwarder> m_forwarder;
            };

            std::map<std::string, MACsecTrafficManager> m_macsecTrafficManagers;
    };
}
